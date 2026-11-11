#include "modbus.h"
#include "serial_port.h"
#include "config.h"
#include "logger.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

/*------------------------------------------------------------------
 * Modbus RTU 主机实现
 *
 * 帧格式（读保持寄存器 0x03）：
 *   请求  [addr][03][reg_h][reg_l][cnt_h][cnt_l][crc_l][crc_h]        8 字节
 *   应答  [addr][03][byte_cnt][data ...][crc_l][crc_h]       5 + 2N 字节
 *   CRC  ：CRC-16/MODBUS，多项式 0xA001（反向），初值 0xFFFF，低字节在前
 *
 * 现场注意：
 *   - 从机是 LoRa 转发的，一帧来回要几十到几百毫秒，超时给到 1000ms；
 *   - LoRa 是半双工，发完必须等应答，所以这里严格"一问一答"，不做并发。
 *----------------------------------------------------------------*/

#define MB_TIMEOUT_MS       1000    /* 等应答，LoRa 链路比有线慢得多 */
#define MB_FRAME_MAX        64

static int            g_fd        = -1;
static int            g_slave_cnt = 0;
static int            g_cur_slave = 1;
static unsigned long  g_ok_cnt    = 0;
static unsigned long  g_err_cnt   = 0;

unsigned long modbus_ok_count(void)
{
    return g_ok_cnt;
}

unsigned long modbus_err_count(void)
{
    return g_err_cnt;
}

/****************************************************************
 * CRC-16/MODBUS
 ****************************************************************/
unsigned short modbus_crc16(const unsigned char *buf, int len)
{
    unsigned short crc = 0xFFFF;
    int            i;
    int            b;

    if (NULL == buf || len <= 0)
    {
        return 0;
    }

    for (i = 0; i < len; i++)
    {
        crc ^= (unsigned short)buf[i];
        for (b = 0; b < 8; b++)
        {
            if (0 != (crc & 0x0001))
            {
                crc = (unsigned short)((crc >> 1) ^ 0xA001);
            }
            else
            {
                crc = (unsigned short)(crc >> 1);
            }
        }
    }

    return crc;
}

int modbus_init(const char *dev, int baud, int slaves)
{
    char tag[64];

    if (NULL == dev || '\0' == dev[0])
    {
        LOG_ERROR("[modbus] 没配串口设备");
        return -1;
    }

    /* 允许写 "modbus:/dev/ttymxc2" 这种带前缀的写法，取冒号后面 */
    if (0 == strncmp(dev, "modbus:", 7))
    {
        snprintf(tag, sizeof(tag), "%s", dev + 7);
    }
    else
    {
        snprintf(tag, sizeof(tag), "%s", dev);
    }

    if (slaves < 1)
    {
        slaves = 1;
    }
    if (slaves > 247)
    {
        slaves = 247;
    }

    g_fd = serial_port_open(tag, baud);
    if (g_fd < 0)
    {
        return -1;
    }

    g_slave_cnt = slaves;
    g_cur_slave = 1;
    g_ok_cnt    = 0;
    g_err_cnt   = 0;

    LOG_INFO("[modbus] 主机就绪 %s %d 8N1，从机 1~%d，每轮读 %d 个寄存器",
             tag, baud, slaves, MODBUS_REG_COUNT);

    return 0;
}

void modbus_close(void)
{
    if (g_fd >= 0)
    {
        serial_port_close(g_fd);
        g_fd = -1;
    }

    LOG_INFO("[modbus] 已关闭，成功 %lu 次，失败 %lu 次", g_ok_cnt, g_err_cnt);
}

/****************************************************************
 * 收一帧应答：按期望长度收，超时按绝对时间算
 ****************************************************************/
static int mb_recv(unsigned char *buf, int want, int timeout_ms)
{
    struct timeval t0;
    int            got = 0;

    gettimeofday(&t0, NULL);

    while (got < want)
    {
        struct timeval cur;
        int            elapsed;
        int            remain;
        int            n;

        gettimeofday(&cur, NULL);
        elapsed = (int)((cur.tv_sec - t0.tv_sec) * 1000 +
                        (cur.tv_usec - t0.tv_usec) / 1000);
        remain = timeout_ms - elapsed;

        if (remain <= 0)
        {
            break;
        }
        if (remain > 100)
        {
            remain = 100;               /* 分片等，主线程退出时能及时跳出 */
        }

        n = serial_port_read(g_fd, buf + got, want - got, remain);
        if (n < 0)
        {
            return -1;
        }
        if (n > 0)
        {
            got += n;
        }
    }

    return got;
}

int modbus_read_slave(int slave, struct msgdata *out)
{
    unsigned char req[8];
    unsigned char rsp[MB_FRAME_MAX];
    unsigned short crc;
    int            want;
    int            n;
    int            i;
    time_t         now;

    if (g_fd < 0 || NULL == out)
    {
        return -1;
    }
    if (slave < 1 || slave > 247)
    {
        return -1;
    }

    /* 组请求 */
    req[0] = (unsigned char)slave;
    req[1] = 0x03;
    req[2] = 0x00;
    req[3] = 0x00;                      /* 起始寄存器 0 */
    req[4] = 0x00;
    req[5] = (unsigned char)MODBUS_REG_COUNT;
    crc    = modbus_crc16(req, 6);
    req[6] = (unsigned char)(crc & 0xFF);
    req[7] = (unsigned char)((crc >> 8) & 0xFF);

    serial_port_flush(g_fd);

    if (0 != serial_port_write(g_fd, req, 8))
    {
        g_err_cnt++;
        LOG_WARN("[modbus] 写串口失败（从机 %d）", slave);
        return -1;
    }

    /* 正常应答长度 = 1(addr) + 1(cmd) + 1(cnt) + 2N + 2(crc) */
    want = 5 + MODBUS_REG_COUNT * 2;
    n = mb_recv(rsp, want, MB_TIMEOUT_MS);

    if (n != want)
    {
        g_err_cnt++;
        LOG_WARN("[modbus] 从机 %d 应答长度异常: 期望 %d 实收 %d", slave, want, n);

        /* 从机发回异常码（0x83）时至少打出来，方便判断是地址错还是寄存器错 */
        if (n >= 5 && 0 != (rsp[1] & 0x80))
        {
            LOG_WARN("[modbus] 从机 %d 返回异常码 0x%02X", slave, rsp[2]);
        }
        return -1;
    }

    /* 校验地址、功能码、CRC */
    if (rsp[0] != (unsigned char)slave || rsp[1] != 0x03 ||
        rsp[2] != (unsigned char)(MODBUS_REG_COUNT * 2))
    {
        g_err_cnt++;
        LOG_WARN("[modbus] 从机 %d 应答头不对: addr=%d cmd=0x%02X cnt=%d",
                 slave, rsp[0], rsp[1], rsp[2]);
        return -1;
    }

    crc = modbus_crc16(rsp, want - 2);
    if (rsp[want - 2] != (unsigned char)(crc & 0xFF) ||
        rsp[want - 1] != (unsigned char)((crc >> 8) & 0xFF))
    {
        g_err_cnt++;
        LOG_WARN("[modbus] 从机 %d CRC 校验失败", slave);
        return -1;
    }

    /* 拆寄存器 */
    {
        unsigned short reg[MODBUS_REG_COUNT];

        for (i = 0; i < MODBUS_REG_COUNT; i++)
        {
            reg[i] = (unsigned short)((rsp[3 + i * 2] << 8) | rsp[4 + i * 2]);
        }

        memset(out, 0, sizeof(*out));
        out->id      = g_cfg.dev_id;
        out->bat_id  = slave;
        out->v       = (float)reg[MB_REG_VOLT] / 100.0f;
        out->a       = (float)reg[MB_REG_CURR] / 100.0f;
        out->temp    = (float)(short)reg[MB_REG_TEMP] / 10.0f;
        out->soc     = (float)reg[MB_REG_SOC] / 10.0f;
        out->soh     = (float)reg[MB_REG_SOH] / 10.0f;
        out->smoke   = (float)reg[MB_REG_SMOKE];
        out->alarm   = reg[MB_REG_ALARM];
        out->hum     = 0.0f;            /* 电池柜没装湿度传感器，UI 上显示 "--" */

        /* 节点号寄存器可信的话以它为准，方便现场改地址后对账 */
        if (reg[MB_REG_NODE] > 0 && reg[MB_REG_NODE] < 248)
        {
            out->bat_id = (int)reg[MB_REG_NODE];
        }
    }

    now = time(NULL);
    {
        struct tm tmv;
        struct tm *ptm = localtime(&now);
        if (NULL != ptm)
        {
            tmv = *ptm;
            out->tim = tmv;
        }
    }

    g_ok_cnt++;

    return 0;
}

int modbus_poll_next(struct msgdata *out)
{
    int slave = g_cur_slave;
    int ret;

    /* 轮转地址：一次只问一个节点，问完切下一个，
     * 这样 N 个节点共享一条 LoRa 链路也不会互相撞包 */
    g_cur_slave++;
    if (g_cur_slave > g_slave_cnt)
    {
        g_cur_slave = 1;
    }

    ret = modbus_read_slave(slave, out);

    return ret;
}
