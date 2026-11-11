#include "system_clock.h"
#include "stm32f1xx.h"

uint32_t SystemCoreClock = SYSTEM_CORE_CLOCK_HZ;

/*------------------------------------------------------------------
 * 时钟树配置（不用 HAL，直接写寄存器，方便排查）
 *
 *   HSE 8MHz（板载晶振）
 *     -> PLL x9 = 72MHz  系统时钟
 *     -> AHB  /1 = 72MHz
 *     -> APB2 /1 = 72MHz  （USART1、SPI1、GPIO 挂这儿）
 *     -> APB1 /2 = 36MHz  （USART2、TIM2/3/4 挂这儿，APB1 最高只能 36M）
 *     -> ADC  /6 = 12MHz  （ADC 时钟不能超过 14MHz，这是最容易踩的坑）
 *
 * Flash 在 48MHz 以上必须插 2 个等待周期，否则取指会错。
 *----------------------------------------------------------------*/
void SystemInit(void)
{
    /* 1. 打开 HSE 并等它稳定（外部晶振起振要几百微秒到几毫秒） */
    RCC->CR |= RCC_CR_HSEON;

    {
        volatile uint32_t guard = 0;

        while (0 == (RCC->CR & RCC_CR_HSERDY))
        {
            if (++guard > 0x50000)
            {
                /* 晶振没起来：留在 HSI 8MHz 上跑，至少串口还能出字，
                 * 这时候量一下晶振和负载电容就知道问题了 */
                SystemCoreClock = 8000000UL;
                return;
            }
        }
    }

    /* 2. Flash 等待周期 = 2（72MHz 必须） */
    FLASH->ACR = FLASH_ACR_PRFTBE | FLASH_ACR_LATENCY_2;

    /* 3. 分频：AHB=1, APB1=2, APB2=1, ADC=6 */
    RCC->CFGR &= ~(RCC_CFGR_HPRE | RCC_CFGR_PPRE1 | RCC_CFGR_PPRE2 | RCC_CFGR_ADCPRE);
    RCC->CFGR |= RCC_CFGR_HPRE_DIV1;
    RCC->CFGR |= RCC_CFGR_PPRE1_DIV2;
    RCC->CFGR |= RCC_CFGR_PPRE2_DIV1;
    RCC->CFGR |= RCC_CFGR_ADCPRE_DIV6;

    /* 4. PLL 源 = HSE，倍频 x9 */
    RCC->CFGR &= ~(RCC_CFGR_PLLSRC | RCC_CFGR_PLLXTPRE | RCC_CFGR_PLLMULL);
    RCC->CFGR |= RCC_CFGR_PLLSRC;                 /* 不分频的 HSE 进 PLL */
    RCC->CFGR |= RCC_CFGR_PLLMULL9;

    /* 5. 开 PLL，等锁定 */
    RCC->CR |= RCC_CR_PLLON;
    while (0 == (RCC->CR & RCC_CR_PLLRDY))
    {
    }

    /* 6. 切换系统时钟到 PLL */
    RCC->CFGR &= ~RCC_CFGR_SW;
    RCC->CFGR |= RCC_CFGR_SW_PLL;
    while (RCC_CFGR_SWS != (RCC->CFGR & RCC_CFGR_SWS))
    {
    }

    SystemCoreClock = SYSTEM_CORE_CLOCK_HZ;

    /* 7. 向量表搬到 Flash 起始（Bootloader 跳转过来时这步很关键） */
    SCB->VTOR = FLASH_BASE;
}

/****************************************************************
 * 毫秒忙等
 * 72MHz 下大概 72 个空转 ≈ 1us，这里按 1/8 保守估算
 ****************************************************************/
void delay_ms(uint32_t ms)
{
    while (ms--)
    {
        for (volatile uint32_t i = 0; i < 9000; i++)
        {
            __NOP();
        }
    }
}
