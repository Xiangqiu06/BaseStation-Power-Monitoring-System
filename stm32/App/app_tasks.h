#ifndef __APP_TASKS_H__
#define __APP_TASKS_H__

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "app_cfg.h"

/*------------------------------------------------------------------
 * 三个任务之间的数据流
 *
 *   vBatSampleTask ──队列──▶ vAlarmTask
 *        │  每 200ms 采一次电流电压，2s 采一次温度
 *        │
 *        └──任务通知──▶ vSocTask
 *                        每 1s 用累积的安时数算一次 SOC
 *
 * 为什么采集和 SOC 分开：SOC 要做积分，它关心的是"这一秒流过了多少电荷"，
 * 与采样频率无关；分开以后采样率可以随便调，SOC 逻辑不用动。
 *
 * 为什么告警走队列、SOC 走任务通知：
 *   告警需要"每一条样本都要看"，丢了会漏报 → 队列（深度足够，满了报警）
 *   SOC 只关心最新值，丢几条无所谓 → 任务通知（零拷贝、最轻量）
 *----------------------------------------------------------------*/

extern QueueHandle_t g_q_sample;        /* bat_sample_t */
extern TaskHandle_t  g_task_soc;

/* 采集任务 */
void vBatSampleTask(void *pv);

/* SOC 任务 */
void vSocTask(void *pv);

/* 告警任务 */
void vAlarmTask(void *pv);

/* 节点：LoRa 上报任务 */
void vLoraReportTask(void *pv);

/* 网关：LoRa 接收 + Modbus 从机应答 */
void vGatewayTask(void *pv);

/* ---- 任务间共享的标量（实现在 app_state.c，单字读写天然原子） ---- */
uint16_t app_alarm_bits(void);
void     app_set_alarm_bits(uint16_t bits);
int16_t  app_temp_10(void);
void     app_set_temp_10(int16_t t);
uint16_t app_soc_10(void);

/* 打印任务统计（调试用，串口输出） */
void app_print_stats(void);

#endif
