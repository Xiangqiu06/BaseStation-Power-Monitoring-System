#include "protocol.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/*------------------------------------------------------------------
 * 负载编码（ENV 帧，9 字节，小端）
 *
 *   偏移  长度  字段     单位     说明
 *   0     2     temp    0.1 ℃   有符号，冬天室外可能是负的
 *   2     2     hum     0.1 %   无符号
 *   4     2     volt    0.01 V  后备电源电压，关键量
 *   6     2     curr    0.01 A
 *   8     1     smoke   ppm     量程 0~255 够用，超了按 255 截断
 *----------------------------------------------------------------*/

/* 注意：这里固定 9 字节和一字节 smoke，所以 ENV_PAYLOAD_LEN 是 9。
 * 若以后烟雾要上 500ppm，改成 2 字节并把 ENV_PAYLOAD_LEN 改 10，
 * 帧头帧尾都不用动，下位机同步改即可 */
#define ENV_IDX_TEMP    0
#define ENV_IDX_HUM     2
#define ENV_IDX_VOLT    4
#define ENV_IDX_CURR    6
#define ENV_IDX_SMOKE   8

static void put_u16_le(unsigned char *p, unsigned short v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}

static unsigned short get_u16_le(const unsigned char *p)
{
    return (unsigned short)((unsigned short)p[0] | ((unsigned short)p[1] << 8));
}

/****************************************************************
 * 浮点 -> 整数：四舍五入后钳位
 *
 * 不能直接 (short)(v * 10)，因为 float 存不下 25.3，
 * 编译出来是 25.2999992，乘 10 得 252.999992，截断就变成 25.2。
 * 这种"差 0.1"的错在现场很难查，必须四舍五入
 ****************************************************************/
static long clamp_round(double v, long lo, long hi)
{
    long r;

    if (v >= 0.0)
    {
        r = (long)(v + 0.5);
    }
    else
    {
        r = (long)(v - 0.5);
    }

    if (r < lo)
    {
        r = lo;
    }
    if (r > hi)
    {
        r = hi;
    }

    return r;
}

unsigned char protocol_crc8(const unsigned char *data, int len)
{
    unsigned char crc = 0x00;
    int i;
    int b;

    if (NULL == data || len <= 0)
    {
        return 0x00;
    }

    for (i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (b = 0; b < 8; b++)
        {
            if (0 != (crc & 0x80))
            {
                crc = (unsigned char)((unsigned char)(crc << 1) ^ 0x31);
            }
            else
            {
                crc = (unsigned char)(crc << 1);
            }
        }
    }

    return crc;
}

void protocol_parser_init(struct frame_parser *p)
{
    if (NULL == p)
    {
        return;
    }

    memset(p, 0, sizeof(*p));
}

/****************************************************************
 * 组一帧环境数据
 * 返回值：整帧字节数；失败 -1
 ****************************************************************/
int protocol_build_env(const struct msgdata *m, unsigned char *buf, int cap)
{
    int           i = 0;
    short         t;
    unsigned short h;
    unsigned short v;
    unsigned short c;
    unsigned short s;

    if (NULL == m || NULL == buf || cap < (ENV_PAYLOAD_LEN + 5))
    {
        return -1;
    }

    /* 物理量 -> 整数，先四舍五入再钳位，避免截断误差和溢出 */
    t = (short)clamp_round((double)m->temp * 10.0, -32768, 32767);
    h = (unsigned short)clamp_round((double)m->hum * 10.0, 0, 65535);
    v = (unsigned short)clamp_round((double)m->v * 100.0, 0, 65535);
    c = (unsigned short)clamp_round((double)m->a * 100.0, 0, 65535);
    s = (unsigned short)clamp_round((double)m->smoke, 0, 255);

    buf[i++] = FRAME_HEAD0;
    buf[i++] = FRAME_HEAD1;
    buf[i++] = (unsigned char)ENV_PAYLOAD_LEN;
    buf[i++] = CMD_ENV;

    put_u16_le(&buf[i], (unsigned short)t);
    i += 2;
    put_u16_le(&buf[i], h);
    i += 2;
    put_u16_le(&buf[i], v);
    i += 2;
    put_u16_le(&buf[i], c);
    i += 2;
    buf[i++] = (unsigned char)(s > 255 ? 255 : s);

    /* CRC 覆盖从长度字段到负载结束 */
    buf[i] = protocol_crc8(&buf[2], ENV_PAYLOAD_LEN + 2);
    i++;

    return i;
}

/****************************************************************
 * 组一帧心跳，负载一个字节：设备号低 8 位
 ****************************************************************/
int protocol_build_heart(int dev_id, unsigned char *buf, int cap)
{
    int i = 0;

    if (NULL == buf || cap < 6)
    {
        return -1;
    }

    buf[i++] = FRAME_HEAD0;
    buf[i++] = FRAME_HEAD1;
    buf[i++] = 1;
    buf[i++] = CMD_HEART;
    buf[i++] = (unsigned char)(dev_id & 0xFF);
    buf[i]   = protocol_crc8(&buf[2], 3);
    i++;

    return i;
}

/****************************************************************
 * 把一帧解析成 msgdata
 * 只在 CRC 正确、长度正确的前提下才填 out，半帧不会污染数据
 ****************************************************************/
static int protocol_parse_frame(const unsigned char *f, struct msgdata *out)
{
    unsigned char crc;
    unsigned char cmd;
    int           len;
    short         t;
    time_t        now = 0;
    struct tm     tmv;

    len = f[2];
    cmd = f[3];

    crc = protocol_crc8(&f[2], len + 2);
    if (crc != f[len + 4])
    {
        return PROTO_CRC_ERR;
    }

    if (CMD_HEART == cmd)
    {
        return PROTO_HEART;
    }

    if (CMD_ENV != cmd)
    {
        return PROTO_NONE;              /* 别的命令先不处理 */
    }

    if (len != ENV_PAYLOAD_LEN)
    {
        return PROTO_LEN_ERR;
    }

    if (NULL == out)
    {
        return PROTO_NONE;
    }

    t = (short)get_u16_le(&f[4 + ENV_IDX_TEMP]);

    memset(out, 0, sizeof(*out));
    out->temp  = (float)t / 10.0f;
    out->hum   = (float)get_u16_le(&f[4 + ENV_IDX_HUM]) / 10.0f;
    out->v     = (float)get_u16_le(&f[4 + ENV_IDX_VOLT]) / 100.0f;
    out->a     = (float)get_u16_le(&f[4 + ENV_IDX_CURR]) / 100.0f;
    out->smoke = (float)f[4 + ENV_IDX_SMOKE];

    /* 时间戳以采集板为准；真机上下位机最好也带时间，这里先用本地时间 */
    now = time(NULL);
    memset(&tmv, 0, sizeof(tmv));
#if defined(_WIN32)
    {
        struct tm *ptm = localtime(&now);
        if (NULL != ptm)
        {
            tmv = *ptm;
        }
    }
#else
    (void)localtime_r(&now, &tmv);
#endif
    out->tim = tmv;

    return PROTO_ENV;
}

/****************************************************************
 * 字节流喂进来，凑够一帧就返回结果
 *
 * 状态机只靠 idx 和 need 两个量：
 *   idx  == 0           等帧头 0xAA
 *   idx  == 1           等帧头 0x55（再来个 0xAA 就原地重来）
 *   idx  == 2 收到 LEN  算出 need
 *   idx  <  need        继续收
 *   idx  == need        校验 + 解析，然后复位
 *
 * 好处是断在半个帧里也不怕：下个字节到了接着收，丢一个字节最多坏一帧
 ****************************************************************/
int protocol_feed(struct frame_parser *p, unsigned char byte, struct msgdata *out)
{
    int ret = PROTO_NONE;
    int len = 0;

    if (NULL == p)
    {
        return PROTO_NONE;
    }

    /* 找帧头 */
    if (0 == p->idx)
    {
        if (FRAME_HEAD0 == byte)
        {
            p->buf[0] = byte;
            p->idx    = 1;
        }
        return PROTO_NONE;
    }

    if (1 == p->idx)
    {
        if (FRAME_HEAD1 == byte)
        {
            p->buf[1] = byte;
            p->idx    = 2;
        }
        else if (FRAME_HEAD0 == byte)
        {
            p->buf[0] = byte;           /* 0xAA 0xAA 0x55，原地等 0x55 */
        }
        else
        {
            p->idx = 0;
        }
        return PROTO_NONE;
    }

    if (2 == p->idx)
    {
        len = (int)byte;

        if (len <= 0 || len > FRAME_MAX_PAYLOAD)
        {
            p->len_err++;
            p->idx = 0;
            /* 这个字节本身有可能就是新帧的帧头 */
            if (FRAME_HEAD0 == byte)
            {
                p->buf[0] = byte;
                p->idx    = 1;
            }
            return PROTO_LEN_ERR;
        }

        p->buf[2] = byte;
        p->idx    = 3;
        p->need   = len + 5;            /* 头2 + 长度1 + 命令1 + 负载len + CRC1 */
        return PROTO_NONE;
    }

    /* 长度已经算好了，正常收字节 */
    if (p->idx >= (int)sizeof(p->buf))
    {
        p->idx  = 0;
        p->need = 0;
        return PROTO_LEN_ERR;
    }

    p->buf[p->idx++] = byte;

    if (p->idx < p->need)
    {
        return PROTO_NONE;
    }

    /* 凑满一帧 */
    ret = protocol_parse_frame(p->buf, out);

    if (PROTO_CRC_ERR == ret)
    {
        p->crc_err++;
    }
    else if (PROTO_ENV == ret)
    {
        p->ok_cnt++;
    }

    p->idx  = 0;
    p->need = 0;

    return ret;
}
