#ifndef __STORAGE_H__
#define __STORAGE_H__

#include "linkqueue.h"

#define DB_PATH_DEFAULT "sensor.db"   /* 板子上改成 /var/lib/sensor.db 之类的绝对路径 */

/* 打开数据库、建表、建索引、预编译 INSERT。path 传 NULL 用默认路径 */
int  storage_init(const char *path);

/* 写入一条记录。返回 0 成功，-1 失败 */
int  storage_insert(const struct msgdata *m);

/* 写入一条告警记录（只有 m->alarm != 0 的帧才需要调） */
int  storage_insert_alarm(const struct msgdata *m, const char *text);

/* 查当前总条数，调试用 */
int  storage_count(void);

/* 查最近的 n 条，按时间倒序，写进 out 数组，返回实际条数 */
int  storage_query_recent(struct msgdata *out, int max);

/* 导出 CSV。from_ts / to_ts 传 NULL 表示不限。返回导出条数 */
int  storage_export_csv(const char *csv_path, const char *from_ts, const char *to_ts);

/* 删掉 retain_days 天以前的记录，返回删掉的条数（<=0 表示不清理） */
int  storage_cleanup(int retain_days);

void storage_close(void);

#endif
