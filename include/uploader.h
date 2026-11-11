#ifndef __UPLOADER_H__
#define __UPLOADER_H__

#include "linkqueue.h"

/* 断网期间最多缓存多少条，满了丢最旧的。
 * 1Hz 采集下 256 条约等于 4 分钟，够撑过常见的网络抖动 */
#define UPLOAD_OFFLINE_MAX      256

/* 连接超时（秒），板子上的 4G 模块握手慢，别设太短 */
#define UPLOAD_CONNECT_TO_S     3

/* 解析上报地址。返回 0 成功；-1 表示配置有误（比如没填 host） */
int  uploader_init(void);

/* 1 = 当前连着；0 = 断的 */
int  uploader_connected(void);

/* 发一条。返回 0 表示"已经交付"（真发出去或者进了离线缓存），
 * -1 表示连缓存都满了，这条真的丢了 */
int  uploader_send(const struct msgdata *m);

/* 把离线缓存里的旧数据补发出去，返回这次补发成功的条数 */
int  uploader_flush_offline(void);

int           uploader_offline_count(void);
unsigned long uploader_sent_count(void);
unsigned long uploader_fail_count(void);

/* 断开连接，进程退出时调 */
void uploader_close(void);

#endif
