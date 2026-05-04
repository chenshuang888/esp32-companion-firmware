#include "settings_bluetooth.h"
#include "settings_app.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "lvgl.h"

#include "ui_tokens.h"
#include "ui_widgets.h"
#include "app_shell_ui.h"
#include "app_fonts.h"

#include "ble_driver.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "settings_bt";

#define BLE_DEVICE_NAME "ESP32-S3-DEMO"   /* 与 ble_driver.c 保持一致 */
#define HIT_ZONE_H      30

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *hero_icon;
    lv_obj_t *hero_status;
    lv_obj_t *hero_sub;
    bool      last_connected;
    int       press_y0;
    int       press_y_last;
} ui_t;

static ui_t s_ui;

/* ============================================================================
 * 信息卡 + KV
 * ========================================================================= */

static lv_obj_t *make_info_card(lv_obj_t *parent, int y, int w)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, w, LV_SIZE_CONTENT);
    lv_obj_set_pos(c, (240 - w) / 2, y);
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
 * 事件
 * ========================================================================= */

static void on_disconnect_clicked(lv_event_t *e)
{
    (void)e;
    /* TODO: 实际断开逻辑暂未接（ble_driver 没暴露 disconnect API）*/
    ESP_LOGI(TAG, "disconnect button pressed (no-op)");
}

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

static void apply_status(bool connected)
{
    lv_obj_set_style_bg_color(s_ui.hero_icon,
        connected ? UI_C_ACCENT : UI_C_TEXT_MUTED, 0);
    lv_label_set_text(s_ui.hero_status, connected ? "已连接" : "未连接");
    lv_obj_set_style_text_color(s_ui.hero_status,
        connected ? lv_color_hex(0x34C759) : UI_C_TEXT_MUTED, 0);
    lv_label_set_text(s_ui.hero_sub,
        connected ? "通过 BLE 与电脑通信" : "等待电脑连接");
}

static void create_title(lv_obj_t *parent)
{
    lv_obj_t *t = lv_label_create(parent);
    lv_label_set_text(t, "蓝牙");
    lv_obj_set_style_text_font(t, APP_FONT_TITLE, 0);
    lv_obj_set_style_text_color(t, UI_C_TEXT, 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, UI_SP_MD, 24 + UI_SP_SM);
}

static void create_hero(lv_obj_t *parent)
{
    /* 56px 圆形图标 */
    s_ui.hero_icon = lv_obj_create(parent);
    lv_obj_remove_style_all(s_ui.hero_icon);
    lv_obj_set_size(s_ui.hero_icon, 56, 56);
    lv_obj_align(s_ui.hero_icon, LV_ALIGN_TOP_MID, 0, 64);
    lv_obj_set_style_radius(s_ui.hero_icon, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_ui.hero_icon, UI_C_ACCENT, 0);
    lv_obj_set_style_bg_opa(s_ui.hero_icon, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_ui.hero_icon, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_ui.hero_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_ui.hero_icon, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t *icon = lv_label_create(s_ui.hero_icon);
    lv_obj_set_style_text_font(icon, APP_FONT_ICONS_36, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(icon, ICON_BLUETOOTH);
    lv_obj_center(icon);

    s_ui.hero_status = lv_label_create(parent);
    lv_obj_set_style_text_font(s_ui.hero_status, APP_FONT_TITLE, 0);
    lv_label_set_text(s_ui.hero_status, "已连接");
    lv_obj_align(s_ui.hero_status, LV_ALIGN_TOP_MID, 0, 128);

    s_ui.hero_sub = lv_label_create(parent);
    lv_obj_set_style_text_font(s_ui.hero_sub, APP_FONT_TEXT, 0);
    lv_obj_set_style_text_color(s_ui.hero_sub, UI_C_TEXT_MUTED, 0);
    lv_label_set_text(s_ui.hero_sub, "通过 BLE 与电脑通信");
    lv_obj_align(s_ui.hero_sub, LV_ALIGN_TOP_MID, 0, 150);
}

static void create_info(lv_obj_t *parent)
{
    lv_obj_t *card = make_info_card(parent, 178, 220);

    /* 设备名 */
    make_kv(card, "设备名", BLE_DEVICE_NAME, true);

    /* MAC 地址（本机蓝牙）*/
    uint8_t mac[6] = {0};
    char mac_str[20] = "--";
    if (esp_read_mac(mac, ESP_MAC_BT) == ESP_OK) {
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    make_kv(card, "MAC", mac_str, false);
}

static void create_disconnect_btn(lv_obj_t *parent)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 220, 44);
    lv_obj_set_pos(card, (240 - 220) / 2, 232);
    lv_obj_set_style_bg_color(card, UI_C_PANEL, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, UI_C_BORDER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_opa(card, LV_OPA_50, 0);
    lv_obj_set_style_radius(card, UI_R_LG, 0);
    lv_obj_set_style_pad_left(card, UI_SP_MD, 0);
    lv_obj_set_style_pad_right(card, UI_SP_MD, 0);
    lv_obj_set_style_bg_color(card, UI_C_PANEL_HI, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(card, on_disconnect_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(card);
    lv_obj_center(lbl);
    lv_obj_set_style_text_font(lbl, APP_FONT_TEXT, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFF3B30), 0);
    lv_label_set_text(lbl, "断开连接");
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
    create_disconnect_btn(s_ui.screen);

    s_ui.last_connected = ble_driver_is_connected();
    apply_status(s_ui.last_connected);

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

static void update(void)
{
    bool now = ble_driver_is_connected();
    if (now != s_ui.last_connected) {
        s_ui.last_connected = now;
        apply_status(now);
    }
}

static const page_callbacks_t s_cb = {
    .create  = create,
    .destroy = destroy,
    .update  = update,
};

const page_callbacks_t *settings_bluetooth_get_callbacks(void)
{
    return &s_cb;
}
