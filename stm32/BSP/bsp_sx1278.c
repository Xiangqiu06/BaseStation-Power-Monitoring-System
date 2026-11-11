#include "bsp_sx1278.h"
#include "stm32f1xx.h"
#include "system_clock.h"
#include "bsp_ds18b20.h"     /* 借用 bsp_delay_us */

#include <stddef.h>

/*------------------------------------------------------------------
 * SX1278 寄存器（只列用到的）
 *----------------------------------------------------------------*/
#define REG_FIFO                0x00
#define REG_OP_MODE             0x01
#define REG_FRF_MSB             0x06
#define REG_FRF_MID             0x07
#define REG_FRF_LSB             0x08
#define REG_PA_CONFIG           0x09
#define REG_OCP                 0x0B
#define REG_LNA                 0x0C
#define REG_FIFO_ADDR_PTR       0x0D
#define REG_FIFO_TX_BASE_ADDR   0x0E
#define REG_FIFO_RX_BASE_ADDR   0x0F
#define REG_FIFO_RX_CURRENT     0x10
#define REG_IRQ_FLAGS           0x12
#define REG_RX_NB_BYTES         0x13
#define REG_PKT_RSSI_VALUE      0x1A
#define REG_MODEM_CONFIG_1      0x1D
#define REG_MODEM_CONFIG_2      0x1E
#define REG_PREAMBLE_MSB        0x20
#define REG_PREAMBLE_LSB        0x21
#define REG_PAYLOAD_LENGTH      0x22
#define REG_MODEM_CONFIG_3      0x26
#define REG_DETECTION_OPTIMIZE  0x31
#define REG_DETECTION_THRESHOLD 0x37
#define REG_SYNC_WORD           0x39
#define REG_DIO_MAPPING_1       0x40
#define REG_VERSION             0x42
#define REG_PA_DAC              0x4D

/* 模式 */
#define MODE_SLEEP              0x80
#define MODE_STDBY              0x81
#define MODE_TX                 0x83
#define MODE_RX_CONT            0x85
#define MODE_LORA               0x80

/* 中断标志 */
#define IRQ_TX_DONE             0x08
#define IRQ_RX_DONE             0x40
#define IRQ_PAYLOAD_CRC_ERROR   0x20

/* 引脚 */
#define NSS_LOW()    (GPIOA->BRR = (1u << 4))
#define NSS_HIGH()   (GPIOA->BSRR = (1u << 4))
#define RST_LOW()    (GPIOB->BRR = (1u << 1))
#define RST_HIGH()   (GPIOB->BSRR = (1u << 1))
#define DIO0_READ()  ((GPIOB->IDR & (1u << 0)) ? 1 : 0)

static int g_last_rssi = -120;

/****************************************************************
 * SPI1 收发（8 位，模式 0）
 ****************************************************************/
static uint8_t spi_xfer(uint8_t out)
{
    uint32_t guard = 0;

    while (0 == (SPI1->SR & SPI_SR_TXE))
    {
        if (++guard > 100000)
        {
            return 0;
        }
    }

    *(volatile uint8_t *)&SPI1->DR = out;

    guard = 0;
    while (0 == (SPI1->SR & SPI_SR_RXNE))
    {
        if (++guard > 100000)
        {
            return 0;
        }
    }

    return (uint8_t)SPI1->DR;
}

static void reg_write(uint8_t addr, uint8_t val)
{
    NSS_LOW();
    spi_xfer((uint8_t)(addr | 0x80));    /* 最高位 1 = 写 */
    spi_xfer(val);
    NSS_HIGH();
}

static uint8_t reg_read(uint8_t addr)
{
    uint8_t v;

    NSS_LOW();
    spi_xfer((uint8_t)(addr & 0x7F));    /* 最高位 0 = 读 */
    v = spi_xfer(0x00);
    NSS_HIGH();

    return v;
}

static void reg_write_burst(uint8_t addr, const uint8_t *buf, uint8_t len)
{
    NSS_LOW();
    spi_xfer((uint8_t)(addr | 0x80));
    for (uint8_t i = 0; i < len; i++)
    {
        spi_xfer(buf[i]);
    }
    NSS_HIGH();
}

static void reg_read_burst(uint8_t addr, uint8_t *buf, uint8_t len)
{
    NSS_LOW();
    spi_xfer((uint8_t)(addr & 0x7F));
    for (uint8_t i = 0; i < len; i++)
    {
        buf[i] = spi_xfer(0x00);
    }
    NSS_HIGH();
}

static void spi_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_IOPBEN |
                    RCC_APB2ENR_SPI1EN | RCC_APB2ENR_AFIOEN;

    /* PA5/PA7 复用推挽，PA6 浮空输入，PA4 通用推挽（软件片选） */
    GPIOA->CRL &= ~((0xFu << 20) | (0xFu << 24) | (0xFu << 28) | (0xFu << 16));
    GPIOA->CRL |= (0xBu << 20) | (0x4u << 24) | (0xBu << 28) | (0x3u << 16);

    /* PB0 DIO0 上拉输入，PB1 RESET 推挽输出 */
    GPIOB->CRL &= ~((0xFu << 0) | (0xFu << 4));
    GPIOB->CRL |= (0x8u << 0) | (0x3u << 4);

    NSS_HIGH();
    RST_HIGH();

    /* SPI1: 主机，8 位，CPOL=0 CPHA=0，时钟 72M/8 = 9MHz */
    SPI1->CR1 = 0;
    SPI1->CR1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI |
                SPI_CR1_BR_1 | SPI_CR1_SPE;
}

/****************************************************************
 * 频率：Frf = f / 61.03515625 Hz（32MHz 晶振 / 2^19）
 ****************************************************************/
void sx1278_set_freq(uint32_t hz)
{
    uint32_t frf = (uint32_t)((double)hz / 61.03515625);

    reg_write(REG_FRF_MSB, (uint8_t)(frf >> 16));
    reg_write(REG_FRF_MID, (uint8_t)(frf >> 8));
    reg_write(REG_FRF_LSB, (uint8_t)(frf));
}

void sx1278_init(void)
{
    uint32_t guard = 0;

    spi_init();

    /* 硬复位：RESET 拉低 100us 以上 */
    RST_LOW();
    bsp_delay_us(200);
    RST_HIGH();
    bsp_delay_us(10000);            /* 等内部 RC 稳定 */

    /* 进睡眠模式才能改调制参数（FSTXON/待机下改频点不生效） */
    reg_write(REG_OP_MODE, MODE_LORA | MODE_SLEEP);

    /* 470MHz 频段 */
    sx1278_set_freq(470000000u);

    /* FIFO 基址 */
    reg_write(REG_FIFO_TX_BASE_ADDR, 0x00);
    reg_write(REG_FIFO_RX_BASE_ADDR, 0x00);

    /* 功率：PA_BOOST，+17dBm，再打开 PA_DAC 提到 +20dBm */
    reg_write(REG_PA_CONFIG, 0x8F);     /* PA_BOOST | 输出 17dBm */
    reg_write(REG_PA_DAC, 0x87);        /* +20dBm 模式 */
    reg_write(REG_OCP, 0x2B);           /* 限流 150mA，保护电池 */

    /* 调制参数：SF9 / BW125 / CR4-5 / 显式包头 */
    reg_write(REG_MODEM_CONFIG_1, 0x72);
    reg_write(REG_MODEM_CONFIG_2, 0x94);   /* SF9 + CRC 使能 */
    reg_write(REG_MODEM_CONFIG_3, 0x04);   /* AGC 自动增益 */
    reg_write(REG_DETECTION_OPTIMIZE, 0xC3);
    reg_write(REG_DETECTION_THRESHOLD, 0x0A);

    reg_write(REG_PREAMBLE_MSB, 0x00);
    reg_write(REG_PREAMBLE_LSB, LORA_PREAMBLE);
    reg_write(REG_SYNC_WORD, 0x12);        /* 私有网络同步字，别用默认 0x34 */

    /* 进待机 */
    reg_write(REG_OP_MODE, MODE_LORA | MODE_STDBY);
    bsp_delay_us(1000);

    (void)guard;
}

uint8_t sx1278_version(void)
{
    return reg_read(REG_VERSION);
}

void sx1278_sleep(void)
{
    reg_write(REG_OP_MODE, MODE_LORA | MODE_SLEEP);
}

/****************************************************************
 * 发送
 ****************************************************************/
int sx1278_send(const uint8_t *data, uint8_t len)
{
    uint32_t t0;

    if (NULL == data || 0 == len)
    {
        return -1;
    }

    /* 回待机，写 FIFO */
    reg_write(REG_OP_MODE, MODE_LORA | MODE_STDBY);
    reg_write(REG_FIFO_ADDR_PTR, 0x00);
    reg_write(REG_PAYLOAD_LENGTH, len);
    reg_write_burst(REG_FIFO, data, len);

    /* DIO0 = TxDone */
    reg_write(REG_DIO_MAPPING_1, 0x40);
    reg_write(REG_IRQ_FLAGS, 0xFF);        /* 清所有标志 */

    reg_write(REG_OP_MODE, MODE_LORA | MODE_TX);

    /* 等 TxDone：SF9/125k 下一包 20 字节大约 100ms，超时给 2s */
    t0 = DWT->CYCCNT;

    while (0 == (reg_read(REG_IRQ_FLAGS) & IRQ_TX_DONE))
    {
        if ((DWT->CYCCNT - t0) > (2000u * (SYSTEM_CORE_CLOCK_HZ / 1000u)))
        {
            reg_write(REG_OP_MODE, MODE_LORA | MODE_STDBY);
            return -1;
        }
    }

    reg_write(REG_IRQ_FLAGS, 0xFF);
    reg_write(REG_OP_MODE, MODE_LORA | MODE_STDBY);

    return 0;
}

/****************************************************************
 * 接收：开连续接收，轮询 IRQ 标志
 ****************************************************************/
int sx1278_recv(uint8_t *data, uint8_t cap, int timeout_ms)
{
    uint32_t t0;
    uint32_t limit;
    uint8_t  len;
    uint8_t  flags;
    uint8_t  addr;

    if (NULL == data || 0 == cap)
    {
        return -1;
    }

    reg_write(REG_DIO_MAPPING_1, 0x00);    /* DIO0 = RxDone */
    reg_write(REG_IRQ_FLAGS, 0xFF);
    reg_write(REG_OP_MODE, MODE_LORA | MODE_RX_CONT);

    t0    = DWT->CYCCNT;
    limit = (uint32_t)timeout_ms * (SYSTEM_CORE_CLOCK_HZ / 1000u);

    while ((DWT->CYCCNT - t0) < limit)
    {
        flags = reg_read(REG_IRQ_FLAGS);

        if (0 != (flags & IRQ_RX_DONE))
        {
            if (0 != (flags & IRQ_PAYLOAD_CRC_ERROR))
            {
                reg_write(REG_IRQ_FLAGS, 0xFF);
                continue;               /* CRC 错就丢掉，继续等 */
            }

            len  = reg_read(REG_RX_NB_BYTES);
            addr = reg_read(REG_FIFO_RX_CURRENT);

            reg_write(REG_FIFO_ADDR_PTR, addr);

            if (len > cap)
            {
                len = cap;
            }

            reg_read_burst(REG_FIFO, data, len);

            g_last_rssi = (int)reg_read(REG_PKT_RSSI_VALUE) - 157;

            reg_write(REG_IRQ_FLAGS, 0xFF);

            return (int)len;
        }
    }

    return 0;                           /* 超时 */
}

int sx1278_last_rssi(void)
{
    return g_last_rssi;
}
