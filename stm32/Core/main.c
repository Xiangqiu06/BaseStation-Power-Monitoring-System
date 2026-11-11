#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

#include "system_clock.h"
#include "bsp_uart.h"
#include "bsp_adc.h"
#include "bsp_ds18b20.h"
#include "app_tasks.h"
#include "app_cfg.h"
#include "soc.h"

#include <stdio.h>

/*------------------------------------------------------------------
 * 后备电源检测系统 —— 下位机主程序
 *
 *   ROLE_NODE     电池组节点：采集 + SOC + 告警 + LoRa 上报
 *   ROLE_GATEWAY  网关：LoRa 汇聚 + Modbus RTU 从机（对 i.MX6ULL）
 *
 * 编译时用 -DAPP_ROLE=ROLE_GATEWAY 切换角色，两个设备共用同一套代码。
 *
 * 任务划分（节点）：
 *   sample  prio 4  200ms   读 ADC(DMA) / 2s 读 DS18B20
 *   soc     prio 3  1s      安时积分 + 静置 OCV 校正
 *   alarm   prio 3  事件    队列驱动，逐样本判阈值 + 去抖
 *   lora    prio 2  1s      组帧发送
 *----------------------------------------------------------------*/

extern QueueHandle_t g_q_sample;

/****************************************************************
 * FreeRTOS 钩子
 ****************************************************************/
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;

    taskDISABLE_INTERRUPTS();
    bsp_uart_puts(UART_DEBUG, "\r\n!! STACK OVERFLOW: ");
    bsp_uart_puts(UART_DEBUG, pcTaskName);
    bsp_uart_puts(UART_DEBUG, "\r\n");

    for (;;)
    {
    }
}

void vApplicationMallocFailedHook(void)
{
    taskDISABLE_INTERRUPTS();
    bsp_uart_puts(UART_DEBUG, "\r\n!! MALLOC FAILED (heap 用完了，加大 configTOTAL_HEAP_SIZE)\r\n");

    for (;;)
    {
    }
}

/****************************************************************
 * 任务创建
 ****************************************************************/
static void app_tasks_create(void)
{
#if (APP_ROLE == ROLE_NODE)
    g_q_sample = xQueueCreate(8, sizeof(bat_sample_t));

    if (NULL == g_q_sample)
    {
        bsp_uart_puts(UART_DEBUG, "create queue failed\r\n");
        return;
    }

    soc_init(95.0f);

    /* 先建消费端再建生产端，避免采样任务一上来就把队列塞满 */
    xTaskCreate(vAlarmTask,     "alarm",  256, NULL, 4, NULL);
    xTaskCreate(vSocTask,       "soc",    256, NULL, 3, &g_task_soc);
    xTaskCreate(vLoraReportTask, "lora",  512, NULL, 2, NULL);
    xTaskCreate(vBatSampleTask, "sample", 320, NULL, 5, NULL);
#else
    xTaskCreate(vGatewayTask,   "gateway", 512, NULL, 3, NULL);
#endif
}

int main(void)
{
    char line[96];

    /* 时钟必须在最前面配完：后面所有串口波特率、SysTick 都依赖它 */
    SystemInit();

    bsp_uart_init();
    bsp_ds18b20_init();

#if (APP_ROLE == ROLE_NODE)
    bsp_adc_init();
#endif

    bsp_uart_puts(UART_DEBUG, "\r\n");
    bsp_uart_puts(UART_DEBUG, "================================================\r\n");
    bsp_uart_puts(UART_DEBUG, " BaseStation Backup Power Monitor (STM32F103)\r\n");
    bsp_uart_puts(UART_DEBUG, " FreeRTOS V10.5.1  SysClk=72MHz\r\n");
    sprintf(line, " Role=%s  NodeID=%u\r\n",
            (APP_ROLE == ROLE_NODE) ? "NODE" : "GATEWAY", (unsigned)NODE_ID);
    bsp_uart_puts(UART_DEBUG, line);
    bsp_uart_puts(UART_DEBUG, "================================================\r\n");

    app_tasks_create();

    vTaskStartScheduler();

    /* 正常不会走到这里：调度器启动失败只可能是堆不够 */
    bsp_uart_puts(UART_DEBUG, "scheduler start failed (heap?)\r\n");

    for (;;)
    {
    }
}
