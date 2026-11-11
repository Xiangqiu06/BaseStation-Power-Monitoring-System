#include "mqtt.h"
#include "config.h"
#include "alarm.h"
#include "logger.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <sys/time.h>

#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>

/*------------------------------------------------------------------
 * MQTT 3.1.1 客户端
 *
 * 报文结构（记住这个就能读懂全部代码）：
 *
 *   固定头  [type<<4 | flags] [剩余长度 varint 1~4 字节]
 *   可变头  按报文类型不同（CONNECT 里是协议名/版本/标志/保活）
 *   负载    topic、packet id、真正的数据
 *
 * 剩余长度是变长整数：每字节低 7 位是数据，最高位表示"后面还有"。
 * 128 以内 1 字节，我们发的报文基本都在 1~2 字节范围内。
 *
 * 为什么不引 paho：板子上是 arm-linux-gnueabihf，paho 还带 openssl，
 * 交叉编译一轮要半天；我们只要"连上、发布、收配置"三件事，自己封更省事。
 *----------------------------------------------------------------*/

#define MQTT_KEEPALIVE      60      /* 秒 */

#define PKT_CONNECT     1
#define PKT_CONNACK     2
#define PKT_PUBLISH     3
#define PKT_PUBACK      4
#define PKT_SUBSCRIBE   8
#define PKT_SUBACK      9
#define PKT_PINGREQ     12
#define PKT_PINGRESP    13
#define PKT_DISCONNECT  14

static int    g_sock       = -1;
static int    g_pkt_id     = 0;
static int    g_backoff    = 1;
static time_t g_next_try   = 0;
static time_t g_last_ping  = 0;
static time_t g_last_rx    = 0;
static int    g_reconnects = 0;
static int    g_rx_hdr     = 2;     /* 最近一次收报文的固定头长度 */

static unsigned long g_pub_cnt  = 0;
static unsigned long g_fail_cnt = 0;

static char g_client_id[48];
static char g_topic_data[96];
static char g_topic_alarm[96];
static char g_topic_cmd[96];

unsigned long mqtt_pub_count(void)       { return g_pub_cnt; }
unsigned long mqtt_fail_count(void)      { return g_fail_cnt; }
int           mqtt_reconnect_count(void) { return g_reconnects; }
int           mqtt_connected(void)       { return (g_sock >= 0) ? 1 : 0; }

/*================================================================
 * 编码小工具
 *================================================================*/
static int mqtt_encode_len(unsigned char *p, int len)
{
    int n = 0;

    do
    {
        unsigned char b = (unsigned char)(len % 128);

        len /= 128;
        if (len > 0)
        {
            b |= 0x80;
        }
        p[n++] = b;
    } while (len > 0 && n < 4);

    return n;
}

static int mqtt_write_u16(unsigned char *p, unsigned short v)
{
    p[0] = (unsigned char)((v >> 8) & 0xFF);
    p[1] = (unsigned char)(v & 0xFF);
    return 2;
}

/*================================================================
 * 底层收发
 *================================================================*/
static int sock_send_all(const char *buf, int len)
{
    int sent = 0;

    while (sent < len)
    {
        ssize_t n = send(g_sock, buf + sent, (size_t)(len - sent), MSG_NOSIGNAL);

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

static void mqtt_drop(const char *why)
{
    if (g_sock >= 0)
    {
        close(g_sock);
        g_sock = -1;
    }

    g_next_try = time(NULL) + g_backoff;
    g_backoff  = (g_backoff * 2 > 30) ? 30 : g_backoff * 2;

    if (NULL != why)
    {
        LOG_WARN("[mqtt] 连接断开(%s)，%d 秒后重连", why, g_backoff);
    }
}

/****************************************************************
 * 等到可读。返回 1 可读 / 0 超时 / -1 出错
 ****************************************************************/
static int wait_readable(int timeout_ms)
{
    fd_set         rfds;
    struct timeval tv;
    int            r;

    if (timeout_ms < 0)
    {
        timeout_ms = 0;
    }

    FD_ZERO(&rfds);
    FD_SET(g_sock, &rfds);

    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    r = select(g_sock + 1, &rfds, NULL, NULL, &tv);
    if (r < 0)
    {
        return (EINTR == errno) ? 0 : -1;
    }

    return (r > 0) ? 1 : 0;
}

/****************************************************************
 * 收一个完整报文
 *
 * 返回整个报文长度；0 表示在超时时间内没有报文（poll 模式常见）；
 * -1 表示连接出错。
 *
 * 逐字节收是有意为之：剩余长度是变长的，先算清楚总长再一次性收，
 * 不然半包/粘包的边界很容易算错。
 ****************************************************************/
static int mqtt_recv_packet(unsigned char *buf, int cap, int timeout_ms)
{
    struct timeval t0;
    int            pos = 0;
    int            len = 0;
    int            mult = 1;
    int            i;

    if (g_sock < 0 || NULL == buf || cap < 2)
    {
        return -1;
    }

    gettimeofday(&t0, NULL);

    /* 1) 类型字节 */
    {
        int r = wait_readable(timeout_ms);

        if (r < 0)
        {
            return -1;
        }
        if (0 == r)
        {
            return 0;                   /* 没数据 */
        }

        {
            ssize_t n = recv(g_sock, buf, 1, 0);

            if (n <= 0)
            {
                return -1;
            }
        }
        pos = 1;
    }

    /* 2) 剩余长度（最多 4 字节） */
    for (i = 0; i < 4; i++)
    {
        ssize_t n;

        if (pos >= cap)
        {
            return -1;
        }
        if (0 == wait_readable(1000))
        {
            return -1;
        }

        n = recv(g_sock, buf + pos, 1, 0);
        if (n <= 0)
        {
            return -1;
        }

        len += (buf[pos] & 0x7F) * mult;
        mult *= 128;
        pos++;

        if (0 == (buf[pos - 1] & 0x80))
        {
            break;
        }
    }

    g_rx_hdr = pos;

    if (pos + len > cap)
    {
        LOG_WARN("[mqtt] 报文超长: 头 %d + 体 %d > %d", pos, len, cap);
        return -1;
    }

    /* 3) 可变头 + 负载 */
    while (len > 0)
    {
        ssize_t n;

        if (0 == wait_readable(1000))
        {
            return -1;
        }

        n = recv(g_sock, buf + pos, (size_t)len, 0);
        if (n <= 0)
        {
            return -1;
        }

        pos += (int)n;
        len -= (int)n;
    }

    return pos;
}

/*================================================================
 * 连接 + 握手
 *================================================================*/
static int mqtt_tcp_connect(void)
{
    struct addrinfo  hints;
    struct addrinfo *res = NULL;
    struct addrinfo *p   = NULL;
    char             port[16];
    int              fd  = -1;
    int              rc;

    snprintf(port, sizeof(port), "%d", g_cfg.mqtt_port);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    rc = getaddrinfo(g_cfg.mqtt_host, port, &hints, &res);
    if (0 != rc)
    {
        LOG_WARN("[mqtt] 解析 %s 失败: %s", g_cfg.mqtt_host, gai_strerror(rc));
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
            tv.tv_sec  = 4;
            tv.tv_usec = 0;

            r = select(fd + 1, NULL, &wfds, NULL, &tv);
            if (r > 0)
            {
                rc = getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &so_len);
                rc = (0 == rc && 0 == so_err) ? 0 : -1;
            }
            else
            {
                rc = -1;
            }
        }

        if (0 == rc)
        {
            break;
        }

        close(fd);
        fd = -1;
    }

    if (NULL != res)
    {
        freeaddrinfo(res);
    }

    if (fd < 0)
    {
        return -1;
    }

    {
        int            flags = fcntl(fd, F_GETFL, 0);
        struct timeval to;

        (void)fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

        to.tv_sec  = 3;
        to.tv_usec = 0;
        (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof(to));
    }

    {
        int one = 1;
        (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }

    return fd;
}

/****************************************************************
 * CONNECT + 等 CONNACK
 *
 * 固定头: 0x10 | 剩余长度
 * 变量头: 00 04 "MQTT" | 04(3.1.1) | 02(clean session) | 保活
 * 负载  : 客户端 ID
 ****************************************************************/
static int mqtt_handshake(void)
{
    unsigned char pkt[MQTT_PKT_MAX];
    unsigned char rsp[MQTT_PKT_MAX];
    int           len = 0;
    int           body;
    int           cid_len;
    int           n;

    cid_len = (int)strlen(g_client_id);
    body    = 10 + 2 + cid_len;

    pkt[len++] = 0x10;
    len += mqtt_encode_len(&pkt[len], body);

    pkt[len++] = 0x00;                  /* 协议名 "MQTT" */
    pkt[len++] = 0x04;
    pkt[len++] = 'M';
    pkt[len++] = 'Q';
    pkt[len++] = 'T';
    pkt[len++] = 'T';
    pkt[len++] = 0x04;                  /* 协议级别 4 = 3.1.1 */
    pkt[len++] = 0x02;                  /* clean session */
    len += mqtt_write_u16(&pkt[len], MQTT_KEEPALIVE);

    len += mqtt_write_u16(&pkt[len], (unsigned short)cid_len);
    memcpy(&pkt[len], g_client_id, (size_t)cid_len);
    len += cid_len;

    if (0 != sock_send_all((const char *)pkt, len))
    {
        return -1;
    }

    n = mqtt_recv_packet(rsp, (int)sizeof(rsp), 4000);
    if (n < 4)
    {
        LOG_WARN("[mqtt] 没收到 CONNACK (n=%d)", n);
        return -1;
    }

    if (0x20 != rsp[0])
    {
        LOG_WARN("[mqtt] 应答不是 CONNACK: 0x%02X", rsp[0]);
        return -1;
    }

    if (0 != rsp[3])
    {
        LOG_ERROR("[mqtt] 被 broker 拒绝，返回码 %d "
                  "(1=协议版本 2=客户端ID 3=服务不可用 4=账号密码错 5=未授权)",
                  rsp[3]);
        return -1;
    }

    return 0;
}

static int mqtt_subscribe_cmd(void)
{
    unsigned char pkt[MQTT_PKT_MAX];
    unsigned char rsp[MQTT_PKT_MAX];
    int           len = 0;
    int           body;
    int           tlen;
    int           n;

    tlen = (int)strlen(g_topic_cmd);
    body = 2 + 2 + tlen + 1;

    pkt[len++] = (unsigned char)(0x80 | 0x02);   /* SUBSCRIBE，flags 固定 0x02 */
    len += mqtt_encode_len(&pkt[len], body);

    len += mqtt_write_u16(&pkt[len], (unsigned short)(++g_pkt_id));
    len += mqtt_write_u16(&pkt[len], (unsigned short)tlen);
    memcpy(&pkt[len], g_topic_cmd, (size_t)tlen);
    len += tlen;
    pkt[len++] = 0x01;                  /* 请求 QoS1 */

    if (0 != sock_send_all((const char *)pkt, len))
    {
        return -1;
    }

    n = mqtt_recv_packet(rsp, (int)sizeof(rsp), 3000);
    if (n < 4 || 0x90 != rsp[0])
    {
        LOG_WARN("[mqtt] 订阅应答异常（n=%d）", n);
        return -1;
    }

    LOG_INFO("[mqtt] 已订阅 %s", g_topic_cmd);
    return 0;
}

int mqtt_init(void)
{
    if (0 == g_cfg.mqtt_enable)
    {
        LOG_INFO("[mqtt] 未开启（mqtt_enable = 0）");
        return -1;
    }

    if ('\0' == g_cfg.mqtt_host[0] || g_cfg.mqtt_port <= 0)
    {
        LOG_ERROR("[mqtt] 服务器地址配置不完整");
        return -1;
    }

    snprintf(g_client_id, sizeof(g_client_id), "basestation-%d", g_cfg.dev_id);
    snprintf(g_topic_data, sizeof(g_topic_data),
             "basestation/%d/telemetry", g_cfg.dev_id);
    snprintf(g_topic_alarm, sizeof(g_topic_alarm),
             "basestation/%d/alarm", g_cfg.dev_id);
    snprintf(g_topic_cmd, sizeof(g_topic_cmd),
             "basestation/%d/cmd", g_cfg.dev_id);

    g_sock     = -1;
    g_backoff  = 1;
    g_next_try = 0;

    LOG_INFO("[mqtt] 目标 %s:%d，client_id=%s，保活 %ds",
             g_cfg.mqtt_host, g_cfg.mqtt_port, g_client_id, MQTT_KEEPALIVE);
    LOG_INFO("[mqtt] 主题 telemetry=%s  alarm=%s  cmd=%s",
             g_topic_data, g_topic_alarm, g_topic_cmd);

    return 0;
}

static int ensure_connected(void)
{
    time_t now;

    if (g_sock >= 0)
    {
        return 0;
    }

    now = time(NULL);
    if (now < g_next_try)
    {
        return -1;
    }

    g_sock = mqtt_tcp_connect();
    if (g_sock < 0)
    {
        g_next_try = now + g_backoff;
        g_backoff  = (g_backoff * 2 > 30) ? 30 : g_backoff * 2;
        return -1;
    }

    if (0 != mqtt_handshake())
    {
        mqtt_drop("握手失败");
        return -1;
    }

    g_backoff   = 1;
    g_last_ping = time(NULL);
    g_last_rx   = time(NULL);
    g_reconnects++;

    LOG_INFO("[mqtt] 已连上 %s:%d（第 %d 次连接）",
             g_cfg.mqtt_host, g_cfg.mqtt_port, g_reconnects);

    (void)mqtt_subscribe_cmd();         /* 订阅失败不致命，数据还能发 */

    return 0;
}

/*================================================================
 * 发布
 *================================================================*/
int mqtt_publish_raw(const char *topic, const char *payload, int qos)
{
    unsigned char pkt[MQTT_PKT_MAX];
    int           len  = 0;
    int           body = 0;
    int           tlen;
    int           plen;
    int           pid = 0;

    if (NULL == topic || NULL == payload)
    {
        return -1;
    }

    if (0 != ensure_connected())
    {
        g_fail_cnt++;
        return -1;
    }

    tlen = (int)strlen(topic);
    plen = (int)strlen(payload);
    body = 2 + tlen + (qos > 0 ? 2 : 0) + plen;

    if (body + 5 > (int)sizeof(pkt))
    {
        LOG_WARN("[mqtt] 报文太长(%d)，丢弃", body);
        g_fail_cnt++;
        return -1;
    }

    pkt[len++] = (unsigned char)((PKT_PUBLISH << 4) | (qos > 0 ? 0x02 : 0x00));
    len += mqtt_encode_len(&pkt[len], body);

    len += mqtt_write_u16(&pkt[len], (unsigned short)tlen);
    memcpy(&pkt[len], topic, (size_t)tlen);
    len += tlen;

    if (qos > 0)
    {
        pid  = ++g_pkt_id;
        len += mqtt_write_u16(&pkt[len], (unsigned short)pid);
    }

    memcpy(&pkt[len], payload, (size_t)plen);
    len += plen;

    if (0 != sock_send_all((const char *)pkt, len))
    {
        g_fail_cnt++;
        mqtt_drop("发送失败");
        return -1;
    }

    g_pub_cnt++;
    g_last_rx = time(NULL);

    if (qos > 0)
    {
        unsigned char rsp[MQTT_PKT_MAX];
        int           n = mqtt_recv_packet(rsp, (int)sizeof(rsp), 1500);

        if (n < 4 || 0x40 != rsp[0])
        {
            LOG_DEBUG("[mqtt] 没等到 PUBACK（pid=%d, n=%d）", pid, n);
        }
    }

    return 0;
}

int mqtt_publish(const struct msgdata *m)
{
    char payload[MQTT_PKT_MAX];
    char ts[32];
    char text[128];
    int  n;

    if (NULL == m)
    {
        return -1;
    }

    if (0 == strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &m->tim))
    {
        snprintf(ts, sizeof(ts), "1970-01-01T00:00:00");
    }

    if (0 != m->alarm)
    {
        (void)alarm_text(m->alarm, text, (int)sizeof(text));

        /* 告警单独主题：云端可以直接挂规则引擎推短信 */
        n = snprintf(payload, sizeof(payload),
                     "{\"dev\":%d,\"bat\":%d,\"ts\":\"%s\",\"level\":%d,"
                     "\"flags\":%u,\"detail\":\"%s\",\"v\":%.2f,\"a\":%.2f,"
                     "\"t\":%.2f,\"soc\":%.1f}",
                     m->id, m->bat_id, ts, m->level, m->alarm, text,
                     m->v, m->a, m->temp, m->soc);

        if (n <= 0 || n >= (int)sizeof(payload))
        {
            return -1;
        }

        return mqtt_publish_raw(g_topic_alarm, payload, 1);   /* 告警用 QoS1 */
    }

    n = snprintf(payload, sizeof(payload),
                 "{\"dev\":%d,\"bat\":%d,\"ts\":\"%s\",\"v\":%.2f,\"a\":%.2f,"
                 "\"t\":%.2f,\"h\":%.2f,\"smoke\":%.0f,\"soc\":%.1f,\"soh\":%.1f,"
                 "\"alarm\":%u,\"level\":%d}",
                 m->id, m->bat_id, ts, m->v, m->a, m->temp, m->hum,
                 m->smoke, m->soc, m->soh, m->alarm, m->level);

    if (n <= 0 || n >= (int)sizeof(payload))
    {
        return -1;
    }

    /* 采集数据用 QoS0：丢一两帧无所谓，
     * QoS1 每条都要等 PUBACK，1Hz 下会把链路拖慢 */
    return mqtt_publish_raw(g_topic_data, payload, 0);
}

/*================================================================
 * 下行：SET key=value
 *================================================================*/
static void mqtt_handle_cmd(const char *payload)
{
    char  buf[192];
    char  key[48];
    char  val[48];
    char *eq = NULL;
    char *p  = NULL;

    snprintf(buf, sizeof(buf), "%s", payload);

    p = strpbrk(buf, "\r\n");
    if (NULL != p)
    {
        *p = '\0';
    }

    if (0 == strncmp(buf, "GET", 3))
    {
        LOG_INFO("[mqtt] 云端查询阈值: V=[%.2f,%.2f] T<=%.1f H<=%.1f I<=%.2f S<=%.0f",
                 g_cfg.volt_min, g_cfg.volt_max, g_cfg.temp_max,
                 g_cfg.hum_max, g_cfg.curr_max, g_cfg.smoke_max);
        return;
    }

    if (0 != strncmp(buf, "SET ", 4))
    {
        LOG_WARN("[mqtt] 不认识的命令: %s", buf);
        return;
    }

    eq = strchr(buf + 4, '=');
    if (NULL == eq)
    {
        LOG_WARN("[mqtt] 命令格式应为 SET key=value: %s", buf);
        return;
    }

    *eq = '\0';
    snprintf(key, sizeof(key), "%s", buf + 4);
    snprintf(val, sizeof(val), "%s", eq + 1);

    if (0 == config_set(key, val))
    {
        LOG_INFO("[mqtt] 云端下发配置生效: %s = %s", key, val);
    }
    else
    {
        LOG_WARN("[mqtt] 云端下发了未知配置项: %s", key);
    }
}

/*================================================================
 * 周期任务
 *================================================================*/
void mqtt_poll(void)
{
    unsigned char pkt[MQTT_PKT_MAX];
    time_t        now;
    int           n;

    if (0 == g_cfg.mqtt_enable || g_sock < 0)
    {
        return;
    }

    n = mqtt_recv_packet(pkt, (int)sizeof(pkt), 0);   /* 只看一眼，不等 */

    if (n < 0)
    {
        mqtt_drop("读失败");
        return;
    }

    if (n > 0)
    {
        g_last_rx = time(NULL);

        switch (pkt[0] >> 4)
        {
            case PKT_PUBLISH:
            {
                int off  = g_rx_hdr;
                int tlen;
                int qos;

                if (off + 2 > n)
                {
                    break;
                }

                tlen = (pkt[off] << 8) | pkt[off + 1];
                off += 2;
                off += tlen;

                qos = (pkt[0] >> 1) & 0x03;

                if (qos > 0)
                {
                    off += 2;           /* packet id */
                }

                if (off < n)
                {
                    char body[MQTT_PKT_MAX];
                    int  blen = n - off;

                    if (blen >= (int)sizeof(body))
                    {
                        blen = (int)sizeof(body) - 1;
                    }
                    memcpy(body, pkt + off, (size_t)blen);
                    body[blen] = '\0';

                    mqtt_handle_cmd(body);

                    if (1 == qos)       /* QoS1 要回 PUBACK，不然 broker 一直重发 */
                    {
                        unsigned char ack[4];
                        int           pid = (pkt[off - 2] << 8) | pkt[off - 1];

                        ack[0] = (PKT_PUBACK << 4);
                        ack[1] = 0x02;
                        mqtt_write_u16(&ack[2], (unsigned short)pid);
                        (void)sock_send_all((const char *)ack, 4);
                    }
                }
                break;
            }

            case PKT_PINGRESP:  LOG_DEBUG("[mqtt] PINGRESP"); break;
            case PKT_SUBACK:    LOG_DEBUG("[mqtt] SUBACK");   break;
            case PKT_PUBACK:    LOG_DEBUG("[mqtt] PUBACK");   break;

            default:
                LOG_DEBUG("[mqtt] 收到报文类型 %d", pkt[0] >> 4);
                break;
        }
    }

    now = time(NULL);

    /* 半个保活周期发一次心跳，留够重传余量 */
    if (now - g_last_ping >= (MQTT_KEEPALIVE / 2))
    {
        unsigned char ping[2] = { (PKT_PINGREQ << 4), 0x00 };

        if (0 != sock_send_all((const char *)ping, 2))
        {
            mqtt_drop("PINGREQ 失败");
            return;
        }

        g_last_ping = now;
        LOG_DEBUG("[mqtt] 已发 PINGREQ");
    }

    /* broker 两个保活周期以上没动静，当作链路已死 */
    if (now - g_last_rx > (time_t)(MQTT_KEEPALIVE * 2 + 30))
    {
        mqtt_drop("超时无响应");
    }
}

void mqtt_close(void)
{
    if (g_sock >= 0)
    {
        unsigned char bye[2] = { (PKT_DISCONNECT << 4), 0x00 };

        (void)sock_send_all((const char *)bye, 2);
        close(g_sock);
        g_sock = -1;
    }

    LOG_INFO("[mqtt] 已关闭，累计发布 %lu 条，失败 %lu 次，重连 %d 次",
             g_pub_cnt, g_fail_cnt, g_reconnects);
}
