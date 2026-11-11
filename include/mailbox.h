
#ifndef __MAILBOX_H__
#define __MAILBOX_H__

#include "linklist.h"
#include <signal.h> 
extern Node_t *create_mailbox_system();
extern int register_thread_mail_task(Node_t *pmail_root, char *name, void * (*pfun)(void *));
extern void destroy_mailbox(Node_t *pmail_root);
extern int recv_mail_msg(struct msgdata *mydata, char *send_name);

/* 带超时的收消息。timeout_ms 传 0 表示"只看一眼，没有就算"，
 * UI 线程用它：既要不阻塞地取数据，又要保持 5ms 一帧的刷新节奏 */
extern int recv_mail_msg_timeout(struct msgdata *mydata, char *send_name, int timeout_ms);

extern int send_mail_msg(char *send_name, struct msgdata data, char *recv_name);

extern volatile sig_atomic_t g_running;     /* 新增：全局运行标志 */
#endif