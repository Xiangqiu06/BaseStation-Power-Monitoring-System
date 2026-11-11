#ifndef __PROTOCOL_H__
#define __PROTOCOL_H__

#include "linkqueue.h"

/*------------------------------------------------------------------
 * 串口帧协议（下位机 STM32  ->  IMX6ULL）
 *
 *   +------+------+-----+-----+---------------+-------+
 *   | 0xAA | 0x55 | LEN | CMD |   PAYLOAD     | CRC8  |
 *   +------+------+-----+-----+---------------+-------+
 *       1     1     1     1        LEN           1
 *
 *   LEN  : 负载长度（不含帧头/长度/命令/校验）
 *   CRC8 : CRC-8，多项式 0x31，初值 0x00，高位先出，
 *          校验范围是 LEN、CMD、PAYLOAD
 *
 * 这样定帧的三个好处：
 *   1. 双字节帧头，随便从流中间截断也能重新同步；
 *   2. 有长度字段，收帧不用等结束符；
 *   3. 有校验，串口在电机干扰下偶尔翻一位能被挡掉。
 *----------------------------------------------------------------*/

#define FRAME_HEAD0         0xAA
#define FRAME_HEAD1         0x55

#define CMD_ENV             0x01    /* 环境/电源数据 */
#define CMD_HEART           0x02    /* 心跳 */
#define CMD_ACK             0x03    /* 下位机应答 */

#define ENV_PAYLOAD_LEN     9       /* 见 protocol.c 里的组包说明 */
#define FRAME_MAX_PAYLOAD   32
#define FRAME_MAX_LEN       (FRAME_MAX_PAYLOAD + 5)

/* 解析结果 */
enum proto_result
{
    PROTO_NONE    =  0,     /* 数据还不够，继续喂字节 */
    PROTO_ENV     =  1,     /* 解析出一帧环境数据，*out 已填好 */
    PROTO_HEART   =  2,     /* 收到心跳，只更新计数，不动 *out */
    PROTO_CRC_ERR = -1,     /* 校验错，已丢弃，统计里 +1 */
    PROTO_LEN_ERR = -2      /* 长度越界，已丢弃并重新同步 */
};

/* 解析器状态机，按字节喂，收到半帧可以一直挂着等下半个字节 */
struct frame_parser
{
    unsigned char buf[FRAME_MAX_LEN];
    int           idx;          /* 已经收进 buf 的字节数 */
    int           need;         /* 收满多少字节算一帧，0 表示还没算出 */
    unsigned long ok_cnt;       /* 累计正确帧 */
    unsigned long crc_err;      /* 累计校验错 */
    unsigned long len_err;      /* 累计长度错 */
};

void protocol_parser_init(struct frame_parser *p);

/* 喂一个字节；返回 enum proto_result */
int  protocol_feed(struct frame_parser *p, unsigned char byte, struct msgdata *out);

/* 按协议组一帧环境数据，返回整帧长度，失败返回 -1。
 * 模拟数据源也走这个函数，保证"假数据"和"真数据"走同一条解析路径 */
int  protocol_build_env(const struct msgdata *m, unsigned char *buf, int cap);

/* 按协议组一帧心跳 */
int  protocol_build_heart(int dev_id, unsigned char *buf, int cap);

unsigned char protocol_crc8(const unsigned char *data, int len);

#endif
