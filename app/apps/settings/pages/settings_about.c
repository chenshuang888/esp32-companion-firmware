#include "settings_about.h"
#include "settings_app.h"

#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "lvgl.h"

#include "ui_tokens.h"
#include "ui_widgets.h"
#include "app_shell_ui.h"
#include "app_fonts.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "settings_about";

#define HIT_ZONE_H  30

typedef struct {
    lv_obj_t *screen;
    int       press_y0;
    int       press_y_last;
} ui_t;

static ui_t s_ui;

/* ============================================================================
 * 卡 + KV 行
 * ========================================================================= */

static lv_obj_t *make_card(lv_obj_t *parent, int y)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, 220, LV_SIZE_CONTENT);
    lv_obj_set_pos(c, (240 - 220) / 2, y);
    lv_obj_set_style_bg_color(c, UI_C_PANEL, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(c, UI_C_BORDER, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_opa(c, LV_OPA_50, 0);
    lv_obj_set_style_radius(c, UI_R_LG, 0);
    lv_obj_set_style_pad_all(c, UI_SP_MD, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 0, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(c, LV_OBJ_FLAG_EVENT_BUBBLE);
    return c;
}

static void make_kv(lv_obj_t *card, const char *k, const char *v, bool divider)
{
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 24);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    if (divider) {
        lv_obj_set_style_border_color(row, UI_C_BORDER, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_opa(row, LV_OPA_30, 0);
    }
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t *kl = lv_label_create(row);
    lv_obj_align(kl, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_font(kl, APP_FONT_TEXT, 0);
    lv_obj_set_style_text_color(kl, UI_C_TEXT_MUTED, 0);
    lv_label_set_text(kl, k);

    lv_obj_t *vl = lv_label_create(row);
    lv_obj_align(vl, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_text_font(vl, APP_FONT_TEXT, 0);
    lv_obj_set_style_text_color(vl, UI_C_TEXT, 0);
    lv_label_set_text(vl, v);
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
    lv_label_set_text(t, "关于");
    lv_obj_set_style_text_font(t, APP_FONT_TITLE, 0);
    lv_obj_set_style_text_color(t, UI_C_TEXT, 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, UI_SP_MD, 24 + UI_SP_SM);
}

static void create_hero(lv_obj_t *parent)
{
    /* 紫色圆角 logo 56×56 */
    lv_obj_t *logo = lv_obj_create(parent);
    lv_obj_remove_style_all(logo);
    lv_obj_set_size(logo, 56, 56);
    lv_obj_align(logo, LV_ALIGN_TOP_MID, 0, 64);
    lv_obj_set_style_radius(logo, UI_R_LG, 0);
    lv_obj_set_style_bg_color(logo, UI_C_ACCENT_2, 0);
    lv_obj_set_style_bg_opa(logo, LV_OPA_COVER, 0);
    lv_obj_clear_flag(logo, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(logo, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(logo, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t *icon = lv_label_create(logo);
    lv_obj_set_style_text_font(icon, APP_FONT_ICONS_36, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(icon, ICON_SCHEDULE);
    lv_obj_center(icon);

    /* 名字 */
    lv_obj_t *name = lv_label_create(parent);
    lv_obj_set_style_text_font(name, APP_FONT_TITLE, 0);
    lv_obj_set_style_text_color(name, UI_C_TEXT, 0);
    lv_label_set_text(name, "ESP32 Watch");
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 128);

    /* 版本 */
    const esp_app_desc_t *desc = esp_app_get_description();
    char ver[80];
    if (desc) {
        snprintf(ver, sizeof(ver), "%s · %s",
                 desc->version[0] ? desc->version : "v?",
                 desc->date[0] ? desc->date : "");
    } else {
        snprintf(ver, sizeof(ver), "v0.0");
    }

    lv_obj_t *sub = lv_label_create(parent);
    lv_obj_set_style_text_font(sub, APP_FONT_TEXT, 0);
    lv_obj_set_style_text_color(sub, UI_C_TEXT_MUTED, 0);
    lv_label_set_text(sub, ver);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 152);
}

static void create_info(lv_obj_t *parent)
{
    lv_obj_t *card = make_card(parent, 184);

    /* 芯片型号 */
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    char chip_str[32];
    const char *model = "Unknown";
    switch (chip.model) {
        case CHIP_ESP32:    model = "ESP32";    break;
        case CHIP_ESP32S2:  model = "ESP32-S2"; break;
        case CHIP_ESP32S3:  model = "ESP32-S3"; break;
        case CHIP_ESP32C3:  model = "ESP32-C3"; break;
        case CHIP_ESP32C6:  model = "ESP32-C6"; break;
        case CHIP_ESP32H2:  model = "ESP32-H2"; break;
        default: break;
    }
    snprintf(chip_str, sizeof(chip_str), "%s · %d 核", model, chip.cores);

    /* Flash 大小 */
    uint32_t flash_sz = 0;
    esp_flash_get_size(NULL, &flash_sz);
    char flash_str[16];
    snprintf(flash_str, sizeof(flash_str), "%lu MB",
             (unsigned long)(flash_sz / 1024 / 1024));

    /* PSRAM 大小 */
    size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    char psram_str[16];
    if (psram_total > 0) {
        snprintf(psram_str, sizeof(psram_str), "%u MB",
                 (unsigned)(psram_total / 1024 / 1024));
    } else {
        snprintf(psram_str, sizeof(psram_str), "无");
    }

    /* IDF 版本 */
    char idf_str[24];
    snprintf(idf_str, sizeof(idf_str), "v%s", IDF_VER);

    make_kv(card, "芯片",  chip_str,   true);
    make_kv(card, "框架",  idf_str,    true);
    make_kv(card, "GUI",   "LVGL 9.5", true);
    make_kv(card, "Flash", flash_str,  true);
    make_kv(card, "PSRAM", psram_str,  false);
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
    create_hero(s_ui.screen);
    create_info(s_ui.screen);

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

static const page_callbacks_t s_callbacks = {
    .create  = create,
    .destroy = destroy,
    .update  = NULL,
};

const page_callbacks_t *settings_about_get_callbacks(void)
{
    return &s_callbacks;
}
