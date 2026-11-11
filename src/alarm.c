#include "alarm.h"
#include "config.h"
#include "logger.h"

#include <stdio.h>
#include <string.h>

/* 上一次的判定结果，用来做"只在变化时打日志"。
 * 只有 alarm_data 这一个线程会碰它，所以不用加锁 */
static unsigned int g_last_flags = ALARM_NONE;
static int          g_last_level = ALARM_LEVEL_NORMAL;

static void append_flag(char *buf, int cap, const char *item)
{
    size_t n = strlen(buf);

    if (n >= (size_t)cap - 1)
    {
        return;
    }

    if (0 != n)
    {
        snprintf(buf + n, (size_t)cap - n, "|");
        n = strlen(buf);
    }

    snprintf(buf + n, (size_t)cap - n, "%s", item);
}

int alarm_text(unsigned int flags, char *buf, int cap)
{
    if (NULL == buf || cap <= 0)
    {
        return -1;
    }

    buf[0] = '\0';

    if (ALARM_NONE == flags)
    {
        snprintf(buf, (size_t)cap, "正常");
        return 0;
    }

    if (0 != (flags & ALARM_TEMP_HIGH))  { append_flag(buf, cap, "温度过高"); }
    if (0 != (flags & ALARM_HUM_HIGH))   { append_flag(buf, cap, "湿度过高"); }
    if (0 != (flags & ALARM_VOLT_LOW))   { append_flag(buf, cap, "电压偏低"); }
    if (0 != (flags & ALARM_VOLT_HIGH))  { append_flag(buf, cap, "电压偏高"); }
    if (0 != (flags & ALARM_CURR_HIGH))  { append_flag(buf, cap, "电流过大"); }
    if (0 != (flags & ALARM_SMOKE_HIGH)) { append_flag(buf, cap, "烟雾报警"); }

    return 0;
}

const char *alarm_level_name(int level)
{
    switch (level)
    {
        case ALARM_LEVEL_NORMAL: return "正常";
        case ALARM_LEVEL_WARN:   return "警告";
        case ALARM_LEVEL_CRIT:   return "严重";
        default:                 return "未知";
    }
}

/****************************************************************
 * 判一帧
 *
 * 等级怎么定：
 *   欠压 / 过压 / 烟雾  -> 严重（基站断电就是通信中断）
 *   温度 / 湿度 / 电流  -> 警告（先观察，持续高了再加人工干预）
 ****************************************************************/
int alarm_check(struct msgdata *m)
{
    unsigned int flags = ALARM_NONE;
    int          level = ALARM_LEVEL_NORMAL;
    char         text[128];

    if (NULL == m)
    {
        return ALARM_NONE;
    }

    if (m->temp > (float)g_cfg.temp_max)
    {
        flags |= ALARM_TEMP_HIGH;
    }
    if (m->hum > (float)g_cfg.hum_max)
    {
        flags |= ALARM_HUM_HIGH;
    }
    if (m->v < (float)g_cfg.volt_min)
    {
        flags |= ALARM_VOLT_LOW;
    }
    if (m->v > (float)g_cfg.volt_max)
    {
        flags |= ALARM_VOLT_HIGH;
    }
    if (m->a > (float)g_cfg.curr_max)
    {
        flags |= ALARM_CURR_HIGH;
    }
    if (m->smoke > (float)g_cfg.smoke_max)
    {
        flags |= ALARM_SMOKE_HIGH;
    }

    if (0 != (flags & (ALARM_VOLT_LOW | ALARM_VOLT_HIGH | ALARM_SMOKE_HIGH)))
    {
        level = ALARM_LEVEL_CRIT;
    }
    else if (ALARM_NONE != flags)
    {
        level = ALARM_LEVEL_WARN;
    }

    m->alarm = flags;
    m->level = level;

    /* 状态变化才打日志：一直超限的时候每秒一条日志会把文件撑爆 */
    if (flags != g_last_flags)
    {
        (void)alarm_text(flags, text, (int)sizeof(text));

        if (ALARM_NONE == flags)
        {
            LOG_INFO("[告警解除] 各项恢复正常（上一次是 %s）",
                     alarm_level_name(g_last_level));
        }
        else if (ALARM_LEVEL_CRIT == level)
        {
            LOG_ERROR("[严重告警] %s  T=%.1f H=%.1f V=%.2f I=%.2f S=%.0f",
                      text, m->temp, m->hum, m->v, m->a, m->smoke);
        }
        else
        {
            LOG_WARN("[警告] %s  T=%.1f H=%.1f V=%.2f I=%.2f S=%.0f",
                     text, m->temp, m->hum, m->v, m->a, m->smoke);
        }

        g_last_flags = flags;
        g_last_level = level;
    }

    return (int)flags;
}
