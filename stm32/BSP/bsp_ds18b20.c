#include "bsp_ds18b20.h"
#include "stm32f1xx.h"
#include "system_clock.h"

#include <stddef.h>

/* PB12 单总线 */
#define DQ_PORT     GPIOB
#define DQ_PIN      12
#define DQ_HIGH()   (DQ_PORT->BSRR = (1u << DQ_PIN))
#define DQ_LOW()    (DQ_PORT->BRR = (1u << DQ_PIN))
#define DQ_READ()   ((DQ_PORT->IDR & (1u << DQ_PIN)) ? 1 : 0)

/* DS18B20 命令 */
#define CMD_SKIP_ROM    0xCC
#define CMD_CONVERT_T   0x44
#define CMD_READ_SCRATCH 0xBE

/****************************************************************
 * DWT 周期计数器延时
 * 72MHz 下一个周期约 13.9ns，比空循环精确得多
 ****************************************************************/
void bsp_delay_us(uint32_t us)
{
    uint32_t start = DWT->CYCCNT;
    uint32_t ticks = us * (SYSTEM_CORE_CLOCK_HZ / 1000000u);

    while ((DWT->CYCCNT - start) < ticks)
    {
    }
}

static void dq_output(void)
{
    /* CRH：PB12 是第 4 个（12-8），推挽输出 50MHz */
    DQ_PORT->CRH &= ~(0xFu << ((DQ_PIN - 8) * 4));
    DQ_PORT->CRH |= (0x3u << ((DQ_PIN - 8) * 4));
}

static void dq_input(void)
{
    /* 上拉/下拉输入：靠外部 4.7k 上拉 */
    DQ_PORT->CRH &= ~(0xFu << ((DQ_PIN - 8) * 4));
    DQ_PORT->CRH |= (0x8u << ((DQ_PIN - 8) * 4));
}

void bsp_ds18b20_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_IOPBEN;

    /* 打开 DWT：Cortex-M3 的周期计数器，用来做精确微秒延时 */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;

    dq_output();
    DQ_HIGH();
}

/****************************************************************
 * 复位脉冲：拉低 480us 以上，然后释放，等器件回 60~240us 低电平
 * 返回 1 = 有器件应答，0 = 没接
 ****************************************************************/
static int ow_reset(void)
{
    int present;

    dq_output();
    DQ_LOW();
    bsp_delay_us(500);

    DQ_HIGH();
    dq_input();
    bsp_delay_us(70);

    present = (0 == DQ_READ()) ? 1 : 0;

    bsp_delay_us(430);          /* 等复位时隙走完 */

    return present;
}

static void ow_write_bit(int bit)
{
    dq_output();
    DQ_LOW();

    if (bit)
    {
        bsp_delay_us(6);
        DQ_HIGH();
        bsp_delay_us(64);
    }
    else
    {
        bsp_delay_us(60);
        DQ_HIGH();
        bsp_delay_us(10);
    }
}

static int ow_read_bit(void)
{
    int bit;

    dq_output();
    DQ_LOW();
    bsp_delay_us(3);
    DQ_HIGH();
    dq_input();
    bsp_delay_us(10);

    bit = DQ_READ();

    bsp_delay_us(53);

    return bit;
}

static void ow_write_byte(uint8_t byte)
{
    for (uint8_t i = 0; i < 8; i++)
    {
        ow_write_bit(byte & 0x01);
        byte >>= 1;
    }
}

static uint8_t ow_read_byte(void)
{
    uint8_t byte = 0;

    for (uint8_t i = 0; i < 8; i++)
    {
        byte >>= 1;
        if (ow_read_bit())
        {
            byte |= 0x80;
        }
    }

    return byte;
}

void bsp_ds18b20_start_convert(void)
{
    if (0 == ow_reset())
    {
        return;
    }

    ow_write_byte(CMD_SKIP_ROM);
    ow_write_byte(CMD_CONVERT_T);
}

int bsp_ds18b20_read(float *temp_c)
{
    uint8_t  buf[2];
    uint16_t raw;
    int      ret;

    if (NULL == temp_c)
    {
        return -1;
    }

    if (0 == ow_reset())
    {
        return -1;
    }

    ow_write_byte(CMD_SKIP_ROM);
    ow_write_byte(CMD_READ_SCRATCH);

    buf[0] = ow_read_byte();
    buf[1] = ow_read_byte();

    raw = (uint16_t)((buf[1] << 8) | buf[0]);

    /* 0x0550 = 85.0℃ 是上电默认值，说明之前没成功转换 */
    if (0x0550 == raw)
    {
        return -1;
    }

    /* 12 位分辨率：低 4 位是小数 */
    *temp_c = (float)((int16_t)raw) * 0.0625f;

    ret = ((*temp_c > -55.0f) && (*temp_c < 125.0f)) ? 0 : -1;

    return ret;
}

int bsp_ds18b20_read_blocking(float *temp_c)
{
    bsp_ds18b20_start_convert();
    delay_ms(750);              /* 12 位精度最长 750ms */

    return bsp_ds18b20_read(temp_c);
}
