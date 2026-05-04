#include "settings_display.h"
#include "settings_app.h"

#include "esp_log.h"
#include "lvgl.h"

#include "ui_tokens.h"
#include "ui_widgets.h"
#include "app_shell_ui.h"
#include "app_fonts.h"

#include "lcd_panel.h"
#include "backlight_storage.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "settings_display";

#define HIT_ZONE_H  30

/* 屏息时长选项（仅 UI；实际熄屏逻辑暂未实现，先存档位）*/
typedef struct {
    const char *label;
    int         value_sec;   /* -1 = 常亮 */
} idle_opt_t;
static const idle_opt_t IDLE_OPTS[] = {
    { "15s",  15 },
    { "30s",  30 },
    { "1min", 60 },
    { "5min", 300 },
    { "常亮", -1 },
};
#define IDLE_OPTS_CNT (int)(sizeof(IDLE_OPTS) / sizeof(IDLE_OPTS[0]))

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *bri_slider;
    lv_obj_t *bri_value_lbl;
    lv_obj_t *idle_value_lbl;
    lv_obj_t *idle_seg_btns[IDLE_OPTS_CNT];
    int       idle_idx;        /* 当前选中的屏息档位 */
    int       press_y0;
    int       press_y_last;
} ui_t;

static ui_t s_ui;

/* ============================================================================
 * 卡片 + helper
 * ========================================================================= */

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
    lv_obj_set_style_pad_all(c, UI_SP_MD, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(c, LV_OBJ_FLAG_EVENT_BUBBLE);
    return c;
}

/* 卡片标题行（左 K + 右 V）*/
static lv_obj_t *make_card_title(lv_obj_t *card, const char *k, const char *v)
{
    lv_obj_t *kl = lv_label_create(card);
    lv_obj_align(kl, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_text_font(kl, APP_FONT_TEXT, 0);
    lv_obj_set_style_text_color(kl, UI_C_TEXT, 0);
    lv_label_set_text(kl, k);

    lv_obj_t *vl = lv_label_create(card);
    lv_obj_align(vl, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_text_font(vl, APP_FONT_TEXT, 0);
    lv_obj_set_style_text_color(vl, UI_C_ACCENT, 0);
    lv_label_set_text(vl, v);
    return vl;
}

/* ============================================================================
 * 亮度 slider 事件
 * ========================================================================= */

static void on_brightness_change(lv_event_t *e)
{
    lv_obj_t *s = lv_event_get_target(e);
    int v = (int)lv_slider_get_value(s);
    /* 0~100 → 0~255 */
    int duty = (v * 255 + 50) / 100;
    if (duty < 8) duty = 8;             /* 不让屏全黑 */
    if (duty > 255) duty = 255;
    lcd_panel_set_backlight((uint8_t)duty);

    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%", v);
    lv_label_set_text(s_ui.bri_value_lbl, buf);
}

static void on_brightness_released(lv_event_t *e)
{
    lv_obj_t *s = lv_event_get_target(e);
    int v = (int)lv_slider_get_value(s);
    int duty = (v * 255 + 50) / 100;
    if (duty < 8) duty = 8;
    if (duty > 255) duty = 255;
    backlight_storage_set((uint8_t)duty);   /* 仅松手时落 NVS，避免拖拽时频繁写 */
    ESP_LOGI(TAG, "brightness saved: %d%% (duty=%d)", v, duty);
}

/* ============================================================================
 * 屏息时长 segmented 事件
 * ========================================================================= */

static void update_idle_seg_visual(void)
{
    for (int i = 0; i < IDLE_OPTS_CNT; i++) {
        bool active = (i == s_ui.idle_idx);
        lv_obj_set_style_bg_color(s_ui.idle_seg_btns[i],
            active ? lv_color_hex(0xFFFFFF) : UI_C_PANEL_HI, 0);
        lv_obj_set_style_bg_opa(s_ui.idle_seg_btns[i], LV_OPA_COVER, 0);
        /* 找按钮内的 label 染色 */
        lv_obj_t *lbl = lv_obj_get_child(s_ui.idle_seg_btns[i], 0);
        if (lbl) {
            lv_obj_set_style_text_color(lbl,
                active ? UI_C_ACCENT : UI_C_TEXT, 0);
        }
    }
    lv_label_set_text(s_ui.idle_value_lbl, IDLE_OPTS[s_ui.idle_idx].label);
}

static void on_idle_opt_clicked(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    s_ui.idle_idx = idx;
    update_idle_seg_visual();
    ESP_LOGI(TAG, "idle timeout selected: %s (UI only, no logic yet)",
             IDLE_OPTS[idx].label);
    (void)btn;
}

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
    lv_label_set_text(t, "显示与亮度");
    lv_obj_set_style_text_font(t, APP_FONT_TITLE, 0);
    lv_obj_set_style_text_color(t, UI_C_TEXT, 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, UI_SP_MD, 24 + UI_SP_SM);
}

static void create_brightness(lv_obj_t *parent)
{
    /* y 从标题底（24+32=56）+ 8 = 64 起 */
    lv_obj_t *card = make_card(parent, 64, 76);

    /* 当前亮度 → 0~100 */
    int duty = backlight_storage_get();
    int pct = (duty * 100 + 127) / 255;
    if (pct < 1) pct = 1;
    if (pct > 100) pct = 100;

    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%", pct);
    s_ui.bri_value_lbl = make_card_title(card, "屏幕亮度", buf);

    /* slider */
    s_ui.bri_slider = lv_slider_create(card);
    lv_obj_set_size(s_ui.bri_slider, 196, 8);
    lv_obj_align(s_ui.bri_slider, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_slider_set_range(s_ui.bri_slider, 0, 100);
    lv_slider_set_value(s_ui.bri_slider, pct, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_ui.bri_slider, UI_C_BORDER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui.bri_slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_ui.bri_slider, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.bri_slider, UI_C_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_ui.bri_slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_ui.bri_slider, 4, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_ui.bri_slider, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_ui.bri_slider, 5, LV_PART_KNOB);
    lv_obj_set_style_outline_color(s_ui.bri_slider, UI_C_ACCENT, LV_PART_KNOB);
    lv_obj_set_style_outline_width(s_ui.bri_slider, 2, LV_PART_KNOB);
    lv_obj_set_style_outline_opa(s_ui.bri_slider, LV_OPA_COVER, LV_PART_KNOB);

    lv_obj_add_event_cb(s_ui.bri_slider, on_brightness_change, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_ui.bri_slider, on_brightness_released, LV_EVENT_RELEASED, NULL);
}

static void create_idle(lv_obj_t *parent)
{
    /* 卡片 y = 64 + 76 + 8 = 148, 高 76 */
    lv_obj_t *card = make_card(parent, 148, 76);

    s_ui.idle_idx = 2;   /* 默认 1min */
    s_ui.idle_value_lbl = make_card_title(card, "自动息屏",
                                            IDLE_OPTS[s_ui.idle_idx].label);

    /* 5 段 segmented control */
    lv_obj_t *seg = lv_obj_create(card);
    lv_obj_remove_style_all(seg);
    lv_obj_set_size(seg, 196, 28);
    lv_obj_align(seg, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(seg, UI_C_PANEL_HI, 0);
    lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(seg, UI_R_MD, 0);
    lv_obj_set_style_pad_all(seg, 2, 0);
    lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(seg, LV_FLEX_ALIGN_SPACE_BETWEEN,
                           LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(seg, 0, 0);
    lv_obj_clear_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(seg, LV_OBJ_FLAG_EVENT_BUBBLE);

    int btn_w = (196 - 4) / IDLE_OPTS_CNT;
    for (int i = 0; i < IDLE_OPTS_CNT; i++) {
        lv_obj_t *btn = lv_obj_create(seg);
        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, btn_w, 24);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_event_cb(btn, on_idle_opt_clicked, LV_EVENT_CLICKED,
                             (void *)(intptr_t)i);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_obj_set_style_text_font(lbl, APP_FONT_TEXT, 0);
        lv_label_set_text(lbl, IDLE_OPTS[i].label);
        lv_obj_center(lbl);

        s_ui.idle_seg_btns[i] = btn;
    }
    update_idle_seg_visual();
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
    create_brightness(s_ui.screen);
    create_idle(s_ui.screen);

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

static const page_callbacks_t s_cb = {
    .create  = create,
    .destroy = destroy,
    .update  = NULL,
};

const page_callbacks_t *settings_display_get_callbacks(void)
{
    return &s_cb;
}
