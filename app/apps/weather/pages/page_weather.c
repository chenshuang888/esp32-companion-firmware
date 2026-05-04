#include "page_weather.h"
#include "app_router.h"
#include "weather_manager.h"
#include "weather_service.h"
#include "weather_icons.h"

#include "esp_log.h"
#include "lvgl.h"
#include "ui_tokens.h"
#include "ui_widgets.h"
#include "ui_anim.h"
#include "app_shell_ui.h"
#include <stdio.h>
#include <time.h>

static const char *TAG = "page_weather";

/* ============================================================================
 * UI 元素（对应 ui_mockups/weather/v5.html）
 *
 *   y=12  city_lbl       顶部居中弱化城市名
 *   y=40  hero (横排)    左 80×80 图标 + 右温度+状态
 *           - icon_img   (40×40 binary, scale=512 显示为 80×80)
 *           - temp_lbl   (48px CJK)
 *           - status_lbl (16px 状态色)
 *   y=160 minmax_card    220×50 卡，最低/最高
 *   y=220 info_card      220×80 卡，湿度/更新
 * ========================================================================= */

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *statusbar;
    lv_obj_t *city_lbl;
    lv_obj_t *hero_box;
    lv_obj_t *icon_img;
    lv_obj_t *temp_lbl;
    lv_obj_t *status_lbl;
    lv_obj_t *minmax_card;
    lv_obj_t *info_card;
    lv_obj_t *min_lbl;
    lv_obj_t *max_lbl;
    lv_obj_t *humidity_lbl;
    lv_obj_t *updated_lbl;

    int      cur_temp_x10;
    uint32_t last_updated_at;
} page_weather_ui_t;

static page_weather_ui_t s_ui = {0};

/* ============================================================================
 * weather_code → 状态色（浅色主题专用，跟 ui_mockups/_shared/tokens.css 一致）
 * ========================================================================= */

static lv_color_t code_color(uint8_t c)
{
    switch (c) {
    case WEATHER_CODE_CLEAR:    return lv_color_hex(0xF59E0B);
    case WEATHER_CODE_CLOUDY:   return lv_color_hex(0x64748B);
    case WEATHER_CODE_OVERCAST: return lv_color_hex(0x475569);
    case WEATHER_CODE_RAIN:     return lv_color_hex(0x2563EB);
    case WEATHER_CODE_SNOW:     return lv_color_hex(0x0891B2);
    case WEATHER_CODE_FOG:      return lv_color_hex(0x7C3AED);
    case WEATHER_CODE_THUNDER:  return lv_color_hex(0xEA580C);
    default:                    return UI_C_TEXT_MUTED;
    }
}

/* ============================================================================
 * 「底缘上滑」退出手势（同 v 之前实现，未变）
 * ========================================================================= */

#define EXIT_EDGE_HEIGHT  50

static int s_press_y0 = -1;

static void on_pressed(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev) { s_press_y0 = -1; return; }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    s_press_y0 = p.y;
}

static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir != LV_DIR_TOP) return;

    int32_t screen_h = lv_obj_get_height(s_ui.screen);
    if (s_press_y0 >= 0 && s_press_y0 >= screen_h - EXIT_EDGE_HEIGHT) {
        app_router_exit_to_launcher();
    }
    s_press_y0 = -1;
}

/* ============================================================================
 * 布局
 * ========================================================================= */

static void create_top_label(void)
{
    /* 城市名 —— 顶部居中弱化 */
    s_ui.city_lbl = lv_label_create(s_ui.screen);
    lv_label_set_text(s_ui.city_lbl, "--");
    lv_obj_set_style_text_color(s_ui.city_lbl, UI_C_TEXT_MUTED, 0);
    lv_obj_set_style_text_font (s_ui.city_lbl, UI_F_BODY, 0);
    /* y=28: statusbar(24) + 4 间距 */
    lv_obj_align(s_ui.city_lbl, LV_ALIGN_TOP_MID, 0, 28);
}

static void create_hero(void)
{
    /* hero_box: 透明容器，240 宽，水平 flex 横排（左图右数） */
    s_ui.hero_box = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(s_ui.hero_box);
    lv_obj_set_size(s_ui.hero_box, 240, 86);
    lv_obj_align(s_ui.hero_box, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_clear_flag(s_ui.hero_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow (s_ui.hero_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_ui.hero_box,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_ui.hero_box, UI_SP_LG, 0);

    /* 左：图标 image —— 原图 40×40，scale 512 显示为 80×80 */
    s_ui.icon_img = lv_image_create(s_ui.hero_box);
    lv_image_set_src(s_ui.icon_img, weather_icon_for(WEATHER_CODE_UNKNOWN));
    lv_image_set_scale(s_ui.icon_img, 512);
    lv_obj_set_size(s_ui.icon_img, 80, 80);

    /* 右：纵向 box（温度 + 状态） */
    lv_obj_t *info = lv_obj_create(s_ui.hero_box);
    lv_obj_remove_style_all(info);
    lv_obj_set_size(info, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_clear_flag(info, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(info, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(info, UI_SP_XS, 0);

    s_ui.temp_lbl = lv_label_create(info);
    lv_label_set_text(s_ui.temp_lbl, "--");
    lv_obj_set_style_text_color(s_ui.temp_lbl, UI_C_TEXT, 0);
    lv_obj_set_style_text_font (s_ui.temp_lbl, UI_F_HUGE, 0);

    s_ui.status_lbl = lv_label_create(info);
    lv_label_set_text(s_ui.status_lbl, "Waiting");
    lv_obj_set_style_text_color(s_ui.status_lbl, UI_C_TEXT_DIM, 0);
    lv_obj_set_style_text_font (s_ui.status_lbl, UI_F_TITLE, 0);
}

static void create_minmax_card(void)
{
    s_ui.minmax_card = ui_card(s_ui.screen);
    /* ui_card 自带 12px padding，这里覆盖为 0 让内部 flex 自由分布 */
    lv_obj_set_style_pad_all(s_ui.minmax_card, 0, 0);
    lv_obj_set_size(s_ui.minmax_card, 220, 50);
    lv_obj_align(s_ui.minmax_card, LV_ALIGN_TOP_MID, 0, 148);

    lv_obj_set_flex_flow(s_ui.minmax_card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_ui.minmax_card,
        LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* 左：最低 */
    lv_obj_t *low_box = lv_obj_create(s_ui.minmax_card);
    lv_obj_remove_style_all(low_box);
    lv_obj_set_size(low_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_clear_flag(low_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(low_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(low_box, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *low_hint = lv_label_create(low_box);
    lv_label_set_text(low_hint, "最低");
    lv_obj_set_style_text_color(low_hint, UI_C_TEXT_MUTED, 0);
    lv_obj_set_style_text_font (low_hint, UI_F_BODY, 0);

    s_ui.min_lbl = lv_label_create(low_box);
    lv_label_set_text(s_ui.min_lbl, "--");
    lv_obj_set_style_text_color(s_ui.min_lbl, UI_C_TEXT, 0);
    lv_obj_set_style_text_font (s_ui.min_lbl, UI_F_TITLE, 0);

    /* 中：分隔条 */
    lv_obj_t *sep = lv_obj_create(s_ui.minmax_card);
    lv_obj_remove_style_all(sep);
    lv_obj_set_size(sep, 1, 28);
    lv_obj_set_style_bg_color(sep, UI_C_BORDER, 0);
    lv_obj_set_style_bg_opa  (sep, LV_OPA_70, 0);

    /* 右：最高 */
    lv_obj_t *high_box = lv_obj_create(s_ui.minmax_card);
    lv_obj_remove_style_all(high_box);
    lv_obj_set_size(high_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_clear_flag(high_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(high_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(high_box, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *high_hint = lv_label_create(high_box);
    lv_label_set_text(high_hint, "最高");
    lv_obj_set_style_text_color(high_hint, UI_C_TEXT_MUTED, 0);
    lv_obj_set_style_text_font (high_hint, UI_F_BODY, 0);

    s_ui.max_lbl = lv_label_create(high_box);
    lv_label_set_text(s_ui.max_lbl, "--");
    lv_obj_set_style_text_color(s_ui.max_lbl, UI_C_TEXT, 0);
    lv_obj_set_style_text_font (s_ui.max_lbl, UI_F_TITLE, 0);
}

static void create_info_card(void)
{
    s_ui.info_card = ui_card(s_ui.screen);
    lv_obj_set_size(s_ui.info_card, 220, 80);
    lv_obj_align(s_ui.info_card, LV_ALIGN_TOP_MID, 0, 212);
    lv_obj_set_flex_flow(s_ui.info_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_ui.info_card, 0, 0);

    ui_kv_row(s_ui.info_card, "湿度", "--%",   &s_ui.humidity_lbl, true);
    ui_kv_row(s_ui.info_card, "更新", "--:--", &s_ui.updated_lbl,  false);
}

/* ============================================================================
 * 显示刷新
 * ========================================================================= */

static void update_display(bool animate)
{
    const weather_payload_t *w = weather_manager_get_latest();
    if (!w) return;
    if (w->updated_at == s_ui.last_updated_at) return;
    s_ui.last_updated_at = w->updated_at;

    /* 城市 */
    lv_label_set_text(s_ui.city_lbl, w->city);

    /* 图标 + 状态色 */
    lv_image_set_src(s_ui.icon_img, weather_icon_for(w->weather_code));
    lv_label_set_text(s_ui.status_lbl, w->description);
    lv_obj_set_style_text_color(s_ui.status_lbl, code_color(w->weather_code), 0);

    /* 温度 */
    char buf[16];
    if (animate) {
        ui_anim_number_rolling(s_ui.temp_lbl,
                                s_ui.cur_temp_x10, w->temp_c_x10,
                                UI_DUR_SLOW, 1, "°");
    } else {
        int t = w->temp_c_x10;
        int sign = t < 0 ? -1 : 1;
        int at = t * sign;
        snprintf(buf, sizeof(buf), "%s%d.%d°",
                 sign < 0 ? "-" : "", at / 10, at % 10);
        lv_label_set_text(s_ui.temp_lbl, buf);
    }
    s_ui.cur_temp_x10 = w->temp_c_x10;

    /* min/max */
    snprintf(buf, sizeof(buf), "%d°", w->temp_min_x10 / 10);
    lv_label_set_text(s_ui.min_lbl, buf);
    snprintf(buf, sizeof(buf), "%d°", w->temp_max_x10 / 10);
    lv_label_set_text(s_ui.max_lbl, buf);

    /* humidity */
    snprintf(buf, sizeof(buf), "%d%%", w->humidity);
    lv_label_set_text(s_ui.humidity_lbl, buf);

    /* updated time */
    time_t ts = (time_t)w->updated_at;
    struct tm tm;
    localtime_r(&ts, &tm);
    snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    lv_label_set_text(s_ui.updated_lbl, buf);
}

/* ============================================================================
 * 入场动画
 * ========================================================================= */

static void play_intro(void)
{
    ui_anim_fade_in(s_ui.city_lbl,    0);
    ui_anim_fade_in(s_ui.hero_box,    60);
    ui_anim_fade_in(s_ui.minmax_card, 160);
    ui_anim_fade_in(s_ui.info_card,   240);
}

/* ============================================================================
 * 页面生命周期
 * ========================================================================= */

static lv_obj_t *page_weather_create(void)
{
    ESP_LOGI(TAG, "Creating weather page");

    s_ui.screen = lv_obj_create(NULL);
    ui_screen_setup(s_ui.screen);

    lv_obj_add_event_cb(s_ui.screen, on_pressed, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_ui.screen, on_gesture, LV_EVENT_GESTURE, NULL);

    create_top_label();
    create_hero();
    create_minmax_card();
    create_info_card();

    /* statusbar 最后挂，确保在最顶层 */
    s_ui.statusbar = app_shell_attach_statusbar(s_ui.screen, false);

    s_ui.last_updated_at = 0;
    s_ui.cur_temp_x10 = 0;
    update_display(false);
    play_intro();

    weather_service_send_request();

    return s_ui.screen;
}

static void page_weather_destroy(void)
{
    ESP_LOGI(TAG, "Destroying weather page");

    if (s_ui.screen) {
        lv_obj_del(s_ui.screen);
        s_ui.screen = NULL;
    }
    s_press_y0 = -1;

    s_ui.statusbar    = NULL;
    s_ui.city_lbl     = NULL;
    s_ui.hero_box     = NULL;
    s_ui.icon_img     = NULL;
    s_ui.temp_lbl     = NULL;
    s_ui.status_lbl   = NULL;
    s_ui.minmax_card  = NULL;
    s_ui.info_card    = NULL;
    s_ui.min_lbl      = NULL;
    s_ui.max_lbl      = NULL;
    s_ui.humidity_lbl = NULL;
    s_ui.updated_lbl  = NULL;
}

static void page_weather_update(void)
{
    update_display(true);
}

static const page_callbacks_t s_callbacks = {
    .create  = page_weather_create,
    .destroy = page_weather_destroy,
    .update  = page_weather_update,
};

const page_callbacks_t *page_weather_get_callbacks(void)
{
    return &s_callbacks;
}
