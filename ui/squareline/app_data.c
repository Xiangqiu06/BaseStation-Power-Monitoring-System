/**
 * @file  app_data.c
 * @brief 数据接口层实现 (含演示数据 + 1 秒刷新定时器)
 *
 * main() 中的调用顺序:
 *   lv_init(); ...显示/输入驱动初始化...;
 *   ui_init();        // SquareLine 生成的界面初始化
 *   app_data_init();  // 启动数据刷新 (本文件)
 */
#include "app_data.h"
#include "ui.h"
#include "lvgl/lvgl.h"
#include <time.h>

/* 弱函数宏: MSVC 无 __attribute__((weak)), 其余编译器(GCC/Clang/armcc)均支持 */
#if defined(_MSC_VER) && !defined(__clang__)
  #define WEAK
#else
  #define WEAK __attribute__((weak))
#endif

/* ---------------- 数据缓存 ---------------- */
static float s_temp_c   = 25.6f;   /* 初始演示值 */
static float s_humi_pct = 58.0f;

/* 今日最高/最低统计 */
static float s_t_min, s_t_max, s_h_min, s_h_max;
static bool  s_stats_valid = false;

/* ================= 预留底层接口的默认实现 (演示) =================
 * 在其他文件中重新实现同名函数即可覆盖(弱链接), 无需修改本文件。 */

WEAK bool app_sensor_read(float *temp_c, float *humi_pct)
{
    (void)temp_c;
    (void)humi_pct;
    return false;   /* 默认无真实传感器 -> 走演示数据 */
}

WEAK bool app_rtc_read(app_datetime_t *dt)
{
    (void)dt;
    return false;   /* 默认无真实 RTC -> 使用系统时间(模拟器下为真实时间) */
}

/* ================= 内部工具 ================= */

static void stats_update(float t, float h)
{
    if (!s_stats_valid) {
        s_t_min = s_t_max = t;
        s_h_min = s_h_max = h;
        s_stats_valid = true;
    } else {
        if (t < s_t_min) s_t_min = t;
        if (t > s_t_max) s_t_max = t;
        if (h < s_h_min) s_h_min = h;
        if (h > s_h_max) s_h_max = h;
    }
    ui_update_stats(s_t_min, s_t_max, s_h_min, s_h_max);
}

/* ================= 上层接口 ================= */

void app_data_set_temperature(float temp_c)
{
    s_temp_c = temp_c;
    ui_update_temperature(temp_c);
    stats_update(temp_c, s_humi_pct);
}

void app_data_set_humidity(float humi_pct)
{
    s_humi_pct = humi_pct;
    ui_update_humidity(humi_pct);
    stats_update(s_temp_c, humi_pct);
}

void app_data_set_datetime(const app_datetime_t *dt)
{
    if (dt == NULL) return;
    ui_update_time(dt->hour, dt->minute, dt->second);
    ui_update_date(dt->year, dt->month, dt->day, dt->weekday);
}

float app_data_get_temperature(void) { return s_temp_c;   }
float app_data_get_humidity(void)    { return s_humi_pct; }

/* ================= 内部: 1 秒刷新定时器 ================= */

static void app_data_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    /* 1. 温湿度: 优先调用用户的传感器接口 */
    float t, h;
    if (app_sensor_read(&t, &h)) {
        app_data_set_temperature(t);
        app_data_set_humidity(h);
    } else {
        /* 演示模式: 数值轻微往复波动, 便于观察界面效果 */
        s_temp_c   += (s_temp_c   > 27.0f) ? -0.2f : 0.2f;
        s_humi_pct += (s_humi_pct > 63.0f) ? -0.4f : 0.4f;
        ui_update_temperature(s_temp_c);
        ui_update_humidity(s_humi_pct);
        stats_update(s_temp_c, s_humi_pct);
    }

    /* 2. 时间: 优先调用用户的 RTC 接口 */
    app_datetime_t dt;
    if (app_rtc_read(&dt)) {
        app_data_set_datetime(&dt);
    } else {
        /* 演示模式: 使用系统时间, PC 模拟器上即为真实时间 */
        time_t now = time(NULL);
        struct tm *lt = localtime(&now);
        if (lt != NULL) {
            dt.year    = lt->tm_year + 1900;
            dt.month   = lt->tm_mon + 1;
            dt.day     = lt->tm_mday;
            dt.hour    = lt->tm_hour;
            dt.minute  = lt->tm_min;
            dt.second  = lt->tm_sec;
            dt.weekday = lt->tm_wday;   /* 0=周日, 与 app_datetime_t 一致 */
            app_data_set_datetime(&dt);
        }
    }
}

void app_data_init(void)
{
    lv_timer_create(app_data_timer_cb, 1000, NULL);  /* 1 秒周期 */
    app_data_timer_cb(NULL);                         /* 立即刷新一次 */
}
