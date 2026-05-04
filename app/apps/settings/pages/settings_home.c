#include "settings_home.h"
#include "settings_app.h"

#include "esp_log.h"
#include "lvgl.h"

#include "ui_tokens.h"
#include "ui_widgets.h"
#include "app_shell_ui.h"
#include "app_fonts.h"

#include "ble_driver.h"

#include <string.h>

static const char *TAG = "settings_home";

/* ============================================================================
 * 设置首页（仿 iOS 设置 app）
 *
 * 布局（240×320）：
 *   y=0~24    statusbar
 *   y=24~52   "设置" 标题（28px）
 *   y=52~290  分组列表（可滚）
 *               group 1: 蓝牙（独立卡）
 *               group 2: 显示与亮度 / 时间调节 / 关于
 *   y=290~320 hit zone（30px 上滑退出）
 *
 * 行设计（44px 高）：
 *   左 28×28 彩色圆角方块 + Material 图标（白色字）
 *   中 label
 *   右 value（可选）+ chevron ›
 * ========================================================================= */

#define HIT_ZONE_H   30
#define ROW_H        44
#define ICON_BOX     28

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *bt_value_lbl;     /* 蓝牙状态行的右值，update 时刷新 */
    lv_obj_t *hit_zone;
    int       press_y0;
    int       press_y_last;
    bool      last_ble;
} ui_t;

static ui_t s_ui;

/* ============================================================================
 * 行工厂：彩色图标方块 + label + value + chevron
 *
 * row 自带 CLICKABLE，调用方 add_event_cb 即可
 * ========================================================================= */

typedef struct {
    lv_obj_t *row;
    lv_obj_t *value;
} row_handle_t;

static row_handle_t make_row(lv_obj_t *parent,
                              const char *icon_utf8,
                              uint32_t icon_bg,
                              const char *label,
                              const char *value)
{
    row_handle_t h = {0};
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), ROW_H);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_left(row, UI_SP_MD, 0);
    lv_obj_set_style_pad_right(row, UI_SP_MD, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    /* 让上滑事件能冒泡到 screen */
    lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_style_bg_color(row, UI_C_PANEL_HI, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);

    /* 图标方块 */
    lv_obj_t *box = lv_obj_create(row);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, ICON_BOX, ICON_BOX);
    lv_obj_align(box, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(icon_bg), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, 7, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(box, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t *icon = lv_label_create(box);
    lv_obj_set_style_text_font(icon, APP_FONT_ICONS_24, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(icon, icon_utf8);
    lv_obj_center(icon);

    /* label */
    lv_obj_t *lbl = lv_label_create(row);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, ICON_BOX + UI_SP_SM, 0);
    lv_obj_set_style_text_font(lbl, APP_FONT_TEXT, 0);
    lv_obj_set_style_text_color(lbl, UI_C_TEXT, 0);
    lv_label_set_text(lbl, label);

    /* chevron 在最右 */
    lv_obj_t *chev = lv_label_create(row);
    lv_obj_align(chev, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_text_font(chev, APP_FONT_TEXT, 0);
    lv_obj_set_style_text_color(chev, UI_C_TEXT_MUTED, 0);
    lv_label_set_text(chev, ICON_CHEVRON_RIGHT);

    /* value 在 chevron 左侧 */
    lv_obj_t *val = lv_label_create(row);
    lv_obj_align(val, LV_ALIGN_RIGHT_MID, -16, 0);
    lv_obj_set_style_text_font(val, APP_FONT_TEXT, 0);
    lv_obj_set_style_text_color(val, UI_C_TEXT_MUTED, 0);
    lv_label_set_text(val, value ? value : "");

    h.row = row;
    h.value = val;
    return h;
}

/* 给 group 加一个底部分隔线（除最后一行外）*/
static void add_row_divider(lv_obj_t *row)
{
    lv_obj_t *line = lv_obj_create(row);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, lv_pct(100) - UI_SP_MD, 1);
    lv_obj_align(line, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(line, UI_C_BORDER, 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_40, 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(line, LV_OBJ_FLAG_EVENT_BUBBLE);
}

static lv_obj_t *make_group(lv_obj_t *parent, int width)
{
    lv_obj_t *g = lv_obj_create(parent);
    lv_obj_remove_style_all(g);
    lv_obj_set_width(g, width);
    lv_obj_set_height(g, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(g, UI_C_PANEL, 0);
    lv_obj_set_style_bg_opa(g, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(g, UI_C_BORDER, 0);
    lv_obj_set_style_border_width(g, 1, 0);
    lv_obj_set_style_border_opa(g, LV_OPA_50, 0);
    lv_obj_set_style_radius(g, UI_R_LG, 0);
    lv_obj_set_style_clip_corner(g, true, 0);
    lv_obj_set_style_pad_all(g, 0, 0);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(g, 0, 0);
    lv_obj_clear_flag(g, LV_OBJ_FLAG_SCROLLABLE);
    return g;
}

/* ============================================================================
 * 事件
 * ========================================================================= */

static void on_bt_clicked(lv_event_t *e)    { (void)e; settings_app_push(SETTINGS_PAGE_BLUETOOTH); }
static void on_disp_clicked(lv_event_t *e)  { (void)e; settings_app_push(SETTINGS_PAGE_DISPLAY); }
static void on_time_clicked(lv_event_t *e)  { (void)e; settings_app_push(SETTINGS_PAGE_TIME); }
static void on_about_clicked(lv_event_t *e) { (void)e; settings_app_push(SETTINGS_PAGE_ABOUT); }

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
    if (dy >= 30) {
        settings_app_pop_or_exit();
    }
}

/* ============================================================================
 * 视图
 * ========================================================================= */

static void create_title(lv_obj_t *parent)
{
    lv_obj_t *t = lv_label_create(parent);
    lv_label_set_text(t, "设置");
    lv_obj_set_style_text_font(t, APP_FONT_TITLE, 0);
    lv_obj_set_style_text_color(t, UI_C_TEXT, 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, UI_SP_MD, 24 + UI_SP_SM);
}

static void create_groups(lv_obj_t *parent)
{
    /* 容器：可滚动 */
    lv_obj_t *body = lv_obj_create(parent);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, 240, 320 - 24 - 32 - HIT_ZONE_H);
    lv_obj_set_pos(body, 0, 24 + 32);
    lv_obj_set_style_pad_left(body, UI_SP_MD, 0);
    lv_obj_set_style_pad_right(body, UI_SP_MD, 0);
    lv_obj_set_style_pad_top(body, 0, 0);
    lv_obj_set_style_pad_bottom(body, UI_SP_SM, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, UI_SP_MD, 0);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);

    int group_w = 240 - 2 * UI_SP_MD;

    /* 蓝牙独立组 */
    lv_obj_t *g1 = make_group(body, group_w);
    bool connected = ble_driver_is_connected();
    row_handle_t bt = make_row(g1, ICON_BLUETOOTH, 0x007AFF,
                                "蓝牙", connected ? "已连接" : "未连接");
    lv_obj_set_style_text_color(bt.value,
        connected ? lv_color_hex(0x34C759) : UI_C_TEXT_MUTED, 0);
    lv_obj_add_event_cb(bt.row, on_bt_clicked, LV_EVENT_CLICKED, NULL);
    s_ui.bt_value_lbl = bt.value;
    s_ui.last_ble = connected;

    /* 通用组 */
    lv_obj_t *g2 = make_group(body, group_w);

    row_handle_t disp = make_row(g2, ICON_BRIGHTNESS, 0xFF9500,
                                  "显示与亮度", NULL);
    add_row_divider(disp.row);
    lv_obj_add_event_cb(disp.row, on_disp_clicked, LV_EVENT_CLICKED, NULL);

    row_handle_t tm = make_row(g2, ICON_EDIT_CALENDAR, 0x34C759,
                                "时间调节", NULL);
    add_row_divider(tm.row);
    lv_obj_add_event_cb(tm.row, on_time_clicked, LV_EVENT_CLICKED, NULL);

    row_handle_t about = make_row(g2, ICON_INFO, 0x6E6E73, "关于", NULL);
    lv_obj_add_event_cb(about.row, on_about_clicked, LV_EVENT_CLICKED, NULL);
}

/* ============================================================================
 * 生命周期
 * ========================================================================= */

static lv_obj_t *settings_home_create(void)
{
    ESP_LOGI(TAG, "create");
    memset(&s_ui, 0, sizeof(s_ui));
    s_ui.press_y0 = -1;

    s_ui.screen = lv_obj_create(NULL);
    ui_screen_setup(s_ui.screen);
    lv_obj_clear_flag(s_ui.screen, LV_OBJ_FLAG_SCROLLABLE);

    app_shell_attach_statusbar(s_ui.screen, false);
    create_title(s_ui.screen);
    create_groups(s_ui.screen);

    /* 底部 hit zone（30px 上滑退出）*/
    s_ui.hit_zone = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(s_ui.hit_zone);
    lv_obj_set_size(s_ui.hit_zone, 240, HIT_ZONE_H);
    lv_obj_align(s_ui.hit_zone, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(s_ui.hit_zone, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_ui.hit_zone, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_ui.hit_zone, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_ui.hit_zone, on_hit_pressed,  LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(s_ui.hit_zone, on_hit_pressing, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_ui.hit_zone, on_hit_released, LV_EVENT_RELEASED, NULL);

    return s_ui.screen;
}

static void settings_home_destroy(void)
{
    ESP_LOGI(TAG, "destroy");
    if (s_ui.screen) {
        lv_obj_del(s_ui.screen);
    }
    memset(&s_ui, 0, sizeof(s_ui));
    s_ui.press_y0 = -1;
}

static void settings_home_update(void)
{
    /* BLE 连接状态变化时刷新右值 */
    if (!s_ui.bt_value_lbl) return;
    bool now = ble_driver_is_connected();
    if (now != s_ui.last_ble) {
        s_ui.last_ble = now;
        lv_label_set_text(s_ui.bt_value_lbl, now ? "已连接" : "未连接");
        lv_obj_set_style_text_color(s_ui.bt_value_lbl,
            now ? lv_color_hex(0x34C759) : UI_C_TEXT_MUTED, 0);
    }
}

static const page_callbacks_t s_callbacks = {
    .create  = settings_home_create,
    .destroy = settings_home_destroy,
    .update  = settings_home_update,
};

const page_callbacks_t *settings_home_get_callbacks(void)
{
    return &s_callbacks;
}
