#include "page_stopwatch.h"
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

static const char *TAG = "page_stopwatch";

/* 秒表状态机：
 *   IDLE     —— 初始 / 重置后
 *   RUNNING  —— 计时中
 *   PAUSED   —— 暂停（保留累计时间）
 *
 * 内部用 esp_timer_get_time() 微秒，UI 转 mm:ss.cc 显示
 * 退出 page 时（destroy）清零（按你定的"退出状态清零"）
 */
typedef enum {
    SW_IDLE = 0,
    SW_RUNNING,
    SW_PAUSED,
} sw_state_t;

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *time_big_lbl;     /* mm:ss */
    lv_obj_t *time_small_lbl;   /* .cc */
    lv_obj_t *btn_left;         /* 计次 / 重置 */
    lv_obj_t *btn_right;        /* 开始 / 停止 */
    lv_obj_t *btn_left_lbl;
    lv_obj_t *btn_right_lbl;

    sw_state_t state;
    int64_t    start_us;        /* 当次 RUNNING 起点 */
    int64_t    accum_us;        /* 历史累计（PAUSED 时保留）*/
} ui_t;
static ui_t s_ui;

/* ============================================================================
 * 时间格式化
 * ========================================================================= */

static int64_t elapsed_us(void)
{
    if (s_ui.state == SW_RUNNING) {
        return s_ui.accum_us + (esp_timer_get_time() - s_ui.start_us);
    }
    return s_ui.accum_us;
}

static void format_and_set(int64_t us)
{
    int total_cs = (int)(us / 10000);          /* centiseconds */
    int cs = total_cs % 100;
    int total_sec = total_cs / 100;
    int sec = total_sec % 60;
    int min = total_sec / 60;
    if (min > 99) min = 99;

    char big[16];
    snprintf(big, sizeof(big), "%02d:%02d", min, sec);
    char small[16];
    snprintf(small, sizeof(small), ".%02d", cs);

    if (s_ui.time_big_lbl)   lv_label_set_text(s_ui.time_big_lbl, big);
    if (s_ui.time_small_lbl) lv_label_set_text(s_ui.time_small_lbl, small);
}

/* ============================================================================
 * 状态切换
 * ========================================================================= */

static void update_buttons(void)
{
    switch (s_ui.state) {
    case SW_IDLE:
        /* 左：计次（禁用灰）/ 右：开始（绿）*/
        lv_obj_set_style_bg_color(s_ui.btn_left, UI_C_TEXT_MUTED, 0);
        lv_obj_set_style_bg_opa(s_ui.btn_left, LV_OPA_30, 0);
        lv_label_set_text(s_ui.btn_left_lbl, "计次");
        lv_obj_set_style_bg_color(s_ui.btn_right, lv_color_hex(0x34C759), 0);
        lv_obj_set_style_bg_opa(s_ui.btn_right, LV_OPA_COVER, 0);
        lv_label_set_text(s_ui.btn_right_lbl, "开始");
        break;
    case SW_RUNNING:
        lv_obj_set_style_bg_color(s_ui.btn_left, UI_C_TEXT_MUTED, 0);
        lv_obj_set_style_bg_opa(s_ui.btn_left, LV_OPA_COVER, 0);
        lv_label_set_text(s_ui.btn_left_lbl, "计次");
        lv_obj_set_style_bg_color(s_ui.btn_right, lv_color_hex(0xFF3B30), 0);
        lv_obj_set_style_bg_opa(s_ui.btn_right, LV_OPA_COVER, 0);
        lv_label_set_text(s_ui.btn_right_lbl, "停止");
        break;
    case SW_PAUSED:
        lv_obj_set_style_bg_color(s_ui.btn_left, UI_C_TEXT_MUTED, 0);
        lv_obj_set_style_bg_opa(s_ui.btn_left, LV_OPA_COVER, 0);
        lv_label_set_text(s_ui.btn_left_lbl, "重置");
        lv_obj_set_style_bg_color(s_ui.btn_right, lv_color_hex(0x34C759), 0);
        lv_obj_set_style_bg_opa(s_ui.btn_right, LV_OPA_COVER, 0);
        lv_label_set_text(s_ui.btn_right_lbl, "开始");
        break;
    }
}

static void on_left_clicked(lv_event_t *e)
{
    (void)e;
    if (s_ui.state == SW_PAUSED) {
        /* 重置 */
        s_ui.accum_us = 0;
        s_ui.state = SW_IDLE;
        format_and_set(0);
        update_buttons();
    } else if (s_ui.state == SW_RUNNING) {
        /* 计次：占位（暂不实现 lap 列表，先 log）*/
        ESP_LOGI(TAG, "lap @ %lld us (TODO: list)", (long long)elapsed_us());
    }
}

static void on_right_clicked(lv_event_t *e)
{
    (void)e;
    if (s_ui.state == SW_RUNNING) {
        /* 停止 → 暂停 */
        s_ui.accum_us += esp_timer_get_time() - s_ui.start_us;
        s_ui.state = SW_PAUSED;
    } else {
        /* 开始（IDLE 或 PAUSED）*/
        s_ui.start_us = esp_timer_get_time();
        s_ui.state = SW_RUNNING;
    }
    update_buttons();
}

/* ============================================================================
 * 视图
 * ========================================================================= */

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

    s_ui.screen = lv_obj_create(NULL);
    ui_screen_setup(s_ui.screen);
    lv_obj_clear_flag(s_ui.screen, LV_OBJ_FLAG_SCROLLABLE);

    app_shell_attach_statusbar(s_ui.screen, false);
    clk_make_tabbar(s_ui.screen);

    /* 内容容器 */
    lv_obj_t *content = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, 240, CLK_CONTENT_H);
    lv_obj_set_pos(content, 0, CLK_CONTENT_TOP);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    /* 中央 180×180 圆框 */
    lv_obj_t *circle = lv_obj_create(content);
    lv_obj_remove_style_all(circle);
    lv_obj_set_size(circle, 180, 180);
    lv_obj_align(circle, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(circle, UI_C_BORDER, 0);
    lv_obj_set_style_border_width(circle, 2, 0);
    lv_obj_set_style_border_opa(circle, LV_OPA_COVER, 0);
    lv_obj_clear_flag(circle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(circle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(circle, LV_OBJ_FLAG_EVENT_BUBBLE);

    s_ui.time_big_lbl = lv_label_create(circle);
    lv_obj_set_style_text_font(s_ui.time_big_lbl, APP_FONT_HUGE, 0);
    lv_obj_set_style_text_color(s_ui.time_big_lbl, UI_C_TEXT, 0);
    lv_label_set_text(s_ui.time_big_lbl, "00:00");
    lv_obj_align(s_ui.time_big_lbl, LV_ALIGN_CENTER, 0, -10);

    s_ui.time_small_lbl = lv_label_create(circle);
    lv_obj_set_style_text_font(s_ui.time_small_lbl, APP_FONT_TEXT, 0);
    lv_obj_set_style_text_color(s_ui.time_small_lbl, UI_C_TEXT_MUTED, 0);
    lv_label_set_text(s_ui.time_small_lbl, ".00");
    lv_obj_align(s_ui.time_small_lbl, LV_ALIGN_CENTER, 0, 24);

    /* 按钮行 */
    lv_obj_t *btn_row = lv_obj_create(content);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_size(btn_row, 240, 56);
    lv_obj_set_pos(btn_row, 0, 192);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    s_ui.btn_left  = make_round_btn(btn_row, -52, on_left_clicked,  &s_ui.btn_left_lbl);
    s_ui.btn_right = make_round_btn(btn_row,  52, on_right_clicked, &s_ui.btn_right_lbl);

    s_ui.state = SW_IDLE;
    s_ui.accum_us = 0;
    update_buttons();
    format_and_set(0);

    clk_attach_hit_and_swipe(s_ui.screen);
    return s_ui.screen;
}

static void destroy(void)
{
    ESP_LOGI(TAG, "destroy");
    /* 退出 → 清零（按你定的规则）*/
    if (s_ui.screen) lv_obj_del(s_ui.screen);
    memset(&s_ui, 0, sizeof(s_ui));
}

static void update_cb(void)
{
    if (s_ui.state == SW_RUNNING) {
        format_and_set(elapsed_us());
    }
}

static const page_callbacks_t s_cb = {
    .create  = create,
    .destroy = destroy,
    .update  = update_cb,
};

const page_callbacks_t *page_stopwatch_get_callbacks(void)
{
    return &s_cb;
}
