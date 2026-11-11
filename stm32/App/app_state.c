#include "app_tasks.h"
#include "soc.h"

/*------------------------------------------------------------------
 * 任务间共享的几个量
 *
 * 只放"一个写、多个读"的简单标量，读写都是单条指令，
 * Cortex-M3 上天然原子，不需要加锁/临界区。
 * 需要成块数据一致性的（比如采样 7 个字段）才用队列。
 *----------------------------------------------------------------*/

static volatile uint16_t g_alarm_bits = 0;
static volatile int16_t  g_temp_10    = 250;    /* 25.0℃，还没读到就先用这个 */

uint16_t app_alarm_bits(void)
{
    return g_alarm_bits;
}

void app_set_alarm_bits(uint16_t bits)
{
    g_alarm_bits = bits;
}

int16_t app_temp_10(void)
{
    return g_temp_10;
}

void app_set_temp_10(int16_t t)
{
    g_temp_10 = t;
}

uint16_t app_soc_10(void)
{
    return soc_get_10();
}
