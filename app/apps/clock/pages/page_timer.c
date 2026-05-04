#include "page_timer.h"
#include "clock_app.h"
#include "clock_ui_common.h"
#include "app_shell_ui.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "ui_tokens.h"
#include "ui_widgets.h"
#include "app_fonts.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "page_timer";

/* 预设档位（秒）*/
typedef struct {
    const char *label;
    int total_sec;
} preset_t;
static const preset_t PRESETS[] = {
    { "1分",  60 },
    { "3分",  180 },
    { "5分",  300 },
    { "10分", 600 },
    { "30分", 1800 },
};
#define PRESET_CNT (int)(sizeof(PRESETS) / sizeof(PRESETS[0]))

typedef enum {
    TM_IDLE = 0,
    TM_RUNNING,
    TM_PAUSED,
    TM_DONE,
} tm_state_t;

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *arc;
    lv_obj_t *time_lbl;
    lv_obj_t *hint_lbl;
    lv_obj_t *preset_chips[PRESET_CNT];
    int       preset_idx;

    lv_obj_t *btn_left;
    lv_obj_t *btn_right;
    lv_obj_t *btn_left_lbl;
    lv_obj_t *btn_right_lbl;

    tm_state_t state;
    int        total_sec;
    int64_t    start_us;
    int64_t    paused_left_us;
} ui_t;
static ui_t s_ui;

/* ============================================================================
 * 状态计算
 * ========================================================================= */

static int64_t left_us(void)
{
    if (s_ui.state == TM_RUNNING) {
        int64_t total_us = (int64_t)s_ui.total_sec * 1000000;
        int64_t spent = esp_timer_get_time() - s_ui.start_us;
        int64_t left = total_us - spent;
        return left > 0 ? left : 0;
    }
    if (s_ui.state == TM_PAUSED) return s_ui.paused_left_us;
    if (s_ui.state == TM_IDLE)   return (int64_t)s_ui.total_sec * 1000000;
    return 0;
}

static void format_time(int64_t us, char *out, size_t sz)
{
    int total_sec = (int)((us + 999999) / 1000000);   /* 向上取整避免显示 0 */
    if (total_sec < 0) total_sec = 0;
    int sec = total_sec % 60;
    int min = total_sec / 60;
    if (min > 99) min = 99;
    snprintf(out, sz, "%02d:%02d", min, sec);
}

/* ============================================================================
 * 视图刷新
 * ========================================================================= */

static void refresh_time(void)
{
    char buf[16];
    format_time(left_us(), buf, sizeof(buf));
    lv_label_set_text(s_ui.time_lbl, buf);

    /* 进度环：剩余比例 */
    int total_us = s_ui.total_sec * 1000000;
    int progress = 0;
    if (total_us > 0) {
        int64_t lu = left_us();
        progress = (int)((lu * 100) / total_us);
        if (progress < 0) progress = 0;
        if (progress > 100) progress = 100;
    }
    lv_arc_set_value(s_ui.arc, progress);
}

static void update_buttons(void)
{
    switch (s_ui.state) {
    case TM_IDLE:
        lv_obj_set_style_bg_color(s_ui.btn_left, UI_C_TEXT_MUTED, 0);
        lv_obj_set_style_bg_opa(s_ui.btn_left, LV_OPA_30, 0);
        lv_label_set_text(s_ui.btn_left_lbl, "取消");
        lv_obj_set_style_bg_color(s_ui.btn_right, lv_color_hex(0x34C759), 0);
        lv_obj_set_style_bg_opa(s_ui.btn_right, LV_OPA_COVER, 0);
        lv_label_set_text(s_ui.btn_right_lbl, "开始");
        lv_label_set_text(s_ui.hint_lbl, "剩余");
        break;
    case TM_RUNNING:
        lv_obj_set_style_bg_color(s_ui.btn_left, UI_C_TEXT_MUTED, 0);
        lv_obj_set_style_bg_opa(s_ui.btn_left, LV_OPA_COVER, 0);
        lv_label_set_text(s_ui.btn_left_lbl, "取消");
        lv_obj_set_style_bg_color(s_ui.btn_right, lv_color_hex(0xFF9500), 0);
        lv_obj_set_style_bg_opa(s_ui.btn_right, LV_OPA_COVER, 0);
        lv_label_set_text(s_ui.btn_right_lbl, "暂停");
        lv_label_set_text(s_ui.hint_lbl, "剩余");
        break;
    case TM_PAUSED:
        lv_obj_set_style_bg_color(s_ui.btn_left, UI_C_TEXT_MUTED, 0);
        lv_obj_set_style_bg_opa(s_ui.btn_left, LV_OPA_COVER, 0);
        lv_label_set_text(s_ui.btn_left_lbl, "取消");
        lv_obj_set_style_bg_color(s_ui.btn_right, lv_color_hex(0x34C759), 0);
        lv_obj_set_style_bg_opa(s_ui.btn_right, LV_OPA_COVER, 0);
        lv_label_set_text(s_ui.btn_right_lbl, "继续");
        lv_label_set_text(s_ui.hint_lbl, "已暂停");
        break;
    case TM_DONE:
        lv_obj_set_style_bg_color(s_ui.btn_left, UI_C_TEXT_MUTED, 0);
        lv_obj_set_style_bg_opa(s_ui.btn_left, LV_OPA_COVER, 0);
        lv_label_set_text(s_ui.btn_left_lbl, "取消");
        lv_obj_set_style_bg_color(s_ui.btn_right, lv_color_hex(0x34C759), 0);
        lv_obj_set_style_bg_opa(s_ui.btn_right, LV_OPA_COVER, 0);
        lv_label_set_text(s_ui.btn_right_lbl, "重启");
        lv_label_set_text(s_ui.hint_lbl, "结束");
        break;
    }
}

static void update_chips(void)
{
    for (int i = 0; i < PRESET_CNT; i++) {
        bool active = (i == s_ui.preset_idx);
        lv_obj_t *c = s_ui.preset_chips[i];
        lv_obj_set_style_bg_color(c, active ? UI_C_ACCENT : UI_C_PANEL, 0);
        lv_obj_set_style_border_color(c, active ? UI_C_ACCENT : UI_C_BORDER, 0);
        lv_obj_t *lbl = lv_obj_get_child(c, 0);
        if (lbl) {
            lv_obj_set_style_text_color(lbl,
                active ? lv_color_hex(0xFFFFFF) : UI_C_TEXT, 0);
        }
    }
}

/* ============================================================================
 * 事件
 * ========================================================================= */

static void on_chip_clicked(lv_event_t *e)
{
    if (s_ui.state == TM_RUNNING) return;   /* 跑着不让换 */
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    s_ui.preset_idx = idx;
    s_ui.total_sec = PRESETS[idx].total_sec;
    s_ui.state = TM_IDLE;
    update_chips();
    update_buttons();
    refresh_time();
}

static void on_left_clicked(lv_event_t *e)
{
    (void)e;
    /* 取消 */
    s_ui.state = TM_IDLE;
    s_ui.paused_left_us = 0;
    update_buttons();
    refresh_time();
}

static void on_right_clicked(lv_event_t *e)
{
    (void)e;
    int64_t now = esp_timer_get_time();
    if (s_ui.state == TM_IDLE) {
        s_ui.start_us = now;
        s_ui.state = TM_RUNNING;
    } else if (s_ui.state == TM_RUNNING) {
        s_ui.paused_left_us = left_us();
        s_ui.state = TM_PAUSED;
    } else if (s_ui.state == TM_PAUSED) {
        /* 用 paused_left_us 推算新 start */
        int64_t total_us = (int64_t)s_ui.total_sec * 1000000;
        s_ui.start_us = now - (total_us - s_ui.paused_left_us);
        s_ui.state = TM_RUNNING;
    } else if (s_ui.state == TM_DONE) {
        s_ui.start_us = now;
        s_ui.state = TM_RUNNING;
    }
    update_buttons();
    refresh_time();
}

/* ============================================================================
 * 视图构建
 * ========================================================================= */

static lv_obj_t *make_arc(lv_obj_t *parent)
{
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_set_size(arc, 180, 180);
    lv_obj_align(arc, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_arc_set_rotation(arc, 270);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_value(arc, 100);
    lv_obj_set_style_arc_color(arc, UI_C_BORDER, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, UI_C_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_INDICATOR);
    return arc;
}

static lv_obj_t *make_chip(lv_obj_t *parent, int idx, const preset_t *p)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, LV_SIZE_CONTENT, 22);
    lv_obj_set_style_bg_color(c, UI_C_PANEL, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(c, UI_C_BORDER, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_radius(c, 11, 0);
    lv_obj_set_style_pad_left(c, 8, 0);
    lv_obj_set_style_pad_right(c, 8, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(c, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(c, on_chip_clicked, LV_EVENT_CLICKED,
                         (void *)(intptr_t)idx);

    lv_obj_t *lbl = lv_label_create(c);
    lv_obj_set_style_text_font(lbl, APP_FONT_TEXT, 0);
    lv_label_set_text(lbl, p->label);
    lv_obj_center(lbl);
    return c;
}

static lv_obj_t *make_round_btn(lv_obj_t *parent, int x_align,
                                  lv_event_cb_t cb, lv_obj_t **out_lbl)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 56, 56);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, x_align, 0);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_obj_set_style_text_font(lbl, APP_FONT_TEXT, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(lbl, "--");
    lv_obj_center(lbl);
    if (out_lbl) *out_lbl = lbl;
    return btn;
}

static lv_obj_t *create(void)
{
    ESP_LOGI(TAG, "create");
    memset(&s_ui, 0, sizeof(s_ui));
    s_ui.preset_idx = 2;            /* 默认 5 分钟 */
    s_ui.total_sec = PRESETS[2].total_sec;
    s_ui.state = TM_IDLE;

    s_ui.screen = lv_obj_create(NULL);
    ui_screen_setup(s_ui.screen);
    lv_obj_clear_flag(s_ui.screen, LV_OBJ_FLAG_SCROLLABLE);

    app_shell_attach_statusbar(s_ui.screen, false);
    clk_make_tabbar(s_ui.screen);

    lv_obj_t *content = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, 240, CLK_CONTENT_H);
    lv_obj_set_pos(content, 0, CLK_CONTENT_TOP);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    /* 进度环（180×180）含中央时间 + hint */
    s_ui.arc = make_arc(content);

    s_ui.time_lbl = lv_label_create(s_ui.arc);
    lv_obj_set_style_text_font(s_ui.time_lbl, APP_FONT_HUGE, 0);
    lv_obj_set_style_text_color(s_ui.time_lbl, UI_C_TEXT, 0);
    lv_label_set_text(s_ui.time_lbl, "05:00");
    lv_obj_align(s_ui.time_lbl, LV_ALIGN_CENTER, 0, -10);

    s_ui.hint_lbl = lv_label_create(s_ui.arc);
    lv_obj_set_style_text_font(s_ui.hint_lbl, APP_FONT_TEXT, 0);
    lv_obj_set_style_text_color(s_ui.hint_lbl, UI_C_TEXT_MUTED, 0);
    lv_label_set_text(s_ui.hint_lbl, "剩余");
    lv_obj_align(s_ui.hint_lbl, LV_ALIGN_CENTER, 0, 24);

    /* 预设 chips 行 */
    lv_obj_t *chip_row = lv_obj_create(content);
    lv_obj_remove_style_all(chip_row);
    lv_obj_set_size(chip_row, 240, 26);
    lv_obj_set_pos(chip_row, 0, 188);
    lv_obj_set_style_bg_opa(chip_row, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(chip_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(chip_row, LV_FLEX_ALIGN_CENTER,
                           LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(chip_row, 6, 0);
    lv_obj_clear_flag(chip_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(chip_row, LV_OBJ_FLAG_EVENT_BUBBLE);
    for (int i = 0; i < PRESET_CNT; i++) {
        s_ui.preset_chips[i] = make_chip(chip_row, i, &PRESETS[i]);
    }

    /* 按钮行 */
    lv_obj_t *btn_row = lv_obj_create(content);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_size(btn_row, 240, 56);
    lv_obj_set_pos(btn_row, 0, 222 - 8);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    s_ui.btn_left  = make_round_btn(btn_row, -52, on_left_clicked,  &s_ui.btn_left_lbl);
    s_ui.btn_right = make_round_btn(btn_row,  52, on_right_clicked, &s_ui.btn_right_lbl);

    update_chips();
    update_buttons();
    refresh_time();

    clk_attach_hit_and_swipe(s_ui.screen);
    return s_ui.screen;
}

static void destroy(void)
{
    ESP_LOGI(TAG, "destroy");
    /* 退出清零 */
    if (s_ui.screen) lv_obj_del(s_ui.screen);
    memset(&s_ui, 0, sizeof(s_ui));
}

static void update_cb(void)
{
    if (s_ui.state == TM_RUNNING) {
        if (left_us() == 0) {
            s_ui.state = TM_DONE;
            update_buttons();
        }
        refresh_time();
    }
}

static const page_callbacks_t s_cb = {
    .create  = create,
    .destroy = destroy,
    .update  = update_cb,
};

const page_callbacks_t *page_timer_get_callbacks(void)
{
    return &s_cb;
}
