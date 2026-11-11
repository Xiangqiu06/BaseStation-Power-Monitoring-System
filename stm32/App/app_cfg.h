#ifndef __APP_CFG_H__
#define __APP_CFG_H__

#include <stdint.h>

/*------------------------------------------------------------------
 * 电池组节点 / 网关 的公共配置
 *
 * 一个工程两个角色，用 ROLE_NODE / ROLE_GATEWAY 区分：
 *   节点  ：采集 + SOC + 告警，通过 LoRa 把数据发给网关
 *   网关  ：收 LoRa，维护寄存器映像，对 i.MX6ULL 做 Modbus 从机
 * 这样两个设备共用一套驱动和协议定义，改一处两边都生效。
 *----------------------------------------------------------------*/

#define ROLE_NODE       1
#define ROLE_GATEWAY    2

#ifndef APP_ROLE
#define APP_ROLE        ROLE_NODE
#endif

/* ---------------- 节点身份 ---------------- */
#ifndef NODE_ID
#define NODE_ID         1
#endif

/* ---------------- 采集参数 ---------------- */
#define SAMPLE_PERIOD_MS        200     /* 电流电压 5Hz 采，够快也不费 CPU */
#define TEMP_PERIOD_MS          2000    /* DS18B20 转换要 750ms，2s 一次 */
#define REPORT_PERIOD_MS        1000    /* 上报周期 */
#define MODBUS_POLL_MS          1       /* Modbus 从机轮询间隔，靠它判帧间隙 */

/* ---------------- 电池组参数（48V 锂电组） ---------------- */
#define BAT_CELL_SERIES         16      /* 16 串磷酸铁锂 */
#define BAT_CAPACITY_AH         50.0f   /* 额定容量，安时积分要用 */

/* ---------------- 告警阈值 ---------------- */
#define TH_VOLT_LOW             46.0f
#define TH_VOLT_HIGH            50.5f
#define TH_CURR_HIGH            5.0f
#define TH_TEMP_HIGH            60.0f
#define TH_TEMP_LOW             (-10.0f)

/* 去抖：连续多少次越限才真正报，防止采样毛刺来回跳 */
#define ALARM_DEBOUNCE_CNT      3

/* ---------------- 告警位（与 i.MX6ULL 侧 alarm.h 保持一致） ---------------- */
#define AL_BIT_TEMP_HIGH        0x01u
#define AL_BIT_HUM_HIGH         0x02u
#define AL_BIT_VOLT_LOW         0x04u
#define AL_BIT_VOLT_HIGH        0x08u
#define AL_BIT_CURR_HIGH        0x10u
#define AL_BIT_SMOKE_HIGH       0x20u
#define AL_BIT_TEMP_LOW         0x40u

/* ---------------- LoRa 上报帧 ---------------- */
#define LORA_FRAME_LEN          15      /* 见 lora_task.c 的组帧说明 */

/* 一次采样结果，节点内部流转都用它 */
typedef struct
{
    uint8_t  node_id;
    uint16_t volt_100;      /* 电压 *100，整数传，省得传浮点 */
    int16_t  curr_100;      /* 电流 *100，正=充电 负=放电 */
    int16_t  temp_10;       /* 温度 *10 */
    uint16_t soc_10;        /* SOC  *10 */
    uint16_t soh_10;        /* SOH  *10 */
    uint16_t alarm;         /* 告警位 */
} bat_sample_t;

#endif
