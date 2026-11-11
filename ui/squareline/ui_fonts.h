/**
 * @file  ui_fonts.h
 * @brief 自定义黑体字体(SimHei)声明 —— 覆盖界面全部汉字, 由 lv_font_conv 生成
 *        生成脚本字符集: 界面已用汉字 + 常用预留字, 后续加新汉字需重新生成
 */
#ifndef UI_FONTS_H
#define UI_FONTS_H

#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 黑体 16px: 日期/星期/页脚/统计栏等小字 */
extern const lv_font_t lv_font_heiti_16;

/* 黑体 20px: 系统标题/卡片标题 */
extern const lv_font_t lv_font_heiti_20;

#ifdef __cplusplus
}
#endif

#endif /* UI_FONTS_H */
