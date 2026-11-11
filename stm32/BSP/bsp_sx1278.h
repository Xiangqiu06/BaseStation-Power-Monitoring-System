#ifndef __BSP_SX1278_H__
#define __BSP_SX1278_H__

#include <stdint.h>

/*------------------------------------------------------------------
 * SX1278 LoRa 模块（SPI1）
 *
 *   PA5 SCK / PA6 MISO / PA7 MOSI
 *   PA4 NSS（软件片选）
 *   PB1 RESET / PB0 DIO0
 *
 * 组网方式：每个电池组节点挂一片 SX1278，主从轮询。
 * 节点只在被叫到时才回话，避免几个节点同时发射互相压住
 * （LoRa 是半双工，同频同时发就是互相干扰）。
 *----------------------------------------------------------------*/

#define LORA_SF         9       /* 扩频因子：SF9 在 470M 下兼顾距离和速率 */
#define LORA_BW         125     /* 带宽 kHz */
#define LORA_PREAMBLE   8

void sx1278_init(void);
void sx1278_set_freq(uint32_t hz);

/* 发一包，阻塞到 TX_DONE 或超时。返回 0 成功 */
int  sx1278_send(const uint8_t *data, uint8_t len);

/* 收一包：收到返回字节数（>0），超时返回 0，出错 -1 */
int  sx1278_recv(uint8_t *data, uint8_t cap, int timeout_ms);

/* 最近一包的 RSSI（dBm，负数） */
int  sx1278_last_rssi(void);

/* 读版本号，正常应该是 0x12；不是的话说明 SPI 没通 */
uint8_t sx1278_version(void);

/* 进入睡眠，省电 */
void sx1278_sleep(void);

#endif
