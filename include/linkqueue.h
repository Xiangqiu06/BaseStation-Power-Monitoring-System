#ifndef __LINKQUEUE_H__
#define __LINKQUEUE_H__

#include <time.h>

#define QUEUE_MAX 64        /* 队列最多存 64 条，满了丢最旧的 */

struct msgdata
{
    int id;           //设备id
    int bat_id;       //电池组编号（LoRa 组网时区分节点，0 表示本机）
    float temp;       //温度
    float  hum;        //湿度
    float  v;          //电压
    float a;           //电流
    float smoke;
    float soc;            //剩余电量 %（来自下位机安时积分）
    float soh;            //电池健康度 %（内阻/容量衰减估算）
    unsigned int alarm;   //告警标志位，见 alarm.h，0 表示一切正常
    int level;            //告警等级 0=正常 1=警告 2=严重
    //.......
    struct tm tim;   //时间戳
};

struct msg
{
      char send_name[32];  //邮箱消息的发送者名字
      struct msgdata data;    //共享的数据
      char recv_name[32];  //邮箱消息的接收者名字
};


typedef struct msg QDataType;

typedef struct qnode 
{
	QDataType Data;
	struct qnode *pNext;
}QNode_t;

extern QNode_t *CreateLinkQueue(void);
extern int IsEmptyLinkQueue(QNode_t *pTmpQueue);
extern int EnterLinkQueue(QNode_t *pTmpQueue, QDataType TmpData);
extern int PopLinkQueue(QNode_t *pTmpQueue, QDataType *pTmpData);
extern int DestroyLinkQueue(QNode_t **ppTmpQueue);
extern int GetQueueCount(QNode_t *pTmpQueue);      /* 新增 */
extern int GetDropCount(void);                     /* 新增 */

#endif
