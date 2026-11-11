#ifndef __MQTT_H__
#define __MQTT_H__

#include "linkqueue.h"

/*------------------------------------------------------------------
 * MQTT 3.1.1 客户端（自己封报文，不依赖 paho）
 *
 * 为什么要自己写：
 *   板子上是 arm-linux-gnueabihf 工具链，paho.mqtt.c 要连带 openssl，
 *   交叉编译一次得折腾半天，而我们只需要"连上、发数据、收配置"三件事，
 *   协议本身 300 行就够。顺带也把 MQTT 报文格式彻底弄明白了。
 *
 * 支持：
 *   CONNECT / CONNACK、PUBLISH(QoS0/QoS1) / PUBACK、
 *   SUBSCRIBE(cmd 主题) / 收下行 PUBLISH、PINGREQ / PINGRESP、DISCONNECT
 *
 * 主题：
 *   basestation/<dev_id>/telemetry   周期数据
 *   basestation/<dev_id>/alarm       告警
 *   basestation/<dev_id>/cmd         云端下发（订阅）
 *
 * 下发命令格式（刻意做成不是 JSON，省掉一个 JSON 解析器）：
 *   SET volt_min=45.5
 *   SET temp_max=65
 *   GET                -> 回一条当前阈值
 *----------------------------------------------------------------*/

#define MQTT_PKT_MAX    512     /* 单条报文上限，遥测 JSON 200 字节左右够用 */

int  mqtt_init(void);                       /* 0 成功；-1 配置不全 */
int  mqtt_connected(void);

/* 发一条采集/告警数据（自动选主题、自动打 JSON） */
int  mqtt_publish(const struct msgdata *m);

/* 发原始文本到指定主题 */
int  mqtt_publish_raw(const char *topic, const char *payload, int qos);

/* 周期调用：读下行、回 PUBACK、到点发 PINGREQ */
void mqtt_poll(void);

void mqtt_close(void);

unsigned long mqtt_pub_count(void);
unsigned long mqtt_fail_count(void);
int           mqtt_reconnect_count(void);

#endif
