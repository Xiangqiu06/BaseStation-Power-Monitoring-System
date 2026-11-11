#include "linkqueue.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int g_drop_count = 0;    /* ===== 新增1：累计因为队列满被丢掉多少条 ===== */

QNode_t *CreateLinkQueue(void)
{
	QNode_t *pTmpNode = NULL;

	pTmpNode = malloc(sizeof(QNode_t));
	if (NULL == pTmpNode)
	{
		printf("malloc failed\n");
		return NULL;
	}

	pTmpNode->pNext = NULL;

	return pTmpNode;
}

int IsEmptyLinkQueue(QNode_t *pTmpQueue)
{
	return pTmpQueue->pNext == NULL ? 1 : 0;
}

int EnterLinkQueue(QNode_t *pTmpQueue, QDataType TmpData)
{
	QNode_t *pTmpNode = NULL;
	QNode_t *pLastNode = NULL;
	QDataType tDrop;

	/* ===== 新增2：队列满了，先把最旧的一条挤出去 ===== */
	if (GetQueueCount(pTmpQueue) >= QUEUE_MAX)
	{
		PopLinkQueue(pTmpQueue, &tDrop);
		g_drop_count++;
	}
	/* ===== 新增2结束 ===== */

	pTmpNode = malloc(sizeof(QNode_t));
	if (NULL == pTmpNode)
	{
		printf("malloc failed\n");
		return -1;
	}

	pTmpNode->Data = TmpData;
	pTmpNode->pNext = NULL;

	pLastNode = pTmpQueue;
	while (pLastNode->pNext != NULL)
	{
		pLastNode = pLastNode->pNext;
	}

	pLastNode->pNext = pTmpNode;

	return 0;
}
int PopLinkQueue(QNode_t *pTmpQueue, QDataType *pTmpData)
{
	QNode_t *pFreeNode = NULL;

	if (IsEmptyLinkQueue(pTmpQueue))
	{
		return -1;
	}

	pFreeNode = pTmpQueue->pNext;
	pTmpQueue->pNext = pFreeNode->pNext;
    if (pTmpData != NULL)
    {
	    *pTmpData = pFreeNode->Data;
    }
	free(pFreeNode);
	pFreeNode = NULL;

	return 0;
}

int DestroyLinkQueue(QNode_t **ppTmpQueue)
{
	QNode_t *pTmpNode = NULL;
	QNode_t *pFreeNode = NULL;

	pTmpNode = pFreeNode = *ppTmpQueue;
	while (pTmpNode != NULL)
	{
		pTmpNode = pTmpNode->pNext;
		free(pFreeNode);
		pFreeNode = pTmpNode;
	}
	*ppTmpQueue = NULL;
	
	return 0;
}

/* ===== 新增3：统计函数 ===== */
/* 数一下队列里现在有几条：从头走到尾，跟原来的遍历方式一样 */
int GetQueueCount(QNode_t *pTmpQueue)
{
	int cnt = 0;
	QNode_t *pTmpNode = pTmpQueue->pNext;

	while (pTmpNode != NULL)
	{
		cnt++;
		pTmpNode = pTmpNode->pNext;
	}

	return cnt;
}

int GetDropCount(void)
{
	return g_drop_count;
}
