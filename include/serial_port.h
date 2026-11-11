#ifndef __SERIAL_PORT_H__
#define __SERIAL_PORT_H__

/*------------------------------------------------------------------
 * 串口底层封装
 *
 * 原来 termios 那套代码写在 data_source.c 里，加 Modbus 之后
 * 要用同一套打开/配置逻辑，所以抽出来放这儿：
 *   - protocol 帧模式（自定义 AA55 帧）
 *   - Modbus RTU 模式（轮询 LoRa 网关）
 * 两个模块共用，避免同一段 termios 代码抄两遍。
 *----------------------------------------------------------------*/

/* 打开并配置为 raw 8N1，无流控。失败返回 -1 */
int  serial_port_open(const char *dev, int baud);

/* 关闭，传 -1 安全 */
void serial_port_close(int fd);

/* 带超时读：返回读到的字节数(>0)、0 表示超时、-1 表示出错 */
int  serial_port_read(int fd, unsigned char *buf, int cap, int timeout_ms);

/* 写满 len 个字节，返回 0 成功 */
int  serial_port_write(int fd, const unsigned char *buf, int len);

/* 丢掉收发缓冲里的残留 */
void serial_port_flush(int fd);

#endif
