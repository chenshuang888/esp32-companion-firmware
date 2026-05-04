#include "page_lockscreen.h"
#include "app_router.h"
#include "esp_log.h"
#include "lvgl.h"
#include "app_fonts.h"
#include <time.h>

/* ============================================================================
 * 配色（深紫 + 青绿）
 * ========================================================================= */

#define COLOR_BG         0x1E1B2E
#define COLOR_ACCENT     0x06B6D4
#define COLOR_TEXT       0xF1ECFF
#define COLOR_MUTED      0x9B94B5

static const char *TAG = "page_time";

/* ============================================================================
 * UI 元素（锁屏首页：只有时间 + 日期两块文字）
 * ========================================================================= */

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *time_label;
    lv_obj_t *date_label;

    int last_min;    /* 分钟级刷新节流：上次渲染时的分钟数，-1 表示首次 */
} page_time_ui_t;

static page_time_ui_t s_ui = { .last_min = -1 };

static const char *WEEKDAY_CN[] = {"日", "一", "二", "三", "四", "五", "六"};

/* ============================================================================
 * 布局
 * ========================================================================= */

static void create_lockscreen(void)
{
    /* 大号时间 HH:MM：屏幕中央偏上 */
    s_ui.time_label = lv_label_create(s_ui.screen);
    lv_label_set_text(s_ui.time_label, "--:--");
    lv_obj_set_style_text_color(s_ui.time_label, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_text_font(s_ui.time_label, APP_FONT_HUGE, 0);
    lv_obj_align(s_ui.time_label, LV_ALIGN_CENTER, 0, -20);

    /* 日期 + 中文星期：时间正下方 */
    s_ui.date_label = lv_label_create(s_ui.screen);
    lv_label_set_text(s_ui.date_label, "--");
    lv_obj_set_style_text_color(s_ui.date_label, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_font(s_ui.date_label, APP_FONT_TITLE, 0);
    lv_obj_align(s_ui.date_label, LV_ALIGN_CENTER, 0, 30);

    /* 底部"上滑"视觉提示：一条横线 + 小字 */
    lv_obj_t *hint_bar = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(hint_bar);
    lv_obj_set_size(hint_bar, 60, 3);
    lv_obj_set_style_bg_color(hint_bar, lv_color_hex(COLOR_MUTED), 0);
    lv_obj_set_style_bg_opa(hint_bar, LV_OPA_60, 0);
    lv_obj_set_style_radius(hint_bar, 2, 0);
    lv_obj_align(hint_bar, LV_ALIGN_BOTTOM_MID, 0, -24);

    lv_obj_t *hint_lbl = lv_label_create(s_ui.screen);
    lv_label_set_text(hint_lbl, "上滑");
    lv_obj_set_style_text_color(hint_lbl, lv_color_hex(COLOR_MUTED), 0);
    lv_obj_set_style_text_font(hint_lbl, APP_FONT_TEXT, 0);
    lv_obj_align(hint_lbl, LV_ALIGN_BOTTOM_MID, 0, -6);
}

/* ============================================================================
 * 事件：全屏上滑 → 菜单
 * ========================================================================= */

static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;

    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_TOP) {
        ESP_LOGI(TAG, "Gesture TOP -> launcher");
        app_router_exit_to_launcher();
    }
}

/* ============================================================================
 * 显示刷新（分钟级节流：只有秒从 0 开始重新累计时才真正更新文本）
 * ========================================================================= */

static void update_display(void)
{
    if (!s_ui.time_label || !s_ui.date_label) return;

    time_t now;
    struct tm t;
    time(&now);
    localtime_r(&now, &t);

    if (t.tm_min == s_ui.last_min) {
        return;   /* 同一分钟内不重绘，避免 FPS 级 snprintf 和 LVGL 脏区刷屏 */
    }
    s_ui.last_min = t.tm_min;

    char time_buf[8];
    snprintf(time_buf, sizeof(time_buf), "%02d:%02d", t.tm_hour, t.tm_min);
    lv_label_set_text(s_ui.time_label, time_buf);

    char date_buf[32];
    snprintf(date_buf, sizeof(date_buf), "%d月%d日 星期%s",
             t.tm_mon + 1, t.tm_mday, WEEKDAY_CN[t.tm_wday % 7]);
    lv_label_set_text(s_ui.date_label, date_buf);
}

/* ============================================================================
 * 页面生命周期
 * ========================================================================= */

static lv_obj_t *page_lockscreen_create(void)
{
    ESP_LOGI(TAG, "Creating lockscreen");

    s_ui.screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_ui.screen, lv_color_hex(COLOR_BG), 0);
    lv_obj_clear_flag(s_ui.screen, LV_OBJ_FLAG_SCROLLABLE);

    s_ui.last_min = -1;
    create_lockscreen();

    lv_obj_add_event_cb(s_ui.screen, on_gesture, LV_EVENT_GESTURE, NULL);

    update_display();
    return s_ui.screen;
}

static void page_lockscreen_destroy(void)
{
    ESP_LOGI(TAG, "Destroying lockscreen");

    if (s_ui.screen) {
        lv_obj_del(s_ui.screen);
        s_ui.screen = NULL;
    }

    s_ui.time_label = NULL;
    s_ui.date_label = NULL;
}

static void page_lockscreen_update(void)
{
    update_display();
}

static const page_callbacks_t s_callbacks = {
    .create  = page_lockscreen_create,
    .destroy = page_lockscreen_destroy,
    .update  = page_lockscreen_update,
};

const page_callbacks_t *page_lockscreen_get_callbacks(void)
{
    return &s_callbacks;
}
