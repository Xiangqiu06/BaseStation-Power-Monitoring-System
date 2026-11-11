#ifndef __LINKLIST_H__
#define __LINKLIST_H__

#include "linkqueue.h"
#include <pthread.h>
#include <semaphore.h>

/* 链表存放数据的类型 */
typedef struct thread_task
{
	char name[32]; 
	pthread_t tid;              //线程任务的名称
	void *(*pfun)(void *);    //线程启动后执行的线程任务
	QNode_t *pque;              //线程间用于通信的邮箱队列
	pthread_mutex_t mutex;      //保护队列
	sem_t sem;                  //同步多线程对队列的读写
}DataType;

/* 链表节点类型 */
typedef struct node 
{
	DataType Data;						//数据域
	struct node *pNext;					//地址域
}Node_t;


extern Node_t *CreateEmptyLinkList(void);
extern int InsertHeadLinkList(Node_t *pTmpList, DataType TmpData);
extern void ShowLinkList(Node_t *pTmpList);
extern int IsEmptyLinkList(Node_t *pTmpList);
extern void DestroyLinkList(Node_t **ppTmpList);
extern Node_t *FindMidLinkList(Node_t *pTmpList);
extern Node_t *FindLastKthLinkList(Node_t *pTmpList, int Kth);
extern int DeleteNode(Node_t *pTmpNode);
extern int ReverseLinkList(Node_t *pTmpList);

#endif
