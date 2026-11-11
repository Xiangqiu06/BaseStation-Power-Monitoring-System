#include "bsp_modbus_slave.h"
#include "bsp_uart.h"
#include "stm32f1xx.h"
#include "system_clock.h"

#include <stddef.h>

/*------------------------------------------------------------------
 * Modbus RTU 从机
 *
 * 帧： [从机地址][功能码][数据...][CRC 低][CRC 高]
 *
 * 收帧靠"3.5 字符静默"判结束：9600 波特率下一个字符 11 位 ≈ 1.15ms，
 * 3.5 字符约 4ms。这里用 5ms 的静默窗口，留一点余量。
 * 用纯轮询而不是定时器中断，是因为一帧最多 8+1 字节，
 * 轮询的开销可以忽略，还省了一个定时器资源。
 *----------------------------------------------------------------*/

#define MB_MAX_FRAME    64
#define MB_GAP_MS       5

static mb_read_fn  g_read_fn  = NULL;
static mb_write_fn g_write_fn = NULL;
static uint32_t    g_ok_cnt   = 0;
static uint32_t    g_err_cnt  = 0;

uint32_t modbus_slave_ok_count(void)  { return g_ok_cnt; }
uint32_t modbus_slave_err_count(void) { return g_err_cnt; }

void modbus_slave_init(mb_read_fn rd, mb_write_fn wr)
{
    g_read_fn  = rd;
    g_write_fn = wr;

    bsp_uart_flush(UART_LORA);
    bsp_uart_rs485_tx(UART_LORA, 0);

    g_ok_cnt  = 0;
    g_err_cnt = 0;
}

static uint16_t mb_crc16(const uint8_t *buf, uint16_t len)
{
    uint16_t crc = 0xFFFF;

    for (uint16_t i = 0; i < len; i++)
    {
        crc ^= buf[i];

        for (uint8_t b = 0; b < 8; b++)
        {
            if (0 != (crc & 0x0001))
            {
                crc = (uint16_t)((crc >> 1) ^ 0xA001);
            }
            else
            {
                crc = (uint16_t)(crc >> 1);
            }
        }
    }

    return crc;
}

/* 应答：CRC 低字节在前 */
static void mb_reply(uint8_t *buf, uint8_t len)
{
    uint16_t crc = mb_crc16(buf, len);

    buf[len++] = (uint8_t)(crc & 0xFF);
    buf[len++] = (uint8_t)((crc >> 8) & 0xFF);

    bsp_uart_rs485_tx(UART_LORA, 1);
    bsp_uart_write(UART_LORA, buf, len);
    bsp_uart_rs485_tx(UART_LORA, 0);

    g_ok_cnt++;
}

/* 异常应答：功能码 | 0x80 + 异常码 */
static void mb_exception(uint8_t slave, uint8_t func, uint8_t code)
{
    uint8_t buf[5];

    buf[0] = slave;
    buf[1] = (uint8_t)(func | 0x80);
    buf[2] = code;

    mb_reply(buf, 3);
}

void modbus_slave_poll(void)
{
    static uint8_t  frame[MB_MAX_FRAME];
    static uint8_t  len = 0;
    static uint8_t  idle_ms = 0;

    uint8_t  ch;
    uint16_t crc;
    uint8_t  slave;
    uint8_t  func;
    uint16_t reg;
    uint16_t cnt;
    uint16_t i;

    /* 逐字节收，收到就重置静默计时 */
    while (0 == bsp_uart_getc(UART_LORA, &ch))
    {
        if (len < MB_MAX_FRAME)
        {
            frame[len++] = ch;
        }
        idle_ms = 0;
    }

    if (0 == len)
    {
        return;
    }

    /* 静默判帧结束 */
    idle_ms++;

    if (idle_ms < MB_GAP_MS)
    {
        return;
    }

    idle_ms = 0;

    if (len < 8)
    {
        g_err_cnt++;
        len = 0;
        return;
    }

    crc = mb_crc16(frame, (uint16_t)(len - 2));
    if (frame[len - 2] != (uint8_t)(crc & 0xFF) ||
        frame[len - 1] != (uint8_t)((crc >> 8) & 0xFF))
    {
        g_err_cnt++;
        len = 0;
        return;
    }

    slave = frame[0];
    func  = frame[1];

    /* 地址不匹配就当没听见（总线上可能有别的从机） */
    if (0 == slave)
    {
        len = 0;
        return;                 /* 广播不回应 */
    }

    switch (func)
    {
        case 0x03:
        {
            uint8_t rsp[5 + 2 * 32];

            if (len < 8)
            {
                mb_exception(slave, func, 0x03);
                break;
            }

            reg = (uint16_t)((frame[2] << 8) | frame[3]);
            cnt = (uint16_t)((frame[4] << 8) | frame[5]);

            if (cnt < 1 || cnt > 32 || NULL == g_read_fn)
            {
                mb_exception(slave, func, 0x03);      /* 数据值不合法 */
                break;
            }

            rsp[0] = slave;
            rsp[1] = func;
            rsp[2] = (uint8_t)(cnt * 2);

            for (i = 0; i < cnt; i++)
            {
                uint16_t v = g_read_fn(slave, (uint16_t)(reg + i));

                rsp[3 + i * 2] = (uint8_t)(v >> 8);
                rsp[4 + i * 2] = (uint8_t)(v & 0xFF);
            }

            mb_reply(rsp, (uint8_t)(3 + cnt * 2));
            break;
        }

        case 0x06:
        {
            uint8_t rsp[8];

            if (len < 8)
            {
                mb_exception(slave, func, 0x03);
                break;
            }

            reg = (uint16_t)((frame[2] << 8) | frame[3]);

            if (NULL != g_write_fn)
            {
                g_write_fn(slave, reg, (uint16_t)((frame[4] << 8) | frame[5]));
            }

            /* 0x06 的应答就是把请求原样回一遍 */
            for (i = 0; i < 6; i++)
            {
                rsp[i] = frame[i];
            }

            mb_reply(rsp, 6);
            break;
        }

        default:
            mb_exception(slave, func, 0x01);          /* 非法功能码 */
            break;
    }

    len = 0;
}
