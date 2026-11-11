#ifndef __MODBUS_H__
#define __MODBUS_H__

#include "linkqueue.h"

/*------------------------------------------------------------------
 * Modbus RTU 主机（i.MX6ULL 侧）
 *
 * 组网方式：
 *   [电池节点 STM32 + LoRa] × N  --LoRa-->  [网关 STM32]  --RS485/UART-->  i.MX6ULL
 *
 * i.MX6ULL 作为 Modbus 主机，按从机地址 1..N 轮流发 0x03（读保持寄存器），
 * 网关把 LoRa 收到的各个电池组数据映射到寄存器里应答。
 * 一个从机地址对应一个电池组节点。
 *----------------------------------------------------------------*/

#define MODBUS_REG_COUNT    8       /* 每次都读 8 个寄存器，一次搞定一个节点 */

/* 寄存器映射（每个从机 = 一个电池组） */
#define MB_REG_VOLT     0       /* 组电压   0.01 V   */
#define MB_REG_CURR     1       /* 组电流   0.01 A   */
#define MB_REG_TEMP     2       /* 温度     0.1 ℃    */
#define MB_REG_SOC      3       /* SOC      0.1 %    */
#define MB_REG_SOH      4       /* SOH      0.1 %    */
#define MB_REG_SMOKE    5       /* 烟雾     ppm      */
#define MB_REG_ALARM    6       /* 告警位            */
#define MB_REG_NODE     7       /* 节点号            */

/* dev 传串口设备名（例如 /dev/ttymxc2），slaves 是从机个数（1~247） */
int  modbus_init(const char *dev, int baud, int slaves);

/* 轮询下一个从机（内部自己轮转地址），成功拿到数据返回 0 */
int  modbus_poll_next(struct msgdata *out);

/* 指定从机读一次，成功返回 0 */
int  modbus_read_slave(int slave, struct msgdata *out);

unsigned long modbus_ok_count(void);
unsigned long modbus_err_count(void);

void modbus_close(void);

/* 暴露给测试用：算一帧 Modbus RTU 的 CRC16 */
unsigned short modbus_crc16(const unsigned char *buf, int len);

#endif
