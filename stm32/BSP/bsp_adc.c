#include "bsp_adc.h"
#include "stm32f1xx.h"
#include "system_clock.h"

/* DMA 目标缓冲：两路交替，DMA 一直往里写，CPU 只读 */
static volatile uint16_t g_adc_dma[2];

static float g_zero_mv = ACS712_ZERO_MV;

void bsp_adc_init(void)
{
    /* ---------- 1. GPIO 配成模拟输入 ---------- */
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN;
    GPIOA->CRL &= ~(0xFFu << 0);        /* PA0 / PA1 清成模拟输入 */

    /* ---------- 2. 时钟：ADC 分频在 SystemInit 里已设 /6 = 12MHz ---------- */
    RCC->APB2ENR |= RCC_APB2ENR_ADC1EN;
    RCC->AHBENR  |= RCC_AHBENR_DMA1EN;

    /* ---------- 3. DMA1 通道 1 <- ADC1 ---------- */
    DMA1_Channel1->CCR = 0;             /* 先关掉再配 */
    DMA1_Channel1->CPAR = (uint32_t)&ADC1->DR;
    DMA1_Channel1->CMAR = (uint32_t)g_adc_dma;
    DMA1_Channel1->CNDTR = 2;
    DMA1_Channel1->CCR = DMA_CCR_MINC          /* 内存地址自增 */
                       | DMA_CCR_MSIZE_0       /* 16 位 */
                       | DMA_CCR_PSIZE_0       /* 16 位 */
                       | DMA_CCR_CIRC          /* 循环模式 */
                       | DMA_CCR_PL_1;         /* 高优先级 */
    DMA1_Channel1->CCR |= DMA_CCR_EN;

    /* ---------- 4. ADC1: 连续 + 扫描 + DMA ---------- */
    ADC1->CR1 = ADC_CR1_SCAN;
    ADC1->CR2 = ADC_CR2_CONT | ADC_CR2_DMA | ADC_CR2_ADON;

    /* 采样时间 55.5 个周期：分压电阻阻值大，采样太短会测不准 */
    ADC1->SMPR2 = (0x5u << 0) | (0x5u << 3);

    /* 规则组：通道 0 第 1 个，通道 1 第 2 个 */
    ADC1->SQR1 = 0;                     /* L = 0 表示 1 个转换，下面改成 2 */
    ADC1->SQR1 = (1u << 20);            /* L = 1 → 2 个转换 */
    ADC1->SQR3 = (0u << 0) | (1u << 5);

    /* 上电后先校准一次，不然零点是飘的 */
    ADC1->CR2 |= ADC_CR2_RSTCAL;
    while (0 != (ADC1->CR2 & ADC_CR2_RSTCAL))
    {
    }

    ADC1->CR2 |= ADC_CR2_CAL;
    while (0 != (ADC1->CR2 & ADC_CR2_CAL))
    {
    }

    /* 再置一次 ADON 才真正开始转换（F1 的经典坑：第一次只是唤醒） */
    ADC1->CR2 |= ADC_CR2_ADON;

    delay_ms(5);
}

uint16_t bsp_adc_raw_current(void)
{
    return g_adc_dma[0];
}

uint16_t bsp_adc_raw_volt(void)
{
    return g_adc_dma[1];
}

void bsp_adc_set_zero_mv(float mv)
{
    g_zero_mv = mv;
}

/****************************************************************
 * 零点标定
 *
 * 思路：上电瞬间假定电池组没有充放电电流，
 * 连续采 32 次取平均作为 ACS712 的实际零点。
 * 模块之间的零点离散能到 ±100mV，不标定的话小电流全测不准。
 ****************************************************************/
void bsp_adc_calibrate_zero(void)
{
    uint32_t sum = 0;
    uint8_t  i;

    delay_ms(20);

    for (i = 0; i < 32; i++)
    {
        sum += g_adc_dma[0];
        delay_ms(2);
    }

    g_zero_mv = ((float)sum / 32.0f) * ADC_VREF_MV / ADC_FULL_SCALE;
}

float bsp_adc_read_current_a(uint8_t samples)
{
    uint32_t sum = 0;
    uint8_t  i;
    float    mv;

    if (0 == samples)
    {
        samples = 1;
    }

    for (i = 0; i < samples; i++)
    {
        sum += g_adc_dma[0];
    }

    mv = ((float)sum / (float)samples) * ADC_VREF_MV / ADC_FULL_SCALE;

    /* 电流方向：正 = 充电（传感器输出高于零点） */
    return (mv - g_zero_mv) / ACS712_SENS_MV_A;
}

float bsp_adc_read_bus_volt_v(uint8_t samples)
{
    uint32_t sum = 0;
    uint8_t  i;
    float    mv;

    if (0 == samples)
    {
        samples = 1;
    }

    for (i = 0; i < samples; i++)
    {
        sum += g_adc_dma[1];
    }

    mv = ((float)sum / (float)samples) * ADC_VREF_MV / ADC_FULL_SCALE;

    return mv * VOLT_DIV_RATIO / 1000.0f;
}
