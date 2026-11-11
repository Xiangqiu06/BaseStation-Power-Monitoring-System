#ifndef __BSP_ADC_H__
#define __BSP_ADC_H__

#include <stdint.h>

/*------------------------------------------------------------------
 * ADC1 + DMA1_Channel1 双通道循环采集
 *
 *   PA0 / ADC_IN0  ACS712 霍尔电流传感器输出
 *   PA1 / ADC_IN1  电池电压经 100k/10k 分压
 *
 * 为什么用 DMA：ADC 连续扫描模式下每 1us 就产生一次转换结果，
 * 用中断读会占掉大量 CPU；配成 DMA 循环搬运，CPU 只管读最新值。
 *----------------------------------------------------------------*/

/* 硬件参数，换板子改这里就够了 */
#define ADC_VREF_MV         3300.0f
#define ADC_FULL_SCALE      4095.0f
#define ACS712_SENS_MV_A    66.0f     /* ACS712-30A：66mV/A */
#define ACS712_ZERO_MV      2500.0f   /* 无电流时输出 VCC/2 */
#define VOLT_DIV_RATIO      11.0f     /* 分压比 (100k+10k)/10k */

void     bsp_adc_init(void);

/* 零点标定：上电时认为没有电流，把当前读数当作零点偏移存下来。
 * 现场如果上电就在充放电，这一步会把偏移标歪，所以还留了手动接口 */
void     bsp_adc_calibrate_zero(void);
void     bsp_adc_set_zero_mv(float mv);

/* 读一次（内部做 8 次滑动平均） */
float    bsp_adc_read_current_a(uint8_t samples);
float    bsp_adc_read_bus_volt_v(uint8_t samples);

/* 直接读原始码值，调试用 */
uint16_t bsp_adc_raw_current(void);
uint16_t bsp_adc_raw_volt(void);

#endif
