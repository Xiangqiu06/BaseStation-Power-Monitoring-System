#include "app_tasks.h"
#include "bsp_adc.h"
#include "bsp_ds18b20.h"
#include "bsp_uart.h"
#include "soc.h"

#include <stdio.h>
#include <string.h>

QueueHandle_t g_q_sample = NULL;
TaskHandle_t  g_task_soc = NULL;

/****************************************************************
 * 采集任务
 *
 * 电流电压 5Hz 采（DMA 一直在跑，这里只是读最新值 + 滑动平均），
 * 温度 2s 采一次（DS18B20 转换一次要 750ms，采太快没意义还占时间）。
 * 采完打包成 bat_sample_t 丢进队列，同时给 SOC 任务一个通知。
 ****************************************************************/
void vBatSampleTask(void *pv)
{
    TickType_t last_wake = xTaskGetTickCount();
    TickType_t last_temp = 0;
    float      temp_c    = 25.0f;

    (void)pv;

    /* 上电先标定零点：假定此刻电池组没有充放电 */
    bsp_adc_calibrate_zero();
    (void)bsp_ds18b20_read_blocking(&temp_c);

    for (;;)
    {
        bat_sample_t s;
        float        v;
        float        curr;

        /* ---- 电压/电流：16 次平均，滤掉开关电源的高频纹波 ---- */
        v    = bsp_adc_read_bus_volt_v(16);
        curr = bsp_adc_read_current_a(16);

        /* ---- 温度：到点才真正去读，其余时间沿用上次值 ---- */
        if ((xTaskGetTickCount() - last_temp) >= pdMS_TO_TICKS(TEMP_PERIOD_MS))
        {
            float t;

            if (0 == bsp_ds18b20_read_blocking(&t))
            {
                temp_c = t;
                app_set_temp_10((int16_t)(t * 10.0f));
            }
            last_temp = xTaskGetTickCount();
        }

        memset(&s, 0, sizeof(s));
        s.node_id  = NODE_ID;
        s.volt_100 = (uint16_t)(v * 100.0f + 0.5f);
        s.curr_100 = (int16_t)(curr * 100.0f);
        s.temp_10  = (int16_t)(temp_c * 10.0f);
        s.soc_10   = app_soc_10();
        s.soh_10   = 975;               /* 简化为固定值，见 soc.c 的说明 */
        s.alarm    = app_alarm_bits();

        /* 告警任务必须看到每一条样本，队列满了说明处理不过来了 */
        if (pdPASS != xQueueSend(g_q_sample, &s, 0))
        {
            /* 丢一条也不能死等，否则采集节奏就乱了；
             * 真出现这种情况说明告警任务被卡住了，看串口统计就知道 */
        }

        /* SOC 只要最新值，用任务通知最省 */
        if (NULL != g_task_soc)
        {
            xTaskNotifyGive(g_task_soc);
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
    }
}

/****************************************************************
 * SOC 任务
 *
 * 每 1 秒算一次：把这段时间流过的安时数积分进容量里。
 * 这里的 dt 用"两次唤醒的实际时间差"，而不是写死 1，
 * 因为任务可能被更高优先级抢占，写死会累积误差。
 ****************************************************************/
void vSocTask(void *pv)
{
    TickType_t last = xTaskGetTickCount();

    (void)pv;

    for (;;)
    {
        uint32_t notified = 0;
        float    curr;
        float    dt;

        /* 等采集任务通知，最多等 1 秒；等不到也照算（用上次的电流） */
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));

        notified = (uint32_t)(xTaskGetTickCount() - last);
        last     = xTaskGetTickCount();

        dt = (float)notified / 1000.0f;

        if (dt <= 0.0f || dt > 5.0f)
        {
            dt = 1.0f;                  /* 明显不合理就当 1 秒 */
        }

        /* 取最近一次采样的电流：从队列里偷看而不出队太麻烦，
         * 这里用 bsp_adc 直接读，代价只有几微秒 */
        curr = bsp_adc_read_current_a(4);

        soc_integrate(curr, dt);

        /* 静置（|I| < 0.2A）且电压稳定时用 OCV 校正一次，
         * 长期跑下来安时积分会漂，OCV 就是那把尺子 */
        if (curr > -0.2f && curr < 0.2f)
        {
            soc_ocv_correct(bsp_adc_read_bus_volt_v(8));
        }

    }
}

/****************************************************************
 * 告警任务
 *
 * 状态机：NORMAL -> WARN -> FAULT -> (恢复) -> NORMAL
 * 每一路越限都带计数去抖：连续 ALARM_DEBOUNCE_CNT 次才真正置位，
 * 恢复也要连续同样次数才清。只做单点比较的话，
 * 大电流启停那一下的尖峰就会天天误报。
 ****************************************************************/
void vAlarmTask(void *pv)
{
    uint16_t bits      = 0;
    uint8_t  cnt[7]    = {0};           /* 每路自己的去抖计数 */
    char     line[96];

    (void)pv;

    for (;;)
    {
        bat_sample_t s;
        float        v;
        float        i;
        float        t;
        uint16_t     new_bits = 0;

        if (pdPASS != xQueueReceive(g_q_sample, &s, portMAX_DELAY))
        {
            continue;
        }

        v = (float)s.volt_100 / 100.0f;
        i = (float)s.curr_100 / 100.0f;
        t = (float)s.temp_10 / 10.0f;

        /* 逐路判定 + 去抖 */
        {
            struct
            {
                int      idx;
                int      over;
                uint16_t bit;
            } chk[5];

            chk[0].idx = 0; chk[0].over = (v < TH_VOLT_LOW)  ? 1 : 0; chk[0].bit = AL_BIT_VOLT_LOW;
            chk[1].idx = 1; chk[1].over = (v > TH_VOLT_HIGH) ? 1 : 0; chk[1].bit = AL_BIT_VOLT_HIGH;
            chk[2].idx = 2; chk[2].over = (i > TH_CURR_HIGH) ? 1 : 0; chk[2].bit = AL_BIT_CURR_HIGH;
            chk[3].idx = 3; chk[3].over = (t > TH_TEMP_HIGH) ? 1 : 0; chk[3].bit = AL_BIT_TEMP_HIGH;
            chk[4].idx = 4; chk[4].over = (t < TH_TEMP_LOW)  ? 1 : 0; chk[4].bit = AL_BIT_TEMP_LOW;

            for (uint8_t k = 0; k < 5; k++)
            {
                if (chk[k].over)
                {
                    if (cnt[chk[k].idx] < ALARM_DEBOUNCE_CNT)
                    {
                        cnt[chk[k].idx]++;
                    }
                    if (cnt[chk[k].idx] >= ALARM_DEBOUNCE_CNT)
                    {
                        new_bits |= chk[k].bit;
                    }
                }
                else
                {
                    if (cnt[chk[k].idx] > 0)
                    {
                        cnt[chk[k].idx]--;
                    }
                    if (cnt[chk[k].idx] > 0)
                    {
                        new_bits |= chk[k].bit;      /* 还没退干净，保持 */
                    }
                }
            }
        }

        if (new_bits != bits)
        {
            bits = new_bits;

            snprintf(line, sizeof(line),
                     "[ALARM] node=%u bits=0x%02X V=%.2f I=%.2f T=%.1f\r\n",
                     NODE_ID, bits, (double)v, (double)i, (double)t);
            bsp_uart_puts(UART_DEBUG, line);
        }

        /* 把最新的告警位写回全局，采样任务打包上报时会带上 */
        app_set_alarm_bits(bits);
    }
}
