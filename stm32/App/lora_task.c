#include "app_tasks.h"
#include "bsp_sx1278.h"
#include "bsp_adc.h"
#include "bsp_uart.h"
#include "bsp_modbus_slave.h"
#include "soc.h"

#include <stdio.h>
#include <string.h>
#include <stddef.h>

/*------------------------------------------------------------------
 * LoRa 通信：一个文件两个角色
 *
 * 节点（ROLE_NODE）
 *   每 1s 把采样结果打成 15 字节的帧发出去。
 *   帧格式（小端）：
 *     [0]      node_id
 *     [1]      seq（滚动，丢包能看出来）
 *     [2..3]   volt*100   uint16
 *     [4..5]   curr*100   int16（正=充电 负=放电）
 *     [6..7]   temp*10    int16
 *     [8..9]   soc*10     uint16
 *     [10..11] soh*10     uint16
 *     [12..13] alarm      uint16
 *     [14]     CRC8（poly 0x31，和 i.MX6ULL 侧 protocol.c 一致）
 *
 * 网关（ROLE_GATEWAY）
 *   收 LoRa 帧 -> 更新"寄存器映像表" -> 对 i.MX6ULL 做 Modbus 从机。
 *   寄存器映射跟 Linux 侧 modbus.c 的 mb_read_slave() 一一对应。
 *----------------------------------------------------------------*/

#define MAX_NODES   8

/****************************************************************
 * CRC8：poly 0x31，和 Linux 侧 protocol.c 完全一样
 ****************************************************************/
static uint8_t crc8(const uint8_t *data, uint8_t len)
{
    uint8_t crc = 0x00;

    for (uint8_t i = 0; i < len; i++)
    {
        crc ^= data[i];

        for (uint8_t b = 0; b < 8; b++)
        {
            if (0 != (crc & 0x80))
            {
                crc = (uint8_t)((uint8_t)(crc << 1) ^ 0x31);
            }
            else
            {
                crc = (uint8_t)(crc << 1);
            }
        }
    }

    return crc;
}

#if (APP_ROLE == ROLE_NODE)

/****************************************************************
 * 节点：周期上报
 ****************************************************************/
void vLoraReportTask(void *pv)
{
    TickType_t last = xTaskGetTickCount();
    uint8_t    seq  = 0;
    char       line[128];

    (void)pv;

    sx1278_init();

    sprintf(line, "[LORA] node %u init, version=0x%02X\r\n",
            (unsigned)NODE_ID, (unsigned)sx1278_version());
    bsp_uart_puts(UART_DEBUG, line);

    for (;;)
    {
        uint8_t  frame[LORA_FRAME_LEN];
        float    v;
        float    i;
        uint16_t volt_100;
        int16_t  curr_100;
        uint16_t soc_10;
        uint16_t soh_10;

        /* 上报"此刻"的值：直接读 ADC，比从队列里捞更省事也更实时 */
        v = bsp_adc_read_bus_volt_v(8);
        i = bsp_adc_read_current_a(8);

        volt_100 = (uint16_t)(v * 100.0f + 0.5f);
        curr_100 = (int16_t)(i * 100.0f);
        soc_10   = soc_get_10();
        soh_10   = soh_get_10();

        frame[0] = NODE_ID;
        frame[1] = seq++;

        frame[2] = (uint8_t)(volt_100 & 0xFF);
        frame[3] = (uint8_t)(volt_100 >> 8);

        frame[4] = (uint8_t)((uint16_t)curr_100 & 0xFF);
        frame[5] = (uint8_t)((uint16_t)curr_100 >> 8);

        /* 温度由告警任务更新到全局变量，这里取一次快照 */
        frame[6] = (uint8_t)(app_temp_10() & 0xFF);
        frame[7] = (uint8_t)((uint16_t)app_temp_10() >> 8);

        frame[8]  = (uint8_t)(soc_10 & 0xFF);
        frame[9]  = (uint8_t)(soc_10 >> 8);

        frame[10] = (uint8_t)(soh_10 & 0xFF);
        frame[11] = (uint8_t)(soh_10 >> 8);

        frame[12] = (uint8_t)(((uint16_t)app_alarm_bits()) & 0xFF);
        frame[13] = (uint8_t)(((uint16_t)app_alarm_bits()) >> 8);

        frame[14] = crc8(frame, LORA_FRAME_LEN - 1);

        if (0 != sx1278_send(frame, LORA_FRAME_LEN))
        {
            bsp_uart_puts(UART_DEBUG, "[LORA] send timeout\r\n");
        }

        vTaskDelayUntil(&last, pdMS_TO_TICKS(REPORT_PERIOD_MS));
    }
}

#else /* ========================= 网关 ========================= */

/* 网关的寄存器映像：[节点][8 个寄存器] */
static uint16_t g_reg_map[MAX_NODES + 1][8];
static uint32_t g_rx_ok  = 0;
static uint32_t g_rx_err = 0;

/****************************************************************
 * Modbus 从机读回调
 *
 * i.MX6ULL 读从机 N 的第 R 个寄存器，就从映像表里取。
 * 节点还没上线时返回 0，Linux 侧会看到 0V/0A —— 它配合"连续为 0"
 * 就能判断是掉线，而不是真的没电。
 ****************************************************************/
static uint16_t gw_reg_read(uint8_t slave, uint16_t reg)
{
    if (slave < 1 || slave > MAX_NODES || reg >= 8)
    {
        return 0;
    }

    return g_reg_map[slave][reg];
}

static void gw_reg_write(uint8_t slave, uint16_t reg, uint16_t val)
{
    if (slave < 1 || slave > MAX_NODES || reg >= 8)
    {
        return;
    }

    /* 目前只开放"清告警"：0x06 写寄存器 6 = 0 */
    if (6 == reg && 0 == val)
    {
        g_reg_map[slave][6] = 0;
    }
}

void vGatewayTask(void *pv)
{
    (void)pv;

    sx1278_init();
    modbus_slave_init(gw_reg_read, gw_reg_write);

    bsp_uart_puts(UART_DEBUG, "[GW] ready: LoRa + Modbus RTU slave\r\n");

    for (;;)
    {
        uint8_t buf[32];
        int     n;

        /* ---- 1. 收 LoRa（50ms 超时），收到就刷寄存器映像 ---- */
        n = sx1278_recv(buf, sizeof(buf), 50);

        if (n >= LORA_FRAME_LEN)
        {
            uint8_t node = buf[0];

            if (crc8(buf, LORA_FRAME_LEN - 1) == buf[LORA_FRAME_LEN - 1] &&
                node >= 1 && node <= MAX_NODES)
            {
                g_reg_map[node][0] = (uint16_t)(buf[2]  | (buf[3]  << 8));
                g_reg_map[node][1] = (uint16_t)(buf[4]  | (buf[5]  << 8));
                g_reg_map[node][2] = (uint16_t)(buf[6]  | (buf[7]  << 8));
                g_reg_map[node][3] = (uint16_t)(buf[8]  | (buf[9]  << 8));
                g_reg_map[node][4] = (uint16_t)(buf[10] | (buf[11] << 8));
                g_reg_map[node][5] = 0;
                g_reg_map[node][6] = (uint16_t)(buf[12] | (buf[13] << 8));
                g_reg_map[node][7] = node;

                g_rx_ok++;

                if (0 == (g_rx_ok % 30))
                {
                    char line[112];

                    sprintf(line, "[GW] rx %lu rssi=%d node%u V=%u.%02u I=%d.%02u soc=%u.%u\r\n",
                            (unsigned long)g_rx_ok, sx1278_last_rssi(), (unsigned)node,
                            (unsigned)(g_reg_map[node][0] / 100),
                            (unsigned)(g_reg_map[node][0] % 100),
                            (int)(int16_t)g_reg_map[node][1] / 100,
                            (unsigned)((int)(int16_t)g_reg_map[node][1] % 100),
                            (unsigned)(g_reg_map[node][3] / 10),
                            (unsigned)(g_reg_map[node][3] % 10));
                    bsp_uart_puts(UART_DEBUG, line);
                }
            }
            else
            {
                g_rx_err++;
            }
        }

        /* ---- 2. 应答 Modbus（1ms 一轮，靠静默间隙判帧） ---- */
        modbus_slave_poll();

        vTaskDelay(pdMS_TO_TICKS(MODBUS_POLL_MS));
    }
}

#endif /* APP_ROLE */
