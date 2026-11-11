#ifndef __ALARM_H__
#define __ALARM_H__

#include "linkqueue.h"

/*------------------------------------------------------------------
 * 告警判定
 *
 * 用位标志表示，一帧数据可能同时踩中好几条，所以不是枚举。
 * 阈值全部来自配置文件，改阈值不用重编程序。
 *----------------------------------------------------------------*/

#define ALARM_NONE          0x00u
#define ALARM_TEMP_HIGH     0x01u   /* 温度过高 */
#define ALARM_HUM_HIGH      0x02u   /* 湿度过高，凝露风险 */
#define ALARM_VOLT_LOW      0x04u   /* 电压偏低 —— 后备电池快放完了 */
#define ALARM_VOLT_HIGH     0x08u   /* 电压偏高 —— 充电模块可能失控 */
#define ALARM_CURR_HIGH     0x10u   /* 电流过大 */
#define ALARM_SMOKE_HIGH    0x20u   /* 烟雾 —— 最严重 */

#define ALARM_LEVEL_NORMAL  0
#define ALARM_LEVEL_WARN    1
#define ALARM_LEVEL_CRIT    2

/* 判定一帧，结果直接写回 m->alarm / m->level，返回标志位。
 * 只在状态发生变化时打日志，避免每秒刷屏 */
int alarm_check(struct msgdata *m);

/* 把标志位翻成人话，写进 buf。buf 建议 >= 128 字节 */
int alarm_text(unsigned int flags, char *buf, int cap);

/* 等级名字："正常" / "警告" / "严重" */
const char *alarm_level_name(int level);

#endif
