#include "settings_time.h"
#include "settings_app.h"

#include "esp_log.h"
#include "lvgl.h"

#include "ui_tokens.h"
#include "ui_widgets.h"
#include "app_shell_ui.h"
#include "app_fonts.h"

#include <time.h>
#include <sys/time.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "settings_time";

#define HIT_ZONE_H  30

typedef struct {
    lv_obj_t *screen;

    lv_obj_t *preview_lbl;
    lv_obj_t *date_lbl;

    lv_obj_t *hour_up, *hour_dn;
    lv_obj_t *min_up,  *min_dn;
    lv_obj_t *sec_up,  *sec_dn;
    lv_obj_t *year_up, *year_dn;
    lv_obj_t *mon_up,  *mon_dn;
    lv_obj_t *day_up,  *day_dn;

    int press_y0;
    int press_y_last;
} ui_t;

static ui_t s_ui;

/* ============================================================================
 * stepper（▲ 数字 ▼ + 标签）—— 让 hour/min/sec 等共享同一个外观
 * ========================================================================= */

static lv_obj_t *make_step_btn(lv_obj_t *parent, const char *symbol)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 40, 22);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(btn, UI_C_PANEL_HI, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, symbol);
    lv_obj_set_style_text_font(l, APP_FONT_TEXT, 0);
    lv_obj_set_style_text_color(l, UI_C_ACCENT, 0);
    lv_obj_center(l);
    return btn;
}

/* 单个 stepper 列：上按钮 / 标签 / 下按钮 / 名称
 * y_top 是该 stepper 在卡内的起点；w 是列宽
 */
static void make_stepper_col(lv_obj_t *card, int x, int w,
                              const char *name,
                              lv_obj_t **out_up, lv_obj_t **out_dn)
{
    /* 列容器 */
    lv_obj_t *col = lv_obj_create(card);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, w, LV_SIZE_CONTENT);
    lv_obj_set_pos(col, x, 0);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 2, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(col, LV_OBJ_FLAG_EVENT_BUBBLE);

    *out_up = make_step_btn(col, LV_SYMBOL_UP);
    *out_dn = make_step_btn(col, LV_SYMBOL_DOWN);
    /* up 在最上，dn 在 up 之下，再下面是名字 — 但视觉上想要 ▲ 数字 ▼ 三明治。
     * 数字其实是预览（preview_lbl），不放在 stepper 列里；这里 stepper 只放 ▲ ▼ + 名 */
    lv_obj_t *nm = lv_label_create(col);
    lv_obj_set_style_text_font(nm, APP_FONT_TEXT, 0);
    lv_obj_set_style_text_color(nm, UI_C_TEXT_MUTED, 0);
    lv_label_set_text(nm, name);
}

static lv_obj_t *make_card(lv_obj_t *parent, int y, int height)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, 220, height);
    lv_obj_set_pos(c, (240 - 220) / 2, y);
    lv_obj_set_style_bg_color(c, UI_C_PANEL, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(c, UI_C_BORDER, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_opa(c, LV_OPA_50, 0);
    lv_obj_set_style_radius(c, UI_R_LG, 0);
    lv_obj_set_style_pad_all(c, UI_SP_SM, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(c, LV_OBJ_FLAG_EVENT_BUBBLE);
    return c;
}

/* ============================================================================
 * 业务
 * ========================================================================= */

static void adjust_time(int hour_d, int min_d, int sec_d)
{
    time_t now; struct tm t;
    time(&now); localtime_r(&now, &t);
    t.tm_hour += hour_d; t.tm_min += min_d; t.tm_sec += sec_d;
    time_t new_t = mktime(&t);
    struct timeval tv = { .tv_sec = new_t, .tv_usec = 0 };
    settimeofday(&tv, NULL);
}

static void adjust_date(int year_d, int mon_d, int day_d)
{
    time_t now; struct tm t;
    time(&now); localtime_r(&now, &t);
    t.tm_year += year_d; t.tm_mon += mon_d; t.tm_mday += day_d;
    time_t new_t = mktime(&t);
    struct timeval tv = { .tv_sec = new_t, .tv_usec = 0 };
    settimeofday(&tv, NULL);
}

static void on_hour_up(lv_event_t *e) { (void)e; adjust_time(1, 0, 0); }
static void on_hour_dn(lv_event_t *e) { (void)e; adjust_time(-1, 0, 0); }
static void on_min_up(lv_event_t *e)  { (void)e; adjust_time(0, 1, 0); }
static void on_min_dn(lv_event_t *e)  { (void)e; adjust_time(0, -1, 0); }
static void on_sec_up(lv_event_t *e)  { (void)e; adjust_time(0, 0, 1); }
static void on_sec_dn(lv_event_t *e)  { (void)e; adjust_time(0, 0, -1); }
static void on_year_up(lv_event_t *e) { (void)e; adjust_date(1, 0, 0); }
static void on_year_dn(lv_event_t *e) { (void)e; adjust_date(-1, 0, 0); }
static void on_mon_up(lv_event_t *e)  { (void)e; adjust_date(0, 1, 0); }
static void on_mon_dn(lv_event_t *e)  { (void)e; adjust_date(0, -1, 0); }
static void on_day_up(lv_event_t *e)  { (void)e; adjust_date(0, 0, 1); }
static void on_day_dn(lv_event_t *e)  { (void)e; adjust_date(0, 0, -1); }

/* ============================================================================
 * 上滑退出
 * ========================================================================= */

static void on_hit_pressed(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) { s_ui.press_y0 = -1; return; }
    lv_point_t p; lv_indev_get_point(indev, &p);
    s_ui.press_y0 = p.y;
    s_ui.press_y_last = p.y;
}
static void on_hit_pressing(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p; lv_indev_get_point(indev, &p);
    s_ui.press_y_last = p.y;
}
static void on_hit_released(lv_event_t *e)
{
    (void)e;
    if (s_ui.press_y0 < 0) return;
    int dy = s_ui.press_y0 - s_ui.press_y_last;
    s_ui.press_y0 = -1;
    if (dy >= 30) settings_app_pop_or_exit();
}

/* ============================================================================
 * 视图
 * ========================================================================= */

static void create_title(lv_obj_t *parent)
{
    lv_obj_t *t = lv_label_create(parent);
    lv_label_set_text(t, "时间调节");
    lv_obj_set_style_text_font(t, APP_FONT_TITLE, 0);
    lv_obj_set_style_text_color(t, UI_C_TEXT, 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, UI_SP_MD, 24 + UI_SP_SM);
}

static void create_preview(lv_obj_t *parent)
{
    /* 大数字预览 */
    s_ui.preview_lbl = lv_label_create(parent);
    lv_label_set_text(s_ui.preview_lbl, "--:--:--");
    lv_obj_set_style_text_font(s_ui.preview_lbl, APP_FONT_LARGE, 0);
    lv_obj_set_style_text_color(s_ui.preview_lbl, UI_C_TEXT, 0);
    lv_obj_align(s_ui.preview_lbl, LV_ALIGN_TOP_MID, 0, 60);

    /* 日期 */
    s_ui.date_lbl = lv_label_create(parent);
    lv_label_set_text(s_ui.date_lbl, "----");
    lv_obj_set_style_text_font(s_ui.date_lbl, APP_FONT_TEXT, 0);
    lv_obj_set_style_text_color(s_ui.date_lbl, UI_C_TEXT_MUTED, 0);
    lv_obj_align(s_ui.date_lbl, LV_ALIGN_TOP_MID, 0, 92);
}

static void create_steppers(lv_obj_t *parent)
{
    /* time stepper card y=120, h=72 */
    lv_obj_t *t_card = make_card(parent, 120, 72);
    int col_w = 64;
    int gap = (220 - 2 * UI_SP_SM - 3 * col_w) / 2;
    int x0 = 0;
    make_stepper_col(t_card, x0,                  col_w, "时", &s_ui.hour_up, &s_ui.hour_dn);
    make_stepper_col(t_card, x0 + col_w + gap,    col_w, "分", &s_ui.min_up,  &s_ui.min_dn);
    make_stepper_col(t_card, x0 + 2*(col_w + gap),col_w, "秒", &s_ui.sec_up,  &s_ui.sec_dn);

    /* date stepper card y=200, h=72 */
    lv_obj_t *d_card = make_card(parent, 200, 72);
    make_stepper_col(d_card, x0,                  col_w, "年", &s_ui.year_up, &s_ui.year_dn);
    make_stepper_col(d_card, x0 + col_w + gap,    col_w, "月", &s_ui.mon_up,  &s_ui.mon_dn);
    make_stepper_col(d_card, x0 + 2*(col_w + gap),col_w, "日", &s_ui.day_up,  &s_ui.day_dn);

    /* 事件 */
    lv_obj_add_event_cb(s_ui.hour_up, on_hour_up, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_ui.hour_dn, on_hour_dn, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_ui.min_up,  on_min_up,  LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_ui.min_dn,  on_min_dn,  LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_ui.sec_up,  on_sec_up,  LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_ui.sec_dn,  on_sec_dn,  LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_ui.year_up, on_year_up, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_ui.year_dn, on_year_dn, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_ui.mon_up,  on_mon_up,  LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_ui.mon_dn,  on_mon_dn,  LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_ui.day_up,  on_day_up,  LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_ui.day_dn,  on_day_dn,  LV_EVENT_CLICKED, NULL);
}

static void update_display(void)
{
    if (!s_ui.preview_lbl) return;
    time_t now; struct tm t;
    time(&now); localtime_r(&now, &t);

    char buf[64];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
    lv_label_set_text(s_ui.preview_lbl, buf);

    static const char *WEEK[] = {"日","一","二","三","四","五","六"};
    snprintf(buf, sizeof(buf), "%d 年 %d 月 %d 日 · 周%s",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, WEEK[t.tm_wday % 7]);
    lv_label_set_text(s_ui.date_lbl, buf);
}

/* ============================================================================
 * 生命周期
 * ========================================================================= */

static lv_obj_t *create(void)
{
    ESP_LOGI(TAG, "create");
    memset(&s_ui, 0, sizeof(s_ui));
    s_ui.press_y0 = -1;

    s_ui.screen = lv_obj_create(NULL);
    ui_screen_setup(s_ui.screen);
    lv_obj_clear_flag(s_ui.screen, LV_OBJ_FLAG_SCROLLABLE);

    app_shell_attach_statusbar(s_ui.screen, false);
    create_title(s_ui.screen);
    create_preview(s_ui.screen);
    create_steppers(s_ui.screen);
    update_display();

    /* hit zone */
    lv_obj_t *hit = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(hit);
    lv_obj_set_size(hit, 240, HIT_ZONE_H);
    lv_obj_align(hit, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(hit, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(hit, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(hit, on_hit_pressed,  LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(hit, on_hit_pressing, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(hit, on_hit_released, LV_EVENT_RELEASED, NULL);

    return s_ui.screen;
}

static void destroy(void)
{
    ESP_LOGI(TAG, "destroy");
    if (s_ui.screen) lv_obj_del(s_ui.screen);
    memset(&s_ui, 0, sizeof(s_ui));
    s_ui.press_y0 = -1;
}

static void update_cb(void) { update_display(); }

static const page_callbacks_t s_callbacks = {
    .create  = create,
    .destroy = destroy,
    .update  = update_cb,
};

const page_callbacks_t *settings_time_get_callbacks(void)
{
    return &s_callbacks;
}
