#pragma once

/* ============================================================================
 * UI Widgets —— 设计系统的"原子组件"
 *
 * 三件套：
 *   ui_card     —— 圆角面板，统一 padding / border / 背景
 *   ui_kv_row   —— key-value 一行（左 label，右 value），自带底部分隔线
 *   ui_icon_btn —— 透明按钮 + 居中图标，按下有反馈
 *
 * 所有组件返回 lv_obj_t*，调用方负责 align / size / 进一步样式微调。
 * 内部统一用 UI_C_* / UI_SP_* / UI_R_*，换皮肤只需改 ui_tokens.h。
 * ========================================================================= */

#include "lvgl.h"
#include "ui_tokens.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 卡片 ----
 * 使用：
 *   lv_obj_t *card = ui_card(parent);
 *   lv_obj_set_size(card, 220, 70);
 *   lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 100);
 *   // 自由往里加内容
 */
lv_obj_t *ui_card(lv_obj_t *parent);

/* 强调色卡片（边框带 accent，用于突出某个区块） */
lv_obj_t *ui_card_accent(lv_obj_t *parent);

/* ---- KV 行 ----
 * 自带 flex（左右对齐），自带底部 1px 分隔线。
 * 返回 row 容器；通过 out_value 拿到右侧 label 指针以便后续 set_text。
 *
 * 使用：
 *   lv_obj_t *val;
 *   ui_kv_row(card, "湿度", "--%", &val);
 *   ...
 *   lv_label_set_text(val, "65%");
 *
 * with_divider=false 时不画底分隔线（最后一行用）。
 */
lv_obj_t *ui_kv_row(lv_obj_t *parent, const char *key, const char *value,
                     lv_obj_t **out_value, bool with_divider);

/* ---- 列表行（设置页等用）----
 * 三段式：左 24px 彩色图标（Material 字体）+ 中 label + 右值 + chevron。
 *
 * - parent 应是 ui_card / 普通容器；本组件自带 48px 行高 + 底部分隔线。
 * - icon 传 ICON_* UTF-8 字面量；icon_color 控制其颜色（一图一色风格）。
 * - value 可传 NULL，此时右侧只有 chevron。
 * - out_value 不为 NULL 时，回填右侧 value label 指针，方便后续 set_text 更新。
 *
 * 整行带 LV_OBJ_FLAG_CLICKABLE，调用方挂 LV_EVENT_CLICKED 即可。
 *
 * 使用：
 *   lv_obj_t *val;
 *   lv_obj_t *row = ui_list_row(card, ICON_BRIGHTNESS, "亮度", "中",
 *                               UI_C_WARN, &val);
 *   lv_obj_add_event_cb(row, on_bright_clicked, LV_EVENT_CLICKED, NULL);
 *   ...
 *   lv_label_set_text(val, "高");
 */
lv_obj_t *ui_list_row(lv_obj_t *parent,
                       const char *icon, const char *label, const char *value,
                       lv_color_t icon_color, lv_obj_t **out_value);

/* ---- 图标按钮 ----
 * symbol 传 LV_SYMBOL_LEFT / "\xEF\x83\x90" 等 FontAwesome 字符串。
 * w/h 给 0 用默认 36×30。
 *
 * 使用：
 *   lv_obj_t *btn = ui_icon_btn(parent, LV_SYMBOL_LEFT, 36, 30);
 *   lv_obj_align(btn, LV_ALIGN_TOP_LEFT, 8, 8);
 *   lv_obj_add_event_cb(btn, on_back, LV_EVENT_CLICKED, NULL);
 */
lv_obj_t *ui_icon_btn(lv_obj_t *parent, const char *symbol, int w, int h);

/* ---- 屏幕背景 ----
 * 把传入的 screen 整个底色刷成 UI_C_BG，去 padding / scroll bar。
 * 每个新页 create() 后第一行调用即可。
 */
void ui_screen_setup(lv_obj_t *screen);

#ifdef __cplusplus
}
#endif
