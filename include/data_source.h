#ifndef __DATA_SOURCE_H__
#define __DATA_SOURCE_H__

#include "linkqueue.h"

/* 没配配置文件时的兜底采集周期 */
#define DATA_SOURCE_PERIOD_MS  1000

/* 初始化数据源。
 *   dev 传 "sim" / NULL / 空串  -> 走内置模拟数据源
 *   dev 传 "/dev/ttymxc2"       -> 打开串口，按 protocol.h 的帧格式收数
 * 两种模式对外完全一样，上层线程不用改一行。
 * 失败返回 -1 */
extern int  data_source_init(const char *dev, int baud);

/* 阻塞读一帧：拿到数据返回 0 并填满 *out；
 * 退出（g_running 变 0）或串口出错返回 -1 */
extern int  data_source_read(struct msgdata *out);

/* 关闭数据源，真机模式下会 close 掉串口 */
extern void data_source_close(void);

/* 1 = 当前是模拟源，0 = 真串口。只用来打印启动横幅 */
extern int  data_source_is_sim(void);

/* 累计收帧错误（CRC/长度），串口干扰大时这个数会涨 */
extern unsigned long data_source_err_count(void);

#endif
