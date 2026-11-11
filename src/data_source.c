#include "data_source.h"
#include "protocol.h"
#include "serial_port.h"
#include "modbus.h"
#include "config.h"
#include "logger.h"
#include "mailbox.h"          /* 借 g_running 判断要不要收工 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <unistd.h>
#include <errno.h>

/*------------------------------------------------------------------
 * 数据源：三种模式，对上层完全一样
 *
 *   sim                 内置模拟源（5 分钟工况循环），不接硬件也能跑通全链路
 *   /dev/ttymxc2        AA55 自定义帧（下位机直连，单节点）
 *   modbus:/dev/ttymxc2 Modbus RTU 主机，轮询 LoRa 网关下的多个电池组节点
 *
 * 之所以保留模拟源：接真机前先把告警、落库、上报、界面全部验一遍，
 * 现场调试时就只剩"串口通不通"一个问题要查。
 *----------------------------------------------------------------*/

#define SIM_CYCLE_TICKS     300      /* 一个完整工况的周期数（1Hz 即 5 分钟） */
#define SIM_OUTAGE_START    240      /* 第 240 拍开始"市电掉电" */
#define SIM_OUTAGE_END      300

#define SRC_MODE_SIM        0
#define SRC_MODE_FRAME      1
#define SRC_MODE_MODBUS     2

static int                 g_inited  = 0;
static int                 g_mode    = SRC_MODE_SIM;
static int                 g_fd      = -1;
static unsigned int        g_tick    = 0;
static struct frame_parser g_parser;
static unsigned long       g_err_cnt = 0;

int data_source_is_sim(void)
{
    return (SRC_MODE_SIM == g_mode) ? 1 : 0;
}

unsigned long data_source_err_count(void)
{
    if (SRC_MODE_MODBUS == g_mode)
    {
        return modbus_err_count();
    }

    return g_err_cnt;
}

/****************************************************************
 * 模拟源：造一条像样的曲线
 *
 * 电压  ：市电正常时在 48.5V 上下小幅波动；进入掉电段后电池放电，
 *         电压线性往下掉到 47.2V，同时电流从 2A 抬到 4A 左右，
 *         温度因为大电流放热慢慢升高 —— 这一段正好能把欠压告警跑出来
 * 湿度  ：缓慢正弦，模拟昼夜变化
 * 烟雾  ：平时几十 ppm，每隔 97 拍尖峰一次，用来触发烟雾告警
 * SOC   ：跟着放电过程往下掉
 ****************************************************************/
static void sim_generate(struct msgdata *m)
{
    unsigned int t      = g_tick % SIM_CYCLE_TICKS;
    int          in_out = (t >= SIM_OUTAGE_START && t < SIM_OUTAGE_END) ? 1 : 0;
    double       phase;

    g_tick++;
    phase = (double)t;

    if (in_out)
    {
        double ratio = (double)(t - SIM_OUTAGE_START) /
                       (double)(SIM_OUTAGE_END - SIM_OUTAGE_START);

        m->v   = (float)(48.45 - 1.25 * ratio);
        m->a   = (float)(2.05 + 2.15 * ratio);
        m->soc = (float)(92.0 - 6.0 * ratio);
    }
    else
    {
        m->v   = (float)(48.50 + 0.18 * sin(phase / 11.0));
        m->a   = (float)(2.00 + 0.25 * sin(phase / 7.0));
        m->soc = (float)(92.0 + 4.0 * (phase / (double)SIM_OUTAGE_START));
    }

    m->temp  = (float)(24.0 + 6.0 * sin(phase / 43.0) + (m->a - 2.0) * 4.0);
    m->hum   = (float)(46.0 + 14.0 * sin(phase / 71.0));
    m->smoke = (float)(30 + 20.0 * sin(phase / 5.0));
    m->soh   = 97.5f;

    if (0 == (g_tick % 97))
    {
        m->smoke = 420.0f;              /* 偶尔来一下，验证烟雾告警链路 */
    }

    m->id     = g_cfg.dev_id;
    m->bat_id = 1;
    m->alarm  = 0;
    m->level  = 0;
}

/****************************************************************
 * 睡 ms 毫秒，但每 100ms 醒一次看要不要退出
 * 直接 sleep(1) 的话 Ctrl+C 之后最多要等 1 秒，日志看着像卡住了
 ****************************************************************/
static void sim_sleep(int ms)
{
    int left = ms;

    while (left > 0 && g_running)
    {
        int step = (left > 100) ? 100 : left;

        usleep((useconds_t)step * 1000u);
        left -= step;
    }
}

/****************************************************************
 * 帧模式：从串口一路收到一个完整 ENV 帧为止
 ****************************************************************/
static int frame_read(struct msgdata *out)
{
    unsigned char buf[128];

    while (g_running)
    {
        int n = serial_port_read(g_fd, buf, (int)sizeof(buf), 200);
        int i;

        if (n < 0)
        {
            LOG_ERROR("读串口失败");
            return -1;
        }
        if (0 == n)
        {
            continue;                   /* 200ms 没数据，正常，下位机是 1Hz */
        }

        for (i = 0; i < n; i++)
        {
            int ret = protocol_feed(&g_parser, buf[i], out);

            if (PROTO_ENV == ret)
            {
                out->id = g_cfg.dev_id;
                return 0;
            }

            if (ret < 0)
            {
                g_err_cnt++;

                if (0 == (g_err_cnt % 50))
                {
                    LOG_WARN("串口收帧异常累计 %lu 次（CRC错 %lu / 长度错 %lu）",
                             g_err_cnt, g_parser.crc_err, g_parser.len_err);
                }
            }
        }
    }

    return -1;
}

int data_source_init(const char *dev, int baud)
{
    const char *p = dev;

    g_tick    = 0;
    g_err_cnt = 0;
    protocol_parser_init(&g_parser);

    if (NULL == p || '\0' == p[0] || 0 == strcmp(p, CONFIG_SIM_TAG))
    {
        g_mode   = SRC_MODE_SIM;
        g_inited = 1;
        LOG_INFO("[data_source] 内置模拟源，每 %d ms 一拍，%d 拍走完一个完整工况",
                 g_cfg.sample_period_ms, (int)SIM_CYCLE_TICKS);
        return 0;
    }

    /* ---------- Modbus RTU 主机 ---------- */
    if (0 == strncmp(p, "modbus:", 7))
    {
        if (0 != modbus_init(p, baud, g_cfg.slave_count))
        {
            return -1;
        }

        g_mode   = SRC_MODE_MODBUS;
        g_inited = 1;
        LOG_INFO("[data_source] Modbus RTU 主机模式，轮询 %d 个电池组节点",
                 g_cfg.slave_count);
        return 0;
    }

    /* ---------- 自定义帧模式 ---------- */
    g_fd = serial_port_open(p, baud);
    if (g_fd < 0)
    {
        return -1;
    }

    g_mode   = SRC_MODE_FRAME;
    g_inited = 1;
    LOG_INFO("[data_source] 串口 %s 已打开，%d 8N1，AA55 帧模式", p, baud);

    return 0;
}

int data_source_read(struct msgdata *out)
{
    if (0 == g_inited || NULL == out)
    {
        return -1;
    }

    if (SRC_MODE_FRAME == g_mode)
    {
        return frame_read(out);
    }

    if (SRC_MODE_MODBUS == g_mode)
    {
        int ret    = modbus_poll_next(out);
        int slaves = (g_cfg.slave_count > 0) ? g_cfg.slave_count : 1;

        /* 一轮 N 个节点平均分掉采样周期，整体还是 1Hz；
         * 某个节点超时也不阻塞后面的节点 */
        sim_sleep(g_cfg.sample_period_ms / slaves);

        return ret;
    }

    /* ---- 模拟源 ---- */
    sim_sleep(g_cfg.sample_period_ms);
    if (!g_running)
    {
        return -1;
    }

    {
        unsigned char  frame[FRAME_MAX_LEN];
        struct msgdata raw;
        int            flen;
        int            i;

        memset(&raw, 0, sizeof(raw));
        sim_generate(&raw);

        /* 先按协议组帧，再走解析器解回来：
         * 这样模拟跑通就等于协议跑通，联调前先把最容易出错的那环验掉 */
        flen = protocol_build_env(&raw, frame, (int)sizeof(frame));
        if (flen < 0)
        {
            return -1;
        }

        protocol_parser_init(&g_parser);

        for (i = 0; i < flen; i++)
        {
            int ret = protocol_feed(&g_parser, frame[i], out);

            if (PROTO_ENV == ret)
            {
                /* 协议帧里没有 SOC/SOH，这两个是模拟源额外补的 */
                out->soc    = raw.soc;
                out->soh    = raw.soh;
                out->bat_id = raw.bat_id;
                return 0;
            }
        }
    }

    LOG_WARN("模拟源出帧失败");
    return -1;
}

void data_source_close(void)
{
    if (SRC_MODE_MODBUS == g_mode)
    {
        modbus_close();
    }
    else if (g_fd >= 0)
    {
        serial_port_close(g_fd);
    }

    g_fd     = -1;
    g_inited = 0;

    LOG_INFO("[data_source] 已关闭，累计收帧错误 %lu 次", data_source_err_count());
}
