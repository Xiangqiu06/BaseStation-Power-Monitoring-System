/**
 * @file  app_data.h
 * @brief 数据接口层 —— 温湿度 / 时间数据与界面的桥梁
 *
 * 数据流:
 *   真实硬件(传感器/RTC) ──┐
 *                          ├─> app_data_set_xxx() ─> 界面刷新
 *   演示数据(默认, 弱函数) ─┘
 *
 * 接入真实硬件: 在任意 .c 文件中实现下面两个函数即可覆盖弱函数:
 *   bool app_sensor_read(float *temp_c, float *humi_pct);
 *   bool app_rtc_read(app_datetime_t *dt);
 */
#ifndef APP_DATA_H
#define APP_DATA_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 日期时间结构体 (weekday: 0=周日, 1=周一 ... 6=周六) */
typedef struct {
    int32_t year;
    int32_t month;
    int32_t day;
    int32_t hour;
    int32_t minute;
    int32_t second;
    int32_t weekday;
} app_datetime_t;

/* 初始化: 创建 1 秒周期的刷新定时器 (在 ui_init() 之后调用) */
void app_data_init(void);

/* -------- 上层接口: 传感器任务 / RTC 任务采集到数据后调用 -------- */
void app_data_set_temperature(float temp_c);              /* 摄氏度 */
void app_data_set_humidity(float humi_pct);               /* %RH    */
void app_data_set_datetime(const app_datetime_t *dt);

/* 查询当前缓存值 */
float app_data_get_temperature(void);
float app_data_get_humidity(void);

/* -------- 预留底层接口 (弱函数, 用户实现后自动接管数据源) --------
 * app_sensor_read: 每秒被调用一次, 读取成功返回 true 并写入两个值
 * app_rtc_read   : 每秒被调用一次, 读取成功返回 true 并填充 dt
 * 默认实现: 返回 false, 走演示数据 + 系统时间(PC 模拟器下即真实时间)
 */
bool app_sensor_read(float *temp_c, float *humi_pct);
bool app_rtc_read(app_datetime_t *dt);

#ifdef __cplusplus
}
#endif

#endif /* APP_DATA_H */
