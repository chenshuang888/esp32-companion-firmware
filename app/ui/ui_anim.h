#pragma once

/* ============================================================================
 * UI Anim —— 三个常用的微动画封装
 *
 *   ui_anim_fade_in(obj, delay_ms)
 *      入场淡入 + 微微下方滑入 12px。staggered 时调用方递增 delay_ms。
 *
 *   ui_anim_press_feedback(obj)
 *      给可点击对象挂自动 scale 0.96 反馈（注册一次性事件回调）。
 *
 *   ui_anim_number_rolling(label, from, to, duration_ms, suffix)
 *      label 数字从 from 滚动到 to，suffix 拼接到末尾（如 "°" "%"）。
 *      支持负数和小数（小数仅 1 位）。suffix 可为 NULL。
 * ========================================================================= */

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_anim_fade_in(lv_obj_t *obj, uint32_t delay_ms);

void ui_anim_press_feedback(lv_obj_t *obj);

/* 整数版本：from/to 是 ×10 定点（与 weather temp_c_x10 同语义），
 * decimals=0 显示整数，decimals=1 显示一位小数 */
void ui_anim_number_rolling(lv_obj_t *label, int from_x10, int to_x10,
                             uint32_t duration_ms, int decimals,
                             const char *suffix);

#ifdef __cplusplus
}
#endif
