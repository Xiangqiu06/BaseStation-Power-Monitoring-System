#ifndef __MODBUS_SLAVE_H__
#define __MODBUS_SLAVE_H__

#include <stdint.h>

/*------------------------------------------------------------------
 * Modbus RTU 从机（USART2 + RS485）
 *
 * 网关 STM32 的角色：对 i.MX6ULL 而言它就是一个 Modbus 从机，
 * 每来一次 0x03（读保持寄存器）就查一下本地的寄存器映像表——
 * 那张表是 LoRa 那头各个电池组节点刚刚刷上来的。
 *
 * 只实现 0x03 和 0x06（写单个寄存器），够用了：
 * 0x03 读数据、0x06 可以下发"清 SOC""改节点地址"这类简单命令。
 *----------------------------------------------------------------*/

/* 寄存器访问回调：i.MX6ULL 读哪个寄存器就调它要值 */
typedef uint16_t (*mb_read_fn)(uint8_t slave, uint16_t reg);
typedef void     (*mb_write_fn)(uint8_t slave, uint16_t reg, uint16_t val);

void modbus_slave_init(mb_read_fn rd, mb_write_fn wr);

/* 在任务里周期调用：收一帧、判地址、应答 */
void modbus_slave_poll(void);

/* 统计，方便调试 */
uint32_t modbus_slave_ok_count(void);
uint32_t modbus_slave_err_count(void);

#endif
