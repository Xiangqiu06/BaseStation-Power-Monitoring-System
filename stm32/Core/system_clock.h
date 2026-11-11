#ifndef __SYSTEM_CLOCK_H__
#define __SYSTEM_CLOCK_H__

#include <stdint.h>

/* 系统主频，FreeRTOS 的 configCPU_CLOCK_HZ 也是 72M，两者必须一致 */
#define SYSTEM_CORE_CLOCK_HZ    72000000UL

extern uint32_t SystemCoreClock;

/* 配时钟：HSE 8MHz -> PLL x9 -> 72MHz，APB1 = 36MHz，APB2 = 72MHz */
void SystemInit(void);

/* 毫秒级忙等，只在调度器启动前用（之后一律用 vTaskDelay） */
void delay_ms(uint32_t ms);

#endif
