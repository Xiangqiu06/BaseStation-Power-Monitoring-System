#ifndef __BSP_UART_H__
#define __BSP_UART_H__

#include <stdint.h>

/*------------------------------------------------------------------
 * 串口（直接用寄存器，不套 HAL）
 *
 *   UART_DEBUG  USART1  PA9/PA10   115200  调试打印
 *   UART_LORA   USART2  PA2/PA3     9600   Modbus RTU（接 RS485 收发器）
 *
 * 接收走中断 + 环形缓冲：Modbus 一帧 8 个字节，来得快，
 * 主循环里轮询容易漏字节，所以 ISR 只做"塞进环形缓冲"这一件事。
 *----------------------------------------------------------------*/

#define UART_DEBUG  0
#define UART_LORA   1

void     bsp_uart_init(void);

/* 发一串字节，阻塞到发完为止 */
void     bsp_uart_write(int port, const uint8_t *data, uint16_t len);

/* 发字符串（调试用） */
void     bsp_uart_puts(int port, const char *s);

/* 取一个字节：返回 0 成功、-1 没数据 */
int      bsp_uart_getc(int port, uint8_t *ch);

/* 环形缓冲里还有多少字节 */
uint16_t bsp_uart_available(int port);

/* 清空接收缓冲（Modbus 一帧收完要清，避免串帧） */
void     bsp_uart_flush(int port);

/* RS485 方向脚控制：1 = 发送，0 = 接收（板子上用一个 GPIO 控 DE/RE） */
void     bsp_uart_rs485_tx(int port, int enable);

#endif
