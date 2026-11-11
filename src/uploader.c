#include "uploader.h"
#include "config.h"
#include "alarm.h"
#include "logger.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>

#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

/*------------------------------------------------------------------
 * 网络上报
 *
 * 原来是 send_data 线程里 printf 一句就完事了，等于没上报。
 * 这里补成真正的 TCP 长连接客户端：
 *
 *   连接：非阻塞 connect + select 超时，避免 4G 模块没插卡时卡死线程
 *   发送：send_all 处理短写，MSG_NOSIGNAL 避免对端关连接时被 SIGPIPE 打死
 *   重连：1s 起，翻倍退避，封顶 30s，不会把 CPU 打满
 *   断网：数据先进本地环形缓存，连上以后按时间顺序补发，
 *         这样短时间断网不会丢点，长断网也不会把内存吃光
 *
 * 上报文本是行协议，一行一条，方便用 nc / python 做对端调试：
 *   HELLO,1,basestation-monitor,1.0
 *   DATA,1,2026-11-11T23:51:10,25.30,45.00,48.62,2.41,120,0x00,0
 *   ALARM,1,2026-11-11T23:51:10,0x04,2,电压偏低
 *----------------------------------------------------------------*/

#define UPLOAD_BACKOFF_MIN      1
#define UPLOAD_BACKOFF_MAX      30
#define UPLOAD_LINE_MAX         256

static int           g_sock       = -1;
static int           g_backoff    = UPLOAD_BACKOFF_MIN;
static time_t        g_next_retry = 0;

static unsigned long g_sent_cnt   = 0;
static unsigned long g_fail_cnt   = 0;

/* 断网缓存：环形，满了覆盖最旧的 */
static struct msgdata g_offline[UPLOAD_OFFLINE_MAX];
static int            g_off_head  = 0;
static int            g_off_count = 0;

int uploader_offline_count(void)
{
    return g_off_count;
}

unsigned long uploader_sent_count(void)
{
    return g_sent_cnt;
}

unsigned long uploader_fail_count(void)
{
    return g_fail_cnt;
}

int uploader_connected(void)
{
    return (g_sock >= 0) ? 1 : 0;
}

/****************************************************************
 * 离线缓存
 ****************************************************************/
static void offline_push(const struct msgdata *m)
{
    if (g_off_count < UPLOAD_OFFLINE_MAX)
    {
        int pos = (g_off_head + g_off_count) % UPLOAD_OFFLINE_MAX;
        g_offline[pos] = *m;
        g_off_count++;
        return;
    }

    /* 满了：覆盖最早那条，同时把尾巴挪一格。
     * 条数少（256），直接整体挪比维护双向索引省事也够快 */
    g_offline[g_off_head] = *m;
    g_off_head = (g_off_head + 1) % UPLOAD_OFFLINE_MAX;

    if (0 == (g_fail_cnt % 60))
    {
        LOG_WARN("[uploader] 离线缓存已满(%d)，覆盖最旧数据", UPLOAD_OFFLINE_MAX);
    }
}

static int offline_pop(struct msgdata *out)
{
    if (g_off_count <= 0)
    {
        return -1;
    }

    *out = g_offline[g_off_head];
    g_off_head = (g_off_head + 1) % UPLOAD_OFFLINE_MAX;
    g_off_count--;

    return 0;
}

/* 只看最旧那条，不动队列。
 * 补发的时候必须先看、发成功了再 pop，
 * 否则"pop 出来发失败又 push 回去"会把这条挪到队尾，时间顺序就乱了 */
static int offline_peek(struct msgdata *out)
{
    if (g_off_count <= 0)
    {
        return -1;
    }

    *out = g_offline[g_off_head];

    return 0;
}

/****************************************************************
 * 非阻塞 connect + select 超时
 ****************************************************************/
static int try_connect(void)
{
    struct addrinfo hints;
    struct addrinfo *res  = NULL;
    struct addrinfo *p    = NULL;
    char             port[16];
    int              fd   = -1;
    int              rc;

    snprintf(port, sizeof(port), "%d", g_cfg.upload_port);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;      /* 4G 下 v6 不一定通，两种都试 */
    hints.ai_socktype = SOCK_STREAM;

    rc = getaddrinfo(g_cfg.upload_host, port, &hints, &res);
    if (0 != rc)
    {
        LOG_WARN("[uploader] 域名解析 %s 失败: %s",
                 g_cfg.upload_host, gai_strerror(rc));
        return -1;
    }

    for (p = res; NULL != p; p = p->ai_next)
    {
        int       flags;
        int       so_err = 0;
        socklen_t so_len = (socklen_t)sizeof(so_err);

        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0)
        {
            continue;
        }

        flags = fcntl(fd, F_GETFL, 0);
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        rc = connect(fd, p->ai_addr, (socklen_t)p->ai_addrlen);
        if (0 != rc && EINPROGRESS == errno)
        {
            fd_set         wfds;
            struct timeval tv;
            int            r;

            FD_ZERO(&wfds);
            FD_SET(fd, &wfds);
            tv.tv_sec  = UPLOAD_CONNECT_TO_S;
            tv.tv_usec = 0;

            r = select(fd + 1, NULL, &wfds, NULL, &tv);
            if (r > 0)
            {
                rc = getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &so_len);
                if (0 == rc && 0 == so_err)
                {
                    rc = 0;                 /* 连上了 */
                }
                else
                {
                    rc = -1;
                }
            }
            else
            {
                rc = -1;                    /* 超时 */
            }
        }

        if (0 == rc)
        {
            break;                          /* 成功，跳出 */
        }

        close(fd);
        fd = -1;
    }

    if (res != NULL)
    {
        freeaddrinfo(res);
    }

    if (fd < 0)
    {
        return -1;
    }

    /* 恢复阻塞模式，并设置发送超时：对端不收的时候 send 会返回 -1 而不是永远卡住 */
    {
        int flags = fcntl(fd, F_GETFL, 0);
        struct timeval snd_to;

        (void)fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

        snd_to.tv_sec  = 2;
        snd_to.tv_usec = 0;
        (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd_to, sizeof(snd_to));

        /* 采集数据都很小，关掉 Nagle 让每帧立刻出去 */
        {
            int one = 1;
            (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        }
    }

    return fd;
}

/****************************************************************
 * 发完一整段；处理短写和 EINTR
 ****************************************************************/
static int send_all(int fd, const char *buf, int len)
{
    int sent = 0;

    while (sent < len)
    {
        ssize_t n = send(fd, buf + sent, (size_t)(len - sent), MSG_NOSIGNAL);

        if (n < 0)
        {
            if (EINTR == errno)
            {
                continue;
            }
            return -1;
        }
        if (0 == n)
        {
            return -1;
        }

        sent += (int)n;
    }

    return 0;
}

static void drop_conn(const char *why)
{
    if (g_sock >= 0)
    {
        close(g_sock);
        g_sock = -1;
    }

    g_next_retry = time(NULL) + g_backoff;
    g_backoff = (g_backoff * 2 > UPLOAD_BACKOFF_MAX) ? UPLOAD_BACKOFF_MAX
                                                     : g_backoff * 2;

    LOG_WARN("[uploader] 连接断开(%s)，%d 秒后重连",
             (NULL == why) ? "未知" : why, g_backoff);
}

/****************************************************************
 * 组一行上报文本
 ****************************************************************/
static int format_line(const struct msgdata *m, char *buf, size_t cap)
{
    char ts[32]   = {0};
    char text[128] = {0};

    if (0 == strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &m->tim))
    {
        snprintf(ts, sizeof(ts), "1970-01-01T00:00:00");
    }

    (void)alarm_text(m->alarm, text, (int)sizeof(text));

    if (0 != m->alarm)
    {
        return snprintf(buf, cap,
                        "ALARM,%d,%s,0x%02X,%d,%s,V=%.2f,I=%.2f,T=%.1f\n",
                        m->id, ts, m->alarm, m->level, text,
                        m->v, m->a, m->temp);
    }

    return snprintf(buf, cap,
                    "DATA,%d,%s,%.2f,%.2f,%.2f,%.2f,%.0f,0x%02X,%d\n",
                    m->id, ts, m->temp, m->hum, m->v, m->a, m->smoke,
                    m->alarm, m->level);
}

/* 真发一条（调用前保证 g_sock >= 0） */
static int send_one(const struct msgdata *m)
{
    char line[UPLOAD_LINE_MAX];
    int  n;

    n = format_line(m, line, sizeof(line));
    if (n <= 0 || n >= (int)sizeof(line))
    {
        return -1;
    }

    if (0 != send_all(g_sock, line, n))
    {
        return -1;
    }

    return 0;
}

int uploader_init(void)
{
    if (0 == g_cfg.upload_enable)
    {
        LOG_INFO("[uploader] 上报功能已关闭（upload_enable = 0）");
        return 0;
    }

    if ('\0' == g_cfg.upload_host[0] || g_cfg.upload_port <= 0)
    {
        LOG_ERROR("[uploader] 上报地址配置不完整，功能关闭");
        g_cfg.upload_enable = 0;
        return -1;
    }

    g_sock       = -1;
    g_backoff    = UPLOAD_BACKOFF_MIN;
    g_next_retry = 0;
    g_sent_cnt   = 0;
    g_fail_cnt   = 0;
    g_off_head   = 0;
    g_off_count  = 0;

    LOG_INFO("[uploader] 目标 %s:%d，首次连接在第 1 条数据到达时发起",
             g_cfg.upload_host, g_cfg.upload_port);

    return 0;
}

/****************************************************************
 * 确保连接可用。返回 0 表示可以发，-1 表示暂时发不出去
 ****************************************************************/
static int ensure_connected(void)
{
    time_t now;

    if (g_sock >= 0)
    {
        return 0;
    }

    now = time(NULL);
    if (now < g_next_retry)
    {
        return -1;                      /* 还在退避期里，别急着再试 */
    }

    g_sock = try_connect();
    if (g_sock < 0)
    {
        g_next_retry = now + g_backoff;
        g_backoff = (g_backoff * 2 > UPLOAD_BACKOFF_MAX) ? UPLOAD_BACKOFF_MAX
                                                         : g_backoff * 2;
        return -1;
    }

    g_backoff = UPLOAD_BACKOFF_MIN;
    LOG_INFO("[uploader] 已连接 %s:%d", g_cfg.upload_host, g_cfg.upload_port);

    /* 握个手，让上位机知道是谁上来了 */
    {
        char hello[128];
        int  n = snprintf(hello, sizeof(hello),
                          "HELLO,%d,basestation-monitor,1.0\n", g_cfg.dev_id);

        if (n > 0 && 0 != send_all(g_sock, hello, n))
        {
            drop_conn("握手失败");
            return -1;
        }
    }

    return 0;
}

int uploader_flush_offline(void)
{
    struct msgdata m;
    int            n = 0;

    if (0 == g_cfg.upload_enable || g_sock < 0)
    {
        return 0;
    }

    while (0 == offline_peek(&m))
    {
        if (0 != send_one(&m))
        {
            /* 又断了，直接停，缓存不动，下次连上再从这条开始补 */
            drop_conn("补发失败");
            return n;
        }

        (void)offline_pop(&m);          /* 发成功了才真正出队 */
        n++;
        g_sent_cnt++;
    }

    if (n > 0)
    {
        LOG_INFO("[uploader] 断网缓存补发完成 %d 条，剩余 %d 条",
                 n, g_off_count);
    }

    return n;
}

int uploader_send(const struct msgdata *m)
{
    if (NULL == m)
    {
        return -1;
    }

    if (0 == g_cfg.upload_enable)
    {
        return 0;
    }

    if (0 != ensure_connected())
    {
        offline_push(m);
        g_fail_cnt++;
        return 0;                       /* 进了缓存，不算丢 */
    }

    /* 先把欠的旧数据补上，保证上位机看到的是时间顺序 */
    if (g_off_count > 0)
    {
        (void)uploader_flush_offline();

        if (g_sock < 0)
        {
            offline_push(m);
            g_fail_cnt++;
            return 0;
        }
    }

    if (0 != send_one(m))
    {
        offline_push(m);
        g_fail_cnt++;
        drop_conn("发送失败");
        return 0;
    }

    g_sent_cnt++;
    LOG_DEBUG("[uploader] 已上报 dev=%d V=%.2f I=%.2f alarm=0x%02X",
              m->id, m->v, m->a, m->alarm);

    return 0;
}

void uploader_close(void)
{
    if (g_sock >= 0)
    {
        close(g_sock);
        g_sock = -1;
    }

    LOG_INFO("[uploader] 已关闭，累计上报 %lu 条，失败 %lu 次，残留缓存 %d 条",
             g_sent_cnt, g_fail_cnt, g_off_count);
}
