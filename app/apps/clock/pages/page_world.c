#include "page_world.h"
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
#include <time.h>

static const char *TAG = "page_world";

/* 城市预设：本地基准 = 北京（UTC+8）。其它列偏移按整数小时差。 */
typedef struct {
    const char *name;
    int         delta_hour;   /* 相对北京的小时差 */
    const char *delta_label;  /* 显示文本 */
} city_t;

static const city_t CITIES[] = {
    { "北京", 0,  "本地时间" },
    { "东京", 1,  "+1h" },
    { "伦敦", -7, "-7h" },
    { "纽约", -13,"-13h · 昨天" },
};
#define CITY_CNT (int)(sizeof(CITIES) / sizeof(CITIES[0]))

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *list;
    lv_obj_t *time_lbls[CITY_CNT];
    int       last_minute;
} ui_t;
static ui_t s_ui;

static void update_times(void)
{
    time_t now;
    struct tm t;
    time(&now);
    localtime_r(&now, &t);
    if (t.tm_min == s_ui.last_minute) return;
    s_ui.last_minute = t.tm_min;

    char buf[12];
    for (int i = 0; i < CITY_CNT; i++) {
        int h = (t.tm_hour + CITIES[i].delta_hour + 24) % 24;
        snprintf(buf, sizeof(buf), "%02d:%02d", h, t.tm_min);
        if (s_ui.time_lbls[i]) {
            lv_label_set_text(s_ui.time_lbls[i], buf);
        }
    }
}

static lv_obj_t *make_card(lv_obj_t *parent, const city_t *c, lv_obj_t **out_time)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, lv_pct(100), 56);
    lv_obj_set_style_bg_color(card, UI_C_PANEL, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, UI_C_BORDER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_opa(card, LV_OPA_50, 0);
    lv_obj_set_style_radius(card, UI_R_LG, 0);
    lv_obj_set_style_pad_left(card, UI_SP_MD, 0);
    lv_obj_set_style_pad_right(card, UI_SP_MD, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t *city_lbl = lv_label_create(card);
    lv_label_set_text(city_lbl, c->name);
    lv_obj_set_style_text_font(city_lbl, APP_FONT_TITLE, 0);
    lv_obj_set_style_text_color(city_lbl, UI_C_TEXT, 0);
    lv_obj_align(city_lbl, LV_ALIGN_LEFT_MID, 0, -8);

    lv_obj_t *delta_lbl = lv_label_create(card);
    lv_label_set_text(delta_lbl, c->delta_label);
    lv_obj_set_style_text_font(delta_lbl, APP_FONT_TEXT, 0);
    lv_obj_set_style_text_color(delta_lbl, UI_C_TEXT_MUTED, 0);
    lv_obj_align(delta_lbl, LV_ALIGN_LEFT_MID, 0, 12);

    lv_obj_t *time_lbl = lv_label_create(card);
    lv_label_set_text(time_lbl, "--:--");
    lv_obj_set_style_text_font(time_lbl, APP_FONT_LARGE, 0);
    lv_obj_set_style_text_color(time_lbl, UI_C_TEXT, 0);
    lv_obj_align(time_lbl, LV_ALIGN_RIGHT_MID, 0, 0);
    if (out_time) *out_time = time_lbl;

    return card;
}

static void on_fab_clicked(lv_event_t *e)
{
    (void)e;
    /* 占位：暂未实现"添加城市"，只 log */
    ESP_LOGI(TAG, "add city (TODO)");
}

static lv_obj_t *create(void)
{
    ESP_LOGI(TAG, "create");
    memset(&s_ui, 0, sizeof(s_ui));
    s_ui.last_minute = -1;

    s_ui.screen = lv_obj_create(NULL);
    ui_screen_setup(s_ui.screen);
    lv_obj_clear_flag(s_ui.screen, LV_OBJ_FLAG_SCROLLABLE);

    app_shell_attach_statusbar(s_ui.screen, false);
    clk_make_tabbar(s_ui.screen);

    s_ui.list = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(s_ui.list);
    lv_obj_set_size(s_ui.list, 240, CLK_CONTENT_H);
    lv_obj_set_pos(s_ui.list, 0, CLK_CONTENT_TOP);
    lv_obj_set_style_pad_left(s_ui.list, UI_SP_MD, 0);
    lv_obj_set_style_pad_right(s_ui.list, UI_SP_MD, 0);
    lv_obj_set_style_pad_top(s_ui.list, UI_SP_SM, 0);
    lv_obj_set_flex_flow(s_ui.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_ui.list, UI_SP_SM, 0);
    lv_obj_set_scroll_dir(s_ui.list, LV_DIR_VER);

    for (int i = 0; i < CITY_CNT; i++) {
        make_card(s_ui.list, &CITIES[i], &s_ui.time_lbls[i]);
    }
    update_times();

    /* + FAB */
    lv_obj_t *fab = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(fab);
    lv_obj_set_size(fab, 28, 28);
    lv_obj_set_pos(fab, 240 - 28 - UI_SP_SM, UI_SP_SM);
    lv_obj_set_style_radius(fab, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(fab, UI_C_ACCENT, 0);
    lv_obj_set_style_bg_opa(fab, LV_OPA_COVER, 0);
    lv_obj_clear_flag(fab, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(fab, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(fab, on_fab_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *plus = lv_label_create(fab);
    lv_obj_set_style_text_font(plus, APP_FONT_TITLE, 0);
    lv_obj_set_style_text_color(plus, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(plus, "+");
    lv_obj_center(plus);

    clk_attach_hit_and_swipe(s_ui.screen);
    return s_ui.screen;
}

static void destroy(void)
{
    ESP_LOGI(TAG, "destroy");
    if (s_ui.screen) lv_obj_del(s_ui.screen);
    memset(&s_ui, 0, sizeof(s_ui));
}

static void update_cb(void) { update_times(); }

static const page_callbacks_t s_cb = {
    .create  = create,
    .destroy = destroy,
    .update  = update_cb,
};

const page_callbacks_t *page_world_get_callbacks(void)
{
    return &s_cb;
}
