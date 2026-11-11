#include "bsp_uart.h"
#include "stm32f1xx.h"
#include <string.h>

/*------------------------------------------------------------------
 * USART1 / USART2 驱动
 *
 * 时钟：USART1 挂 APB2(72MHz)，USART2 挂 APB1(36MHz)，
 * 这两个算波特率时用的 PCLK 不一样，写死一个值会出现
 * "调试口正常、Modbus 乱码"这种很迷惑的现象。
 *----------------------------------------------------------------*/

#define RX_BUF_SIZE 128

typedef struct
{
    USART_TypeDef *reg;
    uint32_t       pclk_hz;
    volatile uint16_t head;      /* 写指针（中断里改） */
    volatile uint16_t tail;      /* 读指针（主循环改） */
    uint8_t        buf[RX_BUF_SIZE];
} uart_port_t;

static uart_port_t g_ports[2] =
{
    { USART1, 72000000UL, 0, 0, {0} },
    { USART2, 36000000UL, 0, 0, {0} },
};

static void uart_enable_clock(int port)
{
    if (UART_DEBUG == port)
    {
        RCC->APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_USART1EN | RCC_APB2ENR_AFIOEN;
    }
    else
    {
        RCC->APB1ENR |= RCC_APB1ENR_USART2EN;
        RCC->APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_AFIOEN;
    }
}

/* 把 PAx 配成复用推挽输出（AFIO 的 CRL/CRH 每 4 位管一个脚） */
static void gpio_af_pp(GPIO_TypeDef *gpio, uint8_t pin)
{
    if (pin < 8)
    {
        gpio->CRL &= ~(0xFu << (pin * 4));
        gpio->CRL |= (0xBu << (pin * 4));       /* 复用推挽，50MHz */
    }
    else
    {
        uint8_t p = (uint8_t)(pin - 8);

        gpio->CRH &= ~(0xFu << (p * 4));
        gpio->CRH |= (0xBu << (p * 4));
    }
}

/* 浮空输入，给 RX 用 */
static void gpio_input_floating(GPIO_TypeDef *gpio, uint8_t pin)
{
    if (pin < 8)
    {
        gpio->CRL &= ~(0xFu << (pin * 4));
        gpio->CRL |= (0x4u << (pin * 4));
    }
    else
    {
        uint8_t p = (uint8_t)(pin - 8);

        gpio->CRH &= ~(0xFu << (p * 4));
        gpio->CRH |= (0x4u << (p * 4));
    }
}

static void uart_set_baud(uart_port_t *p, uint32_t baud)
{
    /* 波特率寄存器 = PCLK / (16 * baud)，小数部分靠 BRR 的低 4 位 */
    uint32_t div = (p->pclk_hz * 25u) / (4u * baud);

    p->reg->BRR = (uint16_t)((div / 100u) << 4 | ((div % 100u) * 16u + 50u) / 100u);
}

void bsp_uart_init(void)
{
    /* ---------- USART1：调试口 ---------- */
    uart_enable_clock(UART_DEBUG);
    gpio_af_pp(GPIOA, 9);          /* TX */
    gpio_input_floating(GPIOA, 10);/* RX */

    uart_set_baud(&g_ports[UART_DEBUG], 115200);
    g_ports[UART_DEBUG].reg->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE |
                                   USART_CR1_RXNEIE;

    NVIC_SetPriority(USART1_IRQn, 6);
    NVIC_EnableIRQ(USART1_IRQn);

    /* ---------- USART2：Modbus RTU / RS485 ---------- */
    uart_enable_clock(UART_LORA);
    gpio_af_pp(GPIOA, 2);          /* TX */
    gpio_input_floating(GPIOA, 3); /* RX */

    uart_set_baud(&g_ports[UART_LORA], 9600);
    g_ports[UART_LORA].reg->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE |
                                  USART_CR1_RXNEIE;

    NVIC_SetPriority(USART2_IRQn, 6);
    NVIC_EnableIRQ(USART2_IRQn);

    /* RS485 方向脚：PA8 推挽输出，默认收 */
    GPIOA->CRH &= ~(0xFu << 0);
    GPIOA->CRH |= (0x3u << 0);     /* 通用推挽 50MHz */
    GPIOA->BRR = (1u << 8);        /* DE = 0，接收态 */
}

void bsp_uart_write(int port, const uint8_t *data, uint16_t len)
{
    uart_port_t *p = &g_ports[port];

    if (NULL == data)
    {
        return;
    }

    for (uint16_t i = 0; i < len; i++)
    {
        while (0 == (p->reg->SR & USART_SR_TXE))
        {
        }
        p->reg->DR = data[i];
    }

    while (0 == (p->reg->SR & USART_SR_TC))
    {
    }
}

void bsp_uart_puts(int port, const char *s)
{
    if (NULL == s)
    {
        return;
    }

    bsp_uart_write(port, (const uint8_t *)s, (uint16_t)strlen(s));
}

int bsp_uart_getc(int port, uint8_t *ch)
{
    uart_port_t *p = &g_ports[port];

    if (NULL == ch || p->head == p->tail)
    {
        return -1;
    }

    *ch = p->buf[p->tail];
    p->tail = (uint16_t)((p->tail + 1) % RX_BUF_SIZE);

    return 0;
}

uint16_t bsp_uart_available(int port)
{
    uart_port_t *p = &g_ports[port];

    return (uint16_t)((p->head + RX_BUF_SIZE - p->tail) % RX_BUF_SIZE);
}

void bsp_uart_flush(int port)
{
    g_ports[port].tail = g_ports[port].head;
}

void bsp_uart_rs485_tx(int port, int enable)
{
    if (UART_LORA != port)
    {
        return;
    }

    if (enable)
    {
        GPIOA->BSRR = (1u << 8);
    }
    else
    {
        GPIOA->BRR = (1u << 8);
    }
}

/****************************************************************
 * 接收中断：只做一件事 —— 把字节塞进环形缓冲
 *
 * 这里不用 FreeRTOS 的 FromISR 接口，因为串口字节流太密，
 * 每个字节唤醒一次任务不值得；主循环/任务按需取就行。
 ****************************************************************/
static void uart_isr(uart_port_t *p)
{
    if (0 != (p->reg->SR & USART_SR_RXNE))
    {
        uint8_t ch = (uint8_t)(p->reg->DR & 0xFF);
        uint16_t next = (uint16_t)((p->head + 1) % RX_BUF_SIZE);

        /* 满了就丢新字节：Modbus 帧很短，真满了说明上层卡住了 */
        if (next != p->tail)
        {
            p->buf[p->head] = ch;
            p->head = next;
        }
    }
}

void USART1_IRQHandler(void)
{
    uart_isr(&g_ports[UART_DEBUG]);
}

void USART2_IRQHandler(void)
{
    uart_isr(&g_ports[UART_LORA]);
}
