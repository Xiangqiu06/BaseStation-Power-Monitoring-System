#include "mailbox.h"
#include "logger.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

/*------------------------------------------------------------------
 * 线程间邮箱
 *
 * 每个线程注册时领到一条自己的队列 + 一把锁 + 一个信号量：
 *   发消息 = 往对方队列里塞一条，然后 sem_post
 *   收消息 = sem_wait 等到有信号，再从队列里弹一条
 *
 * 相比"一个全局大队列 + 消费者抢"，每条队列独立的好处是
 * 一个消费者慢（比如写库）不会把其他消费者一起拖住。
 *
 * 1.1 改了两处：
 *   1) recv 原来死等 sem_wait，进程要退出时只能靠 destroy 里补 post 唤醒，
 *      时序上很脆。现在改成 200ms 超时轮询，天然能感知 g_running；
 *   2) 注册线程失败时把已经申请的资源还回去，不留半拉子节点。
 *----------------------------------------------------------------*/

Node_t *pmail = NULL;
volatile sig_atomic_t g_running = 1;

#define MAIL_WAIT_MS  200       /* 收消息的最长等待，同时也是退出响应时间 */

Node_t *create_mailbox_system(void)
{
    pmail = CreateEmptyLinkList();
    if (NULL == pmail)
    {
        LOG_ERROR("CreateEmptyLinkList error");
        return NULL;
    }

    return pmail;
}

/****************************************************************
 * 注册一个线程：建队列、初始化锁和信号量、把节点挂到链表头、起线程
 ****************************************************************/
int register_thread_mail_task(Node_t *pmail_root, char *name, void *(*pfun)(void *))
{
    Node_t *pTmpNode = NULL;
    int     ret;

    if (NULL == pmail_root || NULL == name || NULL == pfun)
    {
        return -1;
    }

    /* 1.申请空间 */
    pTmpNode = malloc(sizeof(Node_t));
    if (NULL == pTmpNode)
    {
        LOG_ERROR("malloc failed");
        return -1;
    }
    memset(pTmpNode, 0, sizeof(Node_t));

    snprintf(pTmpNode->Data.name, sizeof(pTmpNode->Data.name), "%s", name);
    pTmpNode->Data.pfun = pfun;

    pTmpNode->Data.pque = CreateLinkQueue();
    if (NULL == pTmpNode->Data.pque)
    {
        LOG_ERROR("CreateLinkQueue error");
        free(pTmpNode);
        return -1;
    }

    pthread_mutex_init(&pTmpNode->Data.mutex, NULL);
    sem_init(&pTmpNode->Data.sem, 0, 0);

    /* 2.挂到链表头（头插，顺序无所谓，靠名字找） */
    pTmpNode->pNext   = pmail_root->pNext;
    pmail_root->pNext = pTmpNode;

    /* 3.起线程。tid 在这里被写入，recv_mail_msg 靠它认领自己的队列 */
    ret = pthread_create(&pTmpNode->Data.tid, NULL, pfun, NULL);
    if (0 != ret)
    {
        LOG_ERROR("pthread_create(%s) failed: %s", name, strerror(ret));

        /* 起不来就把刚挂上去的节点摘掉，不能留个死节点让别人往里发消息 */
        pmail_root->pNext = pTmpNode->pNext;

        DestroyLinkQueue(&pTmpNode->Data.pque);
        pthread_mutex_destroy(&pTmpNode->Data.mutex);
        sem_destroy(&pTmpNode->Data.sem);
        free(pTmpNode);

        return -1;
    }

    LOG_INFO("[mailbox] 线程 %-10s 已注册 tid=%lu", name, (unsigned long)pTmpNode->Data.tid);

    return 0;
}

/****************************************************************
 * 销毁整个邮箱系统
 *
 * 顺序很重要：
 *   先 post 信号量把可能还卡在 sem_wait 的线程捅醒 ->
 *   再 join 等它们真正退出 ->
 *   最后才能释放队列和同步对象
 * 反过来的话线程可能操作已经 free 掉的内存
 ****************************************************************/
void destroy_mailbox(Node_t *pmail_root)
{
    Node_t *ptmp = NULL;

    if (NULL == pmail_root)
    {
        return;
    }

    /* 第一步：唤醒所有还在等的线程（配合 recv 里的超时，双保险） */
    for (ptmp = pmail_root->pNext; NULL != ptmp; ptmp = ptmp->pNext)
    {
        sem_post(&ptmp->Data.sem);
    }

    /* 第二步：等所有线程真正退出，join 返回才说明它不再碰队列了 */
    for (ptmp = pmail_root->pNext; NULL != ptmp; ptmp = ptmp->pNext)
    {
        pthread_join(ptmp->Data.tid, NULL);
    }

    /* 第三步：释放队列和同步对象 */
    for (ptmp = pmail_root->pNext; NULL != ptmp; ptmp = ptmp->pNext)
    {
        int left = GetQueueCount(ptmp->Data.pque);

        if (left > 0)
        {
            LOG_WARN("[mailbox] %s 退出时还剩 %d 条未处理",
                     ptmp->Data.name, left);
        }

        DestroyLinkQueue(&ptmp->Data.pque);
        pthread_mutex_destroy(&ptmp->Data.mutex);
        sem_destroy(&ptmp->Data.sem);
    }

    DestroyLinkList(&pmail_root);

    LOG_INFO("[mailbox] 邮箱系统已销毁");   /* 能看到这行，就说明清理走完了 */
}

/****************************************************************
 * 发消息：找到接收者 -> 入队 -> 通知
 ****************************************************************/
int send_mail_msg(char *send_name, struct msgdata data, char *recv_name)
{
    Node_t   *ptmp = NULL;
    QDataType qdata;

    if (NULL == pmail || NULL == recv_name)
    {
        return -1;
    }

    for (ptmp = pmail->pNext; NULL != ptmp; ptmp = ptmp->pNext)
    {
        if (0 == strcmp(ptmp->Data.name, recv_name))
        {
            break;
        }
    }

    if (NULL == ptmp)
    {
        LOG_WARN("Not recver : %s", recv_name);
        return -1;
    }

    snprintf(qdata.send_name, sizeof(qdata.send_name), "%s",
             (NULL == send_name) ? "unknown" : send_name);
    snprintf(qdata.recv_name, sizeof(qdata.recv_name), "%s", recv_name);
    qdata.data = data;

    pthread_mutex_lock(&ptmp->Data.mutex);
    EnterLinkQueue(ptmp->Data.pque, qdata);
    pthread_mutex_unlock(&ptmp->Data.mutex);

    sem_post(&ptmp->Data.sem);

    return 0;
}

/****************************************************************
 * 收消息：按 pthread_self() 认领自己的队列
 * 返回 0 = 收到一条；其他 = 没收到（超时 / 正在退出）
 ****************************************************************/
int recv_mail_msg_timeout(struct msgdata *mydata, char *send_name, int timeout_ms)
{
    pthread_t mytid = pthread_self();
    Node_t   *ptmp  = NULL;
    QDataType qdata;
    int       ret;

    if (NULL == pmail || NULL == mydata)
    {
        return -1;
    }

    /* 退出信号已经来了，别再去动队列，直接返回让上层跳出循环 */
    if (!g_running)
    {
        return -2;
    }

    for (ptmp = pmail->pNext; NULL != ptmp; ptmp = ptmp->pNext)
    {
        if (ptmp->Data.tid == mytid)
        {
            break;
        }
    }

    if (NULL == ptmp)
    {
        LOG_WARN("Not this thread");
        return -1;
    }

    /* 最多等 timeout_ms 毫秒。用带超时的等待，
     * 是为了不用额外条件变量也能周期性地看一眼 g_running；
     * timeout_ms = 0 时退化成"只看一眼"，UI 线程就是靠它边刷界面边取数据 */
    if (timeout_ms > 0)
    {
        struct timespec ts;

        if (0 != clock_gettime(CLOCK_REALTIME, &ts))
        {
            return -1;
        }

        ts.tv_nsec += (long)timeout_ms * 1000000L;
        if (ts.tv_nsec >= 1000000000L)
        {
            ts.tv_sec  += ts.tv_nsec / 1000000000L;
            ts.tv_nsec %= 1000000000L;
        }

        while (0 != sem_timedwait(&ptmp->Data.sem, &ts))
        {
            if (EINTR == errno)
            {
                continue;               /* 被信号打断，接着等剩下的时间 */
            }
            return -1;                  /* ETIMEDOUT：回去检查 g_running */
        }
    }
    else
    {
        if (0 != sem_trywait(&ptmp->Data.sem))
        {
            return -1;                  /* 队列里没东西，立刻返回 */
        }
    }

    pthread_mutex_lock(&ptmp->Data.mutex);
    ret = PopLinkQueue(ptmp->Data.pque, &qdata);
    pthread_mutex_unlock(&ptmp->Data.mutex);

    if (0 != ret)
    {
        return -1;
    }

    *mydata = qdata.data;

    if (NULL != send_name)
    {
        snprintf(send_name, sizeof(qdata.send_name), "%s", qdata.send_name);
    }

    return 0;
}

int recv_mail_msg(struct msgdata *mydata, char *send_name)
{
    return recv_mail_msg_timeout(mydata, send_name, MAIL_WAIT_MS);
}
