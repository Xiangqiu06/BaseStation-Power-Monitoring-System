#include "ui_app.h"
#include "config.h"
#include "logger.h"
#include "mailbox.h"
#include "alarm.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <stdint.h>
#include <sqlite3.h>

#include "lvgl/lvgl.h"
#include "lv_drivers/display/fbdev.h"
#include "lv_drivers/indev/evdev.h"

/* 中文字体由 tools/gen_font.sh 用 lv_font_conv 从 simhei.ttf 生成，
 * 只包含界面实际用到的字，省 Flash/RAM（整字库要几 MB） */
LV_FONT_DECLARE(ui_font_cn_16);
LV_FONT_DECLARE(ui_font_cn_28);

/*------------------------------------------------------------------
 * 配色：深色底 + 高对比数值，户外机柜的屏幕上亮色更清楚
 *----------------------------------------------------------------*/
#define COL_BG        lv_color_hex(0x0F1724)
#define COL_CARD      lv_color_hex(0x1B2637)
#define COL_CARD_BRD  lv_color_hex(0x2B3B52)
#define COL_TEXT      lv_color_hex(0xF1F5F9)
#define COL_SUB       lv_color_hex(0x94A3B8)
#define COL_VOLT      lv_color_hex(0x38BDF8)
#define COL_CURR      lv_color_hex(0xFF8C42)
#define COL_TEMP      lv_color_hex(0xF87171)
#define COL_HUM       lv_color_hex(0x34D399)
#define COL_OK        lv_color_hex(0x4ADE80)
#define COL_WARN      lv_color_hex(0xFBBF24)
#define COL_CRIT      lv_color_hex(0xF43F5E)

#define SCREEN_W      800
#define SCREEN_H      480
#define HEADER_H      56
#define CHART_POINTS  120           /* 曲线缓存点数，1Hz 下是 2 分钟 */
#define HIST_ROWS     10            /* 历史页显示行数 */
#define HIST_REFRESH_S 5            /* 历史表刷新周期 */

/* 显示缓冲：800x480 的 1/10 屏，足够画整屏也不会太占内存 */
#define DISP_BUF_LINES  48

/*------------------------------------------------------------------
 * 控件句柄
 *----------------------------------------------------------------*/
static lv_obj_t *g_lbl_title   = NULL;
static lv_obj_t *g_lbl_state   = NULL;
static lv_obj_t *g_lbl_time    = NULL;
static lv_obj_t *g_lbl_bat     = NULL;

/* 实时参数页 */
static lv_obj_t *g_val_v       = NULL;
static lv_obj_t *g_val_a       = NULL;
static lv_obj_t *g_val_t       = NULL;
static lv_obj_t *g_val_h       = NULL;
static lv_obj_t *g_val_smoke   = NULL;
static lv_obj_t *g_val_soc     = NULL;
static lv_obj_t *g_val_soh     = NULL;

/* 曲线页 */
static lv_obj_t *g_chart_v     = NULL;
static lv_chart_series_t *g_ser_v = NULL;
static lv_obj_t *g_chart_a     = NULL;
static lv_chart_series_t *g_ser_a = NULL;

/* 历史页 */
static lv_obj_t *g_tbl_hist    = NULL;
static sqlite3  *g_hist_db     = NULL;
static time_t    g_hist_last   = 0;

/* 配置页 */
typedef struct
{
    const char *name;       /* 显示名 */
    double     *target;     /* 指向 g_cfg 里的字段（配置里是 double） */
    double      step;
    double      lo;
    double      hi;
    int         dec;        /* 小数位 */
    lv_obj_t   *value_lbl;
} cfg_row_t;

static cfg_row_t g_rows[6];

static int g_ready = 0;

int ui_app_ready(void)
{
    return g_ready;
}

/*------------------------------------------------------------------
 * LVGL 的系统时基。lv_conf.h 里开了 LV_TICK_CUSTOM，
 * 要求应用层提供这个函数，返回开机以来的毫秒数
 *----------------------------------------------------------------*/
uint32_t custom_tick_get(void)
{
    static uint64_t start_ms = 0;
    struct timeval  tv;

    gettimeofday(&tv, NULL);

    {
        uint64_t now = (uint64_t)tv.tv_sec * 1000u + (uint64_t)tv.tv_usec / 1000u;

        if (0 == start_ms)
        {
            start_ms = now;
        }

        return (uint32_t)(now - start_ms);
    }
}

/*================================================================
 * 样式小工具
 *================================================================*/
static void style_card(lv_obj_t *obj)
{
    lv_obj_set_style_bg_color(obj, COL_CARD, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, COL_CARD_BRD, 0);
    lv_obj_set_style_radius(obj, 10, 0);
    lv_obj_set_style_pad_all(obj, 10, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *txt,
                            const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *lbl = lv_label_create(parent);

    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, color, 0);

    return lbl;
}

/* 数值卡片：标题 + 大数值 + 单位 */
static lv_obj_t *make_value_card(lv_obj_t *parent, const char *title,
                                 const char *unit, int x, int y, int w, int h,
                                 lv_color_t accent, lv_obj_t **val_out)
{
    lv_obj_t *card = lv_obj_create(parent);

    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    style_card(card);

    {
        lv_obj_t *t = make_label(card, title, &ui_font_cn_16, COL_SUB);

        lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);
    }

    {
        lv_obj_t *v = make_label(card, "--", &ui_font_cn_28, accent);

        lv_obj_align(v, LV_ALIGN_TOP_LEFT, 0, 24);
        if (NULL != val_out)
        {
            *val_out = v;
        }
    }

    {
        lv_obj_t *u = make_label(card, unit, &ui_font_cn_16, COL_SUB);

        lv_obj_align(u, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    }

    return card;
}

/*================================================================
 * 1. 实时参数页
 *================================================================*/
static void build_page_realtime(lv_obj_t *tab)
{
    lv_obj_set_style_bg_color(tab, COL_BG, 0);
    lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(tab, 8, 0);
    lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);

    /* 上排 4 个大卡片：电压 / 电流 / 温度 / 湿度 */
    make_value_card(tab, "电压", "V",    0,   0, 188, 132, COL_VOLT, &g_val_v);
    make_value_card(tab, "电流", "A",  196,   0, 188, 132, COL_CURR, &g_val_a);
    make_value_card(tab, "温度", "℃",  392,   0, 188, 132, COL_TEMP, &g_val_t);
    make_value_card(tab, "湿度", "%RH", 588,  0, 188, 132, COL_HUM,  &g_val_h);

    /* 下排 3 个小卡片：烟雾 / 电量 / 健康度 */
    make_value_card(tab, "烟雾", "ppm",  0, 140, 250, 120, COL_WARN, &g_val_smoke);
    make_value_card(tab, "电量", "%",   258, 140, 250, 120, COL_OK,   &g_val_soc);
    make_value_card(tab, "健康", "%",   516, 140, 250, 120, COL_HUM,  &g_val_soh);

    /* 底部一行：电池组编号 + 告警描述 */
    {
        lv_obj_t *bar = lv_obj_create(tab);

        lv_obj_set_pos(bar, 0, 268);
        lv_obj_set_size(bar, 776, 52);
        style_card(bar);

        g_lbl_bat = make_label(bar, "电池组 1", &ui_font_cn_16, COL_TEXT);
        lv_obj_align(g_lbl_bat, LV_ALIGN_LEFT_MID, 0, 0);

        g_lbl_state = make_label(bar, "正常", &ui_font_cn_16, COL_OK);
        lv_obj_align(g_lbl_state, LV_ALIGN_RIGHT_MID, 0, 0);
    }
}

/*================================================================
 * 2. 动态曲线页
 *================================================================*/
static lv_obj_t *make_chart(lv_obj_t *tab, const char *title,
                            int y, int h, int max, lv_color_t color,
                            lv_chart_series_t **ser_out)
{
    lv_obj_t *box = lv_obj_create(tab);

    lv_obj_set_pos(box, 0, y);
    lv_obj_set_size(box, 776, h);
    style_card(box);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    {
        lv_obj_t *t = make_label(box, title, &ui_font_cn_16, COL_SUB);

        lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, -2);
    }

    {
        lv_obj_t *c = lv_chart_create(box);

        lv_obj_set_size(c, 752, h - 26);
        lv_obj_align(c, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_chart_set_type(c, LV_CHART_TYPE_LINE);
        lv_chart_set_point_count(c, CHART_POINTS);
        lv_chart_set_range(c, LV_CHART_AXIS_PRIMARY_Y, 0, (lv_coord_t)max);
        lv_chart_set_div_line_count(c, 4, 6);
        lv_obj_set_style_bg_color(c, COL_CARD, 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(c, 0, 0);
        lv_obj_set_style_size(c, 0, LV_PART_INDICATOR);   /* 不画圆点，只要线 */
        lv_obj_set_style_line_width(c, 2, LV_PART_ITEMS);

        *ser_out = lv_chart_add_series(c, color, LV_CHART_AXIS_PRIMARY_Y);

        return c;
    }
}

static void build_page_chart(lv_obj_t *tab)
{
    lv_obj_set_style_bg_color(tab, COL_BG, 0);
    lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(tab, 8, 0);
    lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);

    g_chart_v = make_chart(tab, "电压曲线 (V) 0~60", 0,   156, 600, COL_VOLT, &g_ser_v);
    g_chart_a = make_chart(tab, "电流曲线 (A) 0~10", 164, 156, 100, COL_CURR, &g_ser_a);

    /* 把两条线的初始值填成 0，避免一开始是一串随机内存值 */
    {
        int i;

        for (i = 0; i < CHART_POINTS; i++)
        {
            lv_chart_set_next_value(g_chart_v, g_ser_v, 0);
            lv_chart_set_next_value(g_chart_a, g_ser_a, 0);
        }
    }
}

/*================================================================
 * 3. 参数配置页
 *================================================================*/
static void cfg_row_refresh(int idx)
{
    char buf[24];

    if (idx < 0 || idx >= 6 || NULL == g_rows[idx].value_lbl || NULL == g_rows[idx].target)
    {
        return;
    }

    if (g_rows[idx].dec > 0)
    {
        snprintf(buf, sizeof(buf), "%.*f", g_rows[idx].dec, (double)*g_rows[idx].target);
    }
    else
    {
        snprintf(buf, sizeof(buf), "%.0f", (double)*g_rows[idx].target);
    }

    lv_label_set_text(g_rows[idx].value_lbl, buf);
}

static void cfg_btn_cb(lv_event_t *e)
{
    /* user_data 的低 8 位存行号，高 8 位存方向（1=加 0=减） */
    uintptr_t packed = (uintptr_t)lv_event_get_user_data(e);
    int       idx    = (int)(packed & 0xFF);
    int       dir    = (int)((packed >> 8) & 0xFF);
    double    v;

    if (idx < 0 || idx >= 6 || NULL == g_rows[idx].target)
    {
        return;
    }

    v = *g_rows[idx].target;
    v += (dir ? g_rows[idx].step : -g_rows[idx].step);

    if (v < g_rows[idx].lo)
    {
        v = g_rows[idx].lo;
    }
    if (v > g_rows[idx].hi)
    {
        v = g_rows[idx].hi;
    }

    *g_rows[idx].target = v;
    cfg_row_refresh(idx);

    LOG_INFO("[ui] 阈值改为 %s = %.*f（未保存前只对本进程生效）",
             g_rows[idx].name, g_rows[idx].dec, (double)v);
}

static void btn_save_cb(lv_event_t *e)
{
    lv_obj_t *lbl = (lv_obj_t *)lv_event_get_user_data(e);

    (void)e;

    if (0 == config_save())
    {
        lv_label_set_text(lbl, "已保存");
        lv_obj_set_style_text_color(lbl, COL_OK, 0);
    }
    else
    {
        lv_label_set_text(lbl, "保存失败");
        lv_obj_set_style_text_color(lbl, COL_CRIT, 0);
    }
}

static void btn_default_cb(lv_event_t *e)
{
    int i;

    (void)e;

    /* 只重置阈值，不动串口/数据库这些连接参数 —— 那些改了要重启 */
    g_cfg.temp_max  = 60.0;
    g_cfg.hum_max   = 95.0;
    g_cfg.volt_min  = 46.0;
    g_cfg.volt_max  = 50.5;
    g_cfg.curr_max  = 5.0;
    g_cfg.smoke_max = 300.0;

    for (i = 0; i < 6; i++)
    {
        cfg_row_refresh(i);
    }

    LOG_INFO("[ui] 阈值已恢复默认");
}

static lv_obj_t *make_btn(lv_obj_t *parent, const char *txt, int x, int y,
                          int w, int h, lv_event_cb_t cb, void *user)
{
    lv_obj_t *btn = lv_btn_create(parent);

    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x25344A), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);

    {
        lv_obj_t *l = make_label(btn, txt, &ui_font_cn_16, COL_TEXT);

        lv_obj_center(l);
    }

    if (NULL != cb)
    {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user);
    }

    return btn;
}

static void build_page_config(lv_obj_t *tab)
{
    int i;

    lv_obj_set_style_bg_color(tab, COL_BG, 0);
    lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(tab, 8, 0);
    lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);

    g_rows[0].name = "电压下限"; g_rows[0].target = &g_cfg.volt_min;
    g_rows[0].step = 0.5;  g_rows[0].lo = 30.0;  g_rows[0].hi = 55.0; g_rows[0].dec = 1;

    g_rows[1].name = "电压上限"; g_rows[1].target = &g_cfg.volt_max;
    g_rows[1].step = 0.5;  g_rows[1].lo = 40.0;  g_rows[1].hi = 60.0; g_rows[1].dec = 1;

    g_rows[2].name = "温度上限"; g_rows[2].target = &g_cfg.temp_max;
    g_rows[2].step = 1.0;  g_rows[2].lo = 30.0;  g_rows[2].hi = 90.0; g_rows[2].dec = 0;

    g_rows[3].name = "湿度上限"; g_rows[3].target = &g_cfg.hum_max;
    g_rows[3].step = 1.0;  g_rows[3].lo = 40.0;  g_rows[3].hi = 100.0; g_rows[3].dec = 0;

    g_rows[4].name = "电流上限"; g_rows[4].target = &g_cfg.curr_max;
    g_rows[4].step = 0.5;  g_rows[4].lo = 1.0;   g_rows[4].hi = 20.0; g_rows[4].dec = 1;

    g_rows[5].name = "烟雾上限"; g_rows[5].target = &g_cfg.smoke_max;
    g_rows[5].step = 10.0; g_rows[5].lo = 50.0;  g_rows[5].hi = 800.0; g_rows[5].dec = 0;

    for (i = 0; i < 6; i++)
    {
        int y = 4 + i * 54;

        lv_obj_t *row = lv_obj_create(tab);

        lv_obj_set_pos(row, 0, y);
        lv_obj_set_size(row, 776, 46);
        style_card(row);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        {
            lv_obj_t *n = make_label(row, g_rows[i].name, &ui_font_cn_16, COL_TEXT);

            lv_obj_align(n, LV_ALIGN_LEFT_MID, 0, 0);
        }

        make_btn(row, "-", 260, 4, 56, 34, cfg_btn_cb,
                 (void *)(uintptr_t)(i | (0 << 8)));

        g_rows[i].value_lbl = make_label(row, "--", &ui_font_cn_28, COL_VOLT);
        lv_obj_set_width(g_rows[i].value_lbl, 120);
        lv_obj_set_style_text_align(g_rows[i].value_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(g_rows[i].value_lbl, LV_ALIGN_LEFT_MID, 330, -4);

        make_btn(row, "+", 470, 4, 56, 34, cfg_btn_cb,
                 (void *)(uintptr_t)(i | (1 << 8)));

        cfg_row_refresh(i);
    }

    /* 底部：保存 / 恢复默认 */
    {
        lv_obj_t *lbl_hint = make_label(tab, "改了阈值点保存才会写回配置文件",
                                       &ui_font_cn_16, COL_SUB);

        lv_obj_align(lbl_hint, LV_ALIGN_BOTTOM_LEFT, 4, -8);
    }

    {
        static lv_obj_t *saved_lbl = NULL;

        saved_lbl = make_label(tab, "", &ui_font_cn_16, COL_OK);
        lv_obj_align(saved_lbl, LV_ALIGN_BOTTOM_LEFT, 420, -8);

        make_btn(tab, "恢复默认", 470, 330, 130, 42, btn_default_cb, NULL);
        make_btn(tab, "保存",     620, 330, 150, 42, btn_save_cb, saved_lbl);
    }
}

/*================================================================
 * 4. 历史数据页
 *================================================================*/
static void build_page_history(lv_obj_t *tab)
{
    lv_obj_set_style_bg_color(tab, COL_BG, 0);
    lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(tab, 8, 0);
    lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);

    g_tbl_hist = lv_table_create(tab);

    lv_obj_set_pos(g_tbl_hist, 0, 0);
    lv_obj_set_size(g_tbl_hist, 776, 300);
    lv_table_set_col_cnt(g_tbl_hist, 6);
    lv_table_set_row_cnt(g_tbl_hist, HIST_ROWS + 1);

    lv_table_set_col_width(g_tbl_hist, 0, 210);
    lv_table_set_col_width(g_tbl_hist, 1, 110);
    lv_table_set_col_width(g_tbl_hist, 2, 110);
    lv_table_set_col_width(g_tbl_hist, 3, 110);
    lv_table_set_col_width(g_tbl_hist, 4, 110);
    lv_table_set_col_width(g_tbl_hist, 5, 120);

    lv_obj_set_style_text_font(g_tbl_hist, &ui_font_cn_16, LV_PART_ITEMS);
    lv_obj_set_style_text_color(g_tbl_hist, COL_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(g_tbl_hist, COL_CARD, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(g_tbl_hist, COL_CARD_BRD, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(g_tbl_hist, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_border_color(g_tbl_hist, COL_CARD_BRD, LV_PART_ITEMS);
    lv_obj_set_style_border_width(g_tbl_hist, 1, LV_PART_ITEMS);

    lv_table_set_cell_value(g_tbl_hist, 0, 0, "时间");
    lv_table_set_cell_value(g_tbl_hist, 0, 1, "电压V");
    lv_table_set_cell_value(g_tbl_hist, 0, 2, "电流A");
    lv_table_set_cell_value(g_tbl_hist, 0, 3, "温度℃");
    lv_table_set_cell_value(g_tbl_hist, 0, 4, "电量%");
    lv_table_set_cell_value(g_tbl_hist, 0, 5, "状态");

    {
        int r;

        for (r = 1; r <= HIST_ROWS; r++)
        {
            int c;

            for (c = 0; c < 6; c++)
            {
                lv_table_set_cell_value(g_tbl_hist, (uint16_t)r, (uint16_t)c, "");
            }
        }
    }

    {
        lv_obj_t *lbl = make_label(tab, "数据来源 SQLite，每 5 秒刷新", &ui_font_cn_16, COL_SUB);

        lv_obj_align(lbl, LV_ALIGN_BOTTOM_LEFT, 4, -6);
    }
}

/*================================================================
 * 历史表刷新：UI 线程自己开一个只读连接
 *
 * 为什么不直接用 storage_*：那是 save_data 线程的连接，
 * 一个 sqlite3 连接不能跨线程同时用。WAL 模式下读写可以并发，
 * 所以给界面单独开一个只读连接，互不干扰。
 *================================================================*/
static void hist_refresh(void)
{
    sqlite3_stmt *stmt = NULL;
    int           r    = 1;

    if (NULL == g_tbl_hist)
    {
        return;
    }

    if (NULL == g_hist_db)
    {
        if (SQLITE_OK != sqlite3_open_v2(g_cfg.db_path, &g_hist_db,
                                         SQLITE_OPEN_READONLY, NULL))
        {
            if (NULL != g_hist_db)
            {
                sqlite3_close(g_hist_db);
                g_hist_db = NULL;
            }
            return;             /* 库还没建出来，下一轮再试 */
        }
    }

    if (SQLITE_OK != sqlite3_prepare_v2(g_hist_db,
            "SELECT ts,temp,volt,curr FROM sensor_data "
            "ORDER BY rec_id DESC LIMIT ?;", -1, &stmt, NULL))
    {
        return;
    }

    sqlite3_bind_int(stmt, 1, HIST_ROWS);

    while (SQLITE_ROW == sqlite3_step(stmt) && r <= HIST_ROWS)
    {
        char ts[32];
        char buf[32];
        double v;
        const unsigned char *p_ts = sqlite3_column_text(stmt, 0);

        snprintf(ts, sizeof(ts), "%s", (NULL == p_ts) ? "" : (const char *)p_ts);

        /* 时间只留 时分秒，日期对运行日志没意义，省列宽 */
        if (strlen(ts) >= 19)
        {
            lv_table_set_cell_value(g_tbl_hist, (uint16_t)r, 0, ts + 11);
        }
        else
        {
            lv_table_set_cell_value(g_tbl_hist, (uint16_t)r, 0, ts);
        }

        v = sqlite3_column_double(stmt, 2);
        snprintf(buf, sizeof(buf), "%.2f", v);
        lv_table_set_cell_value(g_tbl_hist, (uint16_t)r, 1, buf);

        v = sqlite3_column_double(stmt, 3);
        snprintf(buf, sizeof(buf), "%.2f", v);
        lv_table_set_cell_value(g_tbl_hist, (uint16_t)r, 2, buf);

        v = sqlite3_column_double(stmt, 1);
        snprintf(buf, sizeof(buf), "%.1f", v);
        lv_table_set_cell_value(g_tbl_hist, (uint16_t)r, 3, buf);

        lv_table_set_cell_value(g_tbl_hist, (uint16_t)r, 4, "--");
        lv_table_set_cell_value(g_tbl_hist, (uint16_t)r, 5, "已入库");

        r++;
    }

    sqlite3_finalize(stmt);

    /* 没读满就把剩下的行清空，避免显示上一次的残留 */
    while (r <= HIST_ROWS)
    {
        int c;

        for (c = 0; c < 6; c++)
        {
            lv_table_set_cell_value(g_tbl_hist, (uint16_t)r, (uint16_t)c, "");
        }
        r++;
    }
}

/*================================================================
 * 顶栏
 *================================================================*/
static void build_header(lv_obj_t *scr)
{
    lv_obj_t *bar = lv_obj_create(scr);

    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, SCREEN_W, HEADER_H);
    lv_obj_set_style_bg_color(bar, COL_CARD, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    g_lbl_title = make_label(bar, "后备电源检测系统", &ui_font_cn_16, COL_TEXT);
    lv_obj_align(g_lbl_title, LV_ALIGN_LEFT_MID, 16, 0);

    g_lbl_time = make_label(bar, "--:--:--", &ui_font_cn_16, COL_SUB);
    lv_obj_align(g_lbl_time, LV_ALIGN_RIGHT_MID, -16, 0);
}

/*================================================================
 * 数据刷新
 *================================================================*/
static void ui_update_data(const struct msgdata *m)
{
    char buf[64];
    lv_color_t st_col;

    if (NULL == m)
    {
        return;
    }

    if (NULL != g_val_v)
    {
        snprintf(buf, sizeof(buf), "%.2f", (double)m->v);
        lv_label_set_text(g_val_v, buf);
    }

    if (NULL != g_val_a)
    {
        snprintf(buf, sizeof(buf), "%.2f", (double)m->a);
        lv_label_set_text(g_val_a, buf);
    }

    if (NULL != g_val_t)
    {
        snprintf(buf, sizeof(buf), "%.1f", (double)m->temp);
        lv_label_set_text(g_val_t, buf);
    }

    if (NULL != g_val_h)
    {
        /* 电池组节点没装湿度传感器时 hum 是 0，显示"--"更诚实 */
        if (m->hum > 0.1f)
        {
            snprintf(buf, sizeof(buf), "%.1f", (double)m->hum);
            lv_label_set_text(g_val_h, buf);
        }
        else
        {
            lv_label_set_text(g_val_h, "--");
        }
    }

    if (NULL != g_val_smoke)
    {
        snprintf(buf, sizeof(buf), "%.0f", (double)m->smoke);
        lv_label_set_text(g_val_smoke, buf);
    }

    if (NULL != g_val_soc)
    {
        snprintf(buf, sizeof(buf), "%.1f", (double)m->soc);
        lv_label_set_text(g_val_soc, buf);
    }

    if (NULL != g_val_soh)
    {
        snprintf(buf, sizeof(buf), "%.1f", (double)m->soh);
        lv_label_set_text(g_val_soh, buf);
    }

    if (NULL != g_lbl_bat)
    {
        snprintf(buf, sizeof(buf), "电池组 %d", m->bat_id);
        lv_label_set_text(g_lbl_bat, buf);
    }

    /* 状态：正常绿 / 警告黄 / 严重红 */
    if (ALARM_LEVEL_CRIT == m->level)
    {
        st_col = COL_CRIT;
    }
    else if (ALARM_LEVEL_WARN == m->level)
    {
        st_col = COL_WARN;
    }
    else
    {
        st_col = COL_OK;
    }

    if (NULL != g_lbl_state)
    {
        char text[128];

        (void)alarm_text(m->alarm, text, (int)sizeof(text));
        lv_label_set_text(g_lbl_state, text);
        lv_obj_set_style_text_color(g_lbl_state, st_col, 0);
    }

    /* 曲线：1Hz 下 120 点正好两分钟 */
    if (NULL != g_chart_v && NULL != g_ser_v)
    {
        /* 点值和量程都放大 10 倍，这样 0.1V 的变化在图上能看出来 */
        lv_chart_set_next_value(g_chart_v, g_ser_v, (lv_coord_t)(m->v * 10.0f));
    }

    if (NULL != g_chart_a && NULL != g_ser_a)
    {
        lv_chart_set_next_value(g_chart_a, g_ser_a, (lv_coord_t)(m->a * 10.0f));
    }
}

static void ui_update_clock(void)
{
    char buf[16];
    time_t now = time(NULL);
    struct tm *tmv = localtime(&now);

    if (NULL == tmv || NULL == g_lbl_time)
    {
        return;
    }

    strftime(buf, sizeof(buf), "%H:%M:%S", tmv);
    lv_label_set_text(g_lbl_time, buf);
}

/*================================================================
 * 界面搭建
 *================================================================*/
static int ui_build(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_t *tv;

    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    build_header(scr);

    tv = lv_tabview_create(scr, LV_DIR_TOP, 40);
    lv_obj_set_pos(tv, 0, HEADER_H);
    lv_obj_set_size(tv, SCREEN_W, SCREEN_H - HEADER_H);
    lv_obj_set_style_bg_color(tv, COL_BG, 0);
    lv_obj_set_style_bg_opa(tv, LV_OPA_COVER, 0);

    build_page_realtime(lv_tabview_add_tab(tv, "实时参数"));
    build_page_chart(lv_tabview_add_tab(tv, "动态曲线"));
    build_page_config(lv_tabview_add_tab(tv, "参数配置"));
    build_page_history(lv_tabview_add_tab(tv, "历史数据"));

    lv_tabview_set_act(tv, 0, LV_ANIM_OFF);

    return 0;
}

/*================================================================
 * 线程主体
 *================================================================*/
void *ui_app_thread(void *arg)
{
    struct msgdata m;
    char           sendname[32] = {0};
    uint32_t       last_clock  = 0;

    (void)arg;

    if (0 == g_cfg.ui_enable)
    {
        /* 界面关掉也要把队列排空，不然生产者那边会一直丢数据 */
        while (g_running)
        {
            (void)recv_mail_msg_timeout(&m, sendname, 200);
        }
        LOG_INFO("[ui] 界面未开启，线程仅做队列排空");
        return NULL;
    }

    /* 没有 framebuffer 就别往下走了：fbdev_init 失败之后 flush 会写空指针 */
    if (0 != access("/dev/fb0", R_OK | W_OK))
    {
        LOG_WARN("[ui] 没有 /dev/fb0，界面以 headless 方式运行");
        while (g_running)
        {
            (void)recv_mail_msg_timeout(&m, sendname, 200);
        }
        return NULL;
    }

    lv_init();
    fbdev_init();

    {
        static lv_color_t        buf1[SCREEN_W * DISP_BUF_LINES];
        static lv_disp_draw_buf_t draw_buf;
        static lv_disp_drv_t      disp_drv;

        lv_disp_draw_buf_init(&draw_buf, buf1, NULL, SCREEN_W * DISP_BUF_LINES);

        lv_disp_drv_init(&disp_drv);
        disp_drv.draw_buf = &draw_buf;
        disp_drv.flush_cb = fbdev_flush;
        disp_drv.hor_res  = SCREEN_W;
        disp_drv.ver_res  = SCREEN_H;
        lv_disp_drv_register(&disp_drv);
    }

    evdev_init();

    {
        static lv_indev_drv_t indev_drv;

        lv_indev_drv_init(&indev_drv);
        indev_drv.type    = LV_INDEV_TYPE_POINTER;
        indev_drv.read_cb = evdev_read;
        lv_indev_drv_register(&indev_drv);
    }

    ui_build();
    g_ready = 1;

    LOG_INFO("[ui] %dx%d 界面已加载，4 个页面：实时参数 / 动态曲线 / 参数配置 / 历史数据",
             SCREEN_W, SCREEN_H);

    while (g_running)
    {
        /* 把队列里积压的数据全部取出来刷到界面上，
         * 取不到就立刻回来继续刷屏，绝不让界面卡在等数据上 */
        while (0 == recv_mail_msg_timeout(&m, sendname, 0))
        {
            ui_update_data(&m);
        }

        lv_timer_handler();

        /* 秒级任务：显示时间、刷新历史表 */
        {
            uint32_t now = custom_tick_get();

            if (now - last_clock >= 1000u)
            {
                last_clock = now;
                ui_update_clock();

                if (time(NULL) - g_hist_last >= HIST_REFRESH_S)
                {
                    g_hist_last = time(NULL);
                    hist_refresh();
                }
            }
        }

        usleep(5000);       /* 5ms 一轮，200Hz 的刷新上限，够顺滑也不占 CPU */
    }

    if (NULL != g_hist_db)
    {
        sqlite3_close(g_hist_db);
        g_hist_db = NULL;
    }

    g_ready = 0;
    LOG_INFO("[ui] 界面线程退出");

    return NULL;
}
