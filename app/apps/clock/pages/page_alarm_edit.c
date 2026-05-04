#include "page_alarm_edit.h"
#include "clock_app.h"
#include "clock_ui_common.h"
#include "app_shell_ui.h"

#include "esp_log.h"
#include "lvgl.h"
#include "ui_tokens.h"
#include "ui_widgets.h"
#include "app_fonts.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "page_alarm_edit";

/* 闹钟编辑（仅 UI；不真的保存）：
 *   - 标题"添加闹钟"
 *   - 大数字 hh:mm 预览
 *   - 上下两组 stepper（▲ 时 ▼ / ▲ 分 ▼）
 *   - 底部"取消" / "保存"
 *   - hit zone：子页不放，改顶部"<" 按钮 pop
 */

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *time_lbl;
    int hour;
    int minute;
} ui_t;
static ui_t s_ui;

static void update_preview(void)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d", s_ui.hour, s_ui.minute);
    lv_label_set_text(s_ui.time_lbl, buf);
}

static void on_hour_up(lv_event_t *e)  { (void)e; s_ui.hour = (s_ui.hour + 1) % 24; update_preview(); }
static void on_hour_dn(lv_event_t *e)  { (void)e; s_ui.hour = (s_ui.hour + 23) % 24; update_preview(); }
static void on_min_up(lv_event_t *e)   { (void)e; s_ui.minute = (s_ui.minute + 1) % 60; update_preview(); }
static void on_min_dn(lv_event_t *e)   { (void)e; s_ui.minute = (s_ui.minute + 59) % 60; update_preview(); }

static void on_back(lv_event_t *e)
{
    (void)e;
    clock_app_close_alarm_edit();
}

static void on_save(lv_event_t *e)
{
    (void)e;
    /* 占位：暂不真的保存 */
    ESP_LOGI(TAG, "save alarm %02d:%02d (TODO: 写 NVS)", s_ui.hour, s_ui.minute);
    clock_app_close_alarm_edit();
}

/* ============================================================================
 * 视图
 * ========================================================================= */

static lv_obj_t *make_step_btn(lv_obj_t *parent, const char *symbol,
                                 lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 56, 28);
    lv_obj_set_style_bg_color(btn, UI_C_PANEL, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, UI_C_PANEL_HI, LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_border_color(btn, UI_C_BORDER, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *l = lv_label_create(btn);
    lv_obj_set_style_text_font(l, APP_FONT_TITLE, 0);
    lv_obj_set_style_text_color(l, UI_C_ACCENT, 0);
    lv_label_set_text(l, symbol);
    lv_obj_center(l);
    return btn;
}

static lv_obj_t *make_action_btn(lv_obj_t *parent, const char *text,
                                   lv_color_t fg, lv_color_t bg,
                                   lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 100, 36);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, UI_R_LG, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *l = lv_label_create(btn);
    lv_obj_set_style_text_font(l, APP_FONT_TEXT, 0);
    lv_obj_set_style_text_color(l, fg, 0);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return btn;
}

static lv_obj_t *create(void)
{
    ESP_LOGI(TAG, "create");
    memset(&s_ui, 0, sizeof(s_ui));
    s_ui.hour = 7;
    s_ui.minute = 30;

    s_ui.screen = lv_obj_create(NULL);
    ui_screen_setup(s_ui.screen);
    lv_obj_clear_flag(s_ui.screen, LV_OBJ_FLAG_SCROLLABLE);

    app_shell_attach_statusbar(s_ui.screen, false);

    /* 顶部栏：< 返回 + "添加闹钟" 标题 */
    lv_obj_t *back = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, 32, 32);
    lv_obj_set_pos(back, UI_SP_XS, 24);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(back, UI_C_PANEL_HI, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_radius(back, 6, 0);
    lv_obj_clear_flag(back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_lbl = lv_label_create(back);
    lv_obj_set_style_text_font(back_lbl, APP_FONT_TITLE, 0);
    lv_obj_set_style_text_color(back_lbl, UI_C_ACCENT, 0);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_center(back_lbl);

    lv_obj_t *title = lv_label_create(s_ui.screen);
    lv_label_set_text(title, "添加闹钟");
    lv_obj_set_style_text_font(title, APP_FONT_TITLE, 0);
    lv_obj_set_style_text_color(title, UI_C_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

    /* 大数字预览 */
    s_ui.time_lbl = lv_label_create(s_ui.screen);
    lv_obj_set_style_text_font(s_ui.time_lbl, APP_FONT_HUGE, 0);
    lv_obj_set_style_text_color(s_ui.time_lbl, UI_C_TEXT, 0);
    lv_obj_align(s_ui.time_lbl, LV_ALIGN_TOP_MID, 0, 80);

    update_preview();

    /* stepper：上排 ▲ ▲，中间 hh:mm，下排 ▼ ▼
     * 用 4 个独立按钮，分别对应 时▲ 分▲ 时▼ 分▼ */
    /* 时 ▲ 在数字左上方，分 ▲ 在右上方 */
    lv_obj_t *h_up = make_step_btn(s_ui.screen, LV_SYMBOL_UP,   on_hour_up);
    lv_obj_align_to(h_up, s_ui.time_lbl, LV_ALIGN_OUT_TOP_LEFT, -8, -10);

    lv_obj_t *m_up = make_step_btn(s_ui.screen, LV_SYMBOL_UP,   on_min_up);
    lv_obj_align_to(m_up, s_ui.time_lbl, LV_ALIGN_OUT_TOP_RIGHT, 8, -10);

    lv_obj_t *h_dn = make_step_btn(s_ui.screen, LV_SYMBOL_DOWN, on_hour_dn);
    lv_obj_align_to(h_dn, s_ui.time_lbl, LV_ALIGN_OUT_BOTTOM_LEFT, -8, 10);

    lv_obj_t *m_dn = make_step_btn(s_ui.screen, LV_SYMBOL_DOWN, on_min_dn);
    lv_obj_align_to(m_dn, s_ui.time_lbl, LV_ALIGN_OUT_BOTTOM_RIGHT, 8, 10);

    /* 底部按钮：取消 + 保存 */
    lv_obj_t *cancel = make_action_btn(s_ui.screen, "取消",
                                         UI_C_TEXT, UI_C_PANEL_HI, on_back);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, UI_SP_MD, -UI_SP_MD);

    lv_obj_t *save = make_action_btn(s_ui.screen, "保存",
                                       lv_color_hex(0xFFFFFF), UI_C_ACCENT, on_save);
    lv_obj_align(save, LV_ALIGN_BOTTOM_RIGHT, -UI_SP_MD, -UI_SP_MD);

    return s_ui.screen;
}

static void destroy(void)
{
    ESP_LOGI(TAG, "destroy");
    if (s_ui.screen) lv_obj_del(s_ui.screen);
    memset(&s_ui, 0, sizeof(s_ui));
}

static const page_callbacks_t s_cb = {
    .create  = create,
    .destroy = destroy,
    .update  = NULL,
};

const page_callbacks_t *page_alarm_edit_get_callbacks(void)
{
    return &s_cb;
}
