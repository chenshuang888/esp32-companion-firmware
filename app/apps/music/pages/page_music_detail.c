#include "page_music_detail.h"
#include "music_app.h"
#include "app_router.h"
#include "app_shell_ui.h"

#include "media_manager.h"
#include "media_service.h"
#include "ble_driver.h"

#include "esp_log.h"
#include "lvgl.h"
#include "app_fonts.h"
#include "ui_tokens.h"
#include "ui_widgets.h"
#include "ui_anim.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "page_music_detail";

/* ============================================================================
 * 布局规划（240×320）
 *
 *   y=0~24    statusbar
 *   y=24~190  圆盘（160×160 居中，align top y=26）
 *   y=192~210 标题
 *   y=212~226 作者
 *   y=234~238 进度条 + y=246~258 时间
 *   y=255~299 三个按钮（44/56/44，整体居中 cy≈277）
 *   y=290~320 底部 hit zone（30px，纯上滑退出区，不放任何点击元素）
 *
 * 设计原则：屏幕最底 30px 留给 home-indicator 风格的上滑手势区，避免误触
 * ========================================================================= */

#define DISC_SIZE   160
#define HIT_ZONE_H  30

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *statusbar;

    lv_obj_t *disc;
    lv_obj_t *disc_ring;     /* 内圈中性灰环 */
    lv_obj_t *disc_hole;     /* 中心钉孔 */

    lv_obj_t *title_lbl;
    lv_obj_t *artist_lbl;

    lv_obj_t *bar_track;
    lv_obj_t *bar_fill;
    lv_obj_t *bar_knob;
    lv_obj_t *time_lbl;

    lv_obj_t *prev_btn;
    lv_obj_t *pp_btn;
    lv_obj_t *pp_icon;
    lv_obj_t *next_btn;

    uint32_t  last_media_ver;
    int16_t   last_pos_sec;
    bool      last_playing;
    int       disc_angle;       /* 0..3599 (0.1° per unit) */

    int       press_y0;
    int       press_y_last;
} ui_t;

static ui_t s_ui;

static void on_pp_clicked(lv_event_t *e);
static void on_prev_clicked(lv_event_t *e);
static void on_next_clicked(lv_event_t *e);
static void on_hit_pressed(lv_event_t *e);
static void on_hit_pressing(lv_event_t *e);
static void on_hit_released(lv_event_t *e);
static void disc_anim_cb(void *var, int32_t v);

/* ============================================================================
 * 组件
 * ========================================================================= */

static void create_disc(void)
{
    /* 圆盘外层：紫色填充 + 圆角 = 圆 */
    s_ui.disc = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(s_ui.disc);
    lv_obj_set_size(s_ui.disc, DISC_SIZE, DISC_SIZE);
    lv_obj_align(s_ui.disc, LV_ALIGN_TOP_MID, 0, 26);
    lv_obj_set_style_radius(s_ui.disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_ui.disc, UI_C_ACCENT_2, 0);
    lv_obj_set_style_bg_opa(s_ui.disc, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_ui.disc, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_ui.disc, LV_OBJ_FLAG_CLICKABLE);
    /* 黑胶纹理：粗描边一圈 */
    lv_obj_set_style_border_width(s_ui.disc, 4, 0);
    lv_obj_set_style_border_color(s_ui.disc, UI_C_ACCENT, 0);
    lv_obj_set_style_border_opa(s_ui.disc, LV_OPA_60, 0);

    /* 内圈灰色环（视觉层次） */
    s_ui.disc_ring = lv_obj_create(s_ui.disc);
    lv_obj_remove_style_all(s_ui.disc_ring);
    lv_obj_set_size(s_ui.disc_ring, 90, 90);
    lv_obj_center(s_ui.disc_ring);
    lv_obj_set_style_radius(s_ui.disc_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_ui.disc_ring, lv_color_hex(0x2A2A30), 0);
    lv_obj_set_style_bg_opa(s_ui.disc_ring, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_ui.disc_ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_ui.disc_ring, LV_OBJ_FLAG_CLICKABLE);

    /* 中心钉孔 */
    s_ui.disc_hole = lv_obj_create(s_ui.disc);
    lv_obj_remove_style_all(s_ui.disc_hole);
    lv_obj_set_size(s_ui.disc_hole, 14, 14);
    lv_obj_center(s_ui.disc_hole);
    lv_obj_set_style_radius(s_ui.disc_hole, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_ui.disc_hole, UI_C_BG, 0);
    lv_obj_set_style_bg_opa(s_ui.disc_hole, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_ui.disc_hole, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_ui.disc_hole, LV_OBJ_FLAG_CLICKABLE);

    /* 旋转中心 = 圆心；transform pivot 默认 0,0，要设到 obj 中心 */
    lv_obj_set_style_transform_pivot_x(s_ui.disc, DISC_SIZE / 2, 0);
    lv_obj_set_style_transform_pivot_y(s_ui.disc, DISC_SIZE / 2, 0);
}

static void create_progress(void)
{
    /* 进度条 track */
    s_ui.bar_track = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(s_ui.bar_track);
    lv_obj_set_size(s_ui.bar_track, 200, 4);
    lv_obj_align(s_ui.bar_track, LV_ALIGN_TOP_MID, 0, 238);
    lv_obj_set_style_radius(s_ui.bar_track, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_ui.bar_track, UI_C_BORDER, 0);
    lv_obj_set_style_bg_opa(s_ui.bar_track, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_ui.bar_track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_ui.bar_track, LV_OBJ_FLAG_CLICKABLE);

    s_ui.bar_fill = lv_obj_create(s_ui.bar_track);
    lv_obj_remove_style_all(s_ui.bar_fill);
    lv_obj_set_size(s_ui.bar_fill, 0, 4);
    lv_obj_align(s_ui.bar_fill, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(s_ui.bar_fill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_ui.bar_fill, UI_C_ACCENT, 0);
    lv_obj_set_style_bg_opa(s_ui.bar_fill, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_ui.bar_fill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_ui.bar_fill, LV_OBJ_FLAG_CLICKABLE);

    s_ui.time_lbl = lv_label_create(s_ui.screen);
    lv_obj_set_size(s_ui.time_lbl, 200, 14);
    lv_obj_align(s_ui.time_lbl, LV_ALIGN_TOP_MID, 0, 250);
    lv_obj_set_style_text_font(s_ui.time_lbl, APP_FONT_TEXT, 0);
    lv_obj_set_style_text_color(s_ui.time_lbl, UI_C_TEXT_MUTED, 0);
    lv_obj_set_style_text_align(s_ui.time_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_ui.time_lbl, "--:-- / --:--");
}

static lv_obj_t *make_round_btn(int size, int x_align, int y_align,
                                 const char *symbol, lv_color_t color,
                                 bool is_primary, lv_event_cb_t cb,
                                 lv_obj_t **out_icon)
{
    lv_obj_t *btn = lv_btn_create(s_ui.screen);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, size, size);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, x_align, y_align);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    if (is_primary) {
        lv_obj_set_style_bg_color(btn, UI_C_ACCENT, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_bg_color(btn, UI_C_PANEL, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(btn, UI_C_BORDER, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
    }
    lv_obj_set_style_bg_color(btn, UI_C_PANEL_HI, LV_STATE_PRESSED);

    lv_obj_t *ic = lv_label_create(btn);
    lv_label_set_text(ic, symbol);
    lv_obj_set_style_text_color(ic, color, 0);
    lv_obj_set_style_text_font(ic, is_primary ? APP_FONT_LARGE : APP_FONT_TITLE, 0);
    lv_obj_center(ic);
    if (out_icon) *out_icon = ic;

    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

/* ============================================================================
 * 刷新
 * ========================================================================= */

static void format_mmss(int16_t total_sec, char *out, size_t out_size)
{
    if (total_sec < 0) { snprintf(out, out_size, "--:--"); return; }
    snprintf(out, out_size, "%d:%02d", total_sec / 60, total_sec % 60);
}

static void refresh_static(const media_payload_t *m)
{
    lv_label_set_text(s_ui.title_lbl,
        (m && m->title[0]) ? m->title : "Nothing playing");
    lv_label_set_text(s_ui.artist_lbl,
        (m && m->artist[0]) ? m->artist : "--");
    lv_label_set_text(s_ui.pp_icon,
        (m && m->playing) ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);

    s_ui.last_playing = (m && m->playing);
}

static void refresh_progress(const media_payload_t *m)
{
    if (!m) {
        lv_obj_set_width(s_ui.bar_fill, 0);
        lv_label_set_text(s_ui.time_lbl, "--:-- / --:--");
        return;
    }
    int16_t pos = media_manager_get_position_now();
    if (pos == s_ui.last_pos_sec) return;
    s_ui.last_pos_sec = pos;

    int w = 0;
    if (pos >= 0 && m->duration_sec > 0) {
        w = (200 * pos) / m->duration_sec;
        if (w > 200) w = 200;
        if (w < 0) w = 0;
    }
    lv_obj_set_width(s_ui.bar_fill, w);

    char cur[8], total[8], buf[20];
    format_mmss(pos, cur, sizeof(cur));
    format_mmss(m->duration_sec, total, sizeof(total));
    snprintf(buf, sizeof(buf), "%s / %s", cur, total);
    lv_label_set_text(s_ui.time_lbl, buf);
}

/* ============================================================================
 * 旋转动画：每 60ms 让 disc 转 6°（一圈 3.6s）；playing=true 时跑
 * ========================================================================= */

static void disc_anim_cb(void *var, int32_t v)
{
    (void)v;
    s_ui.disc_angle = (s_ui.disc_angle + 6) % 3600;   /* 0.1°/unit, 6 = 0.6°/帧 */
    lv_obj_set_style_transform_rotation(s_ui.disc, s_ui.disc_angle, 0);
}

static void start_disc_spin(void)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &s_ui);
    lv_anim_set_exec_cb(&a, disc_anim_cb);
    lv_anim_set_values(&a, 0, 1);    /* 我们只关心 cb 频率，值无所谓 */
    lv_anim_set_duration(&a, 60);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

static void stop_disc_spin(void)
{
    lv_anim_del(&s_ui, disc_anim_cb);
}

/* ============================================================================
 * 事件
 * ========================================================================= */

static void on_pp_clicked(lv_event_t *e) { (void)e; media_service_send_button(MEDIA_BTN_PLAY_PAUSE); }
static void on_prev_clicked(lv_event_t *e) { (void)e; media_service_send_button(MEDIA_BTN_PREV); }
static void on_next_clicked(lv_event_t *e) { (void)e; media_service_send_button(MEDIA_BTN_NEXT); }

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
        ESP_LOGI(TAG, "swipe-up back to list (dy=%d)", dy);
        music_app_pop_or_exit();
    }
}

/* ============================================================================
 * 生命周期
 * ========================================================================= */

static lv_obj_t *create(void)
{
    memset(&s_ui, 0, sizeof(s_ui));
    s_ui.press_y0 = -1;
    s_ui.last_pos_sec = -2;

    s_ui.screen = lv_obj_create(NULL);
    ui_screen_setup(s_ui.screen);
    lv_obj_clear_flag(s_ui.screen, LV_OBJ_FLAG_SCROLLABLE);

    s_ui.statusbar = app_shell_attach_statusbar(s_ui.screen, false);

    create_disc();

    /* 标题 / 作者 */
    s_ui.title_lbl = lv_label_create(s_ui.screen);
    lv_obj_set_size(s_ui.title_lbl, 220, 20);
    lv_obj_align(s_ui.title_lbl, LV_ALIGN_TOP_MID, 0, 192);
    lv_label_set_long_mode(s_ui.title_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_ui.title_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_ui.title_lbl, APP_FONT_TITLE, 0);
    lv_obj_set_style_text_color(s_ui.title_lbl, UI_C_TEXT, 0);
    lv_label_set_text(s_ui.title_lbl, "--");

    s_ui.artist_lbl = lv_label_create(s_ui.screen);
    lv_obj_set_size(s_ui.artist_lbl, 220, 16);
    lv_obj_align(s_ui.artist_lbl, LV_ALIGN_TOP_MID, 0, 214);
    lv_label_set_long_mode(s_ui.artist_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_ui.artist_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_ui.artist_lbl, APP_FONT_TEXT, 0);
    lv_obj_set_style_text_color(s_ui.artist_lbl, UI_C_TEXT_MUTED, 0);
    lv_label_set_text(s_ui.artist_lbl, "--");

    create_progress();

    /* 三个按钮：cy≈277（hit zone 顶 290 - 13），small_top=259, big_top=253 */
    s_ui.prev_btn = make_round_btn(36, -60, 259, LV_SYMBOL_PREV,
                                    UI_C_TEXT, false, on_prev_clicked, NULL);
    s_ui.pp_btn = make_round_btn(48, 0, 253, LV_SYMBOL_PLAY,
                                  lv_color_hex(0xFFFFFF), true,
                                  on_pp_clicked, &s_ui.pp_icon);
    s_ui.next_btn = make_round_btn(36, 60, 259, LV_SYMBOL_NEXT,
                                    UI_C_TEXT, false, on_next_clicked, NULL);

    /* 底部 40px hit zone：纯透明、不放任何点击元素，专管上滑退出。
     * 屏幕最下面这条不放按钮，避免任何误触。 */
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

    /* 首屏渲染 */
    const media_payload_t *m = media_manager_get_latest();
    refresh_static(m);
    refresh_progress(m);
    s_ui.last_media_ver = media_manager_version();

    if (s_ui.last_playing) {
        start_disc_spin();
    }

    ui_anim_fade_in(s_ui.disc, 0);
    ui_anim_fade_in(s_ui.title_lbl, 80);
    ui_anim_fade_in(s_ui.artist_lbl, 120);
    return s_ui.screen;
}

static void destroy(void)
{
    stop_disc_spin();
    if (s_ui.screen) lv_obj_del(s_ui.screen);
    memset(&s_ui, 0, sizeof(s_ui));
    s_ui.press_y0 = -1;
}

static void update(void)
{
    uint32_t mv = media_manager_version();
    const media_payload_t *m = media_manager_get_latest();

    if (mv != s_ui.last_media_ver) {
        s_ui.last_media_ver = mv;
        s_ui.last_pos_sec = -2;
        bool was_playing = s_ui.last_playing;
        refresh_static(m);
        if (s_ui.last_playing && !was_playing) start_disc_spin();
        if (!s_ui.last_playing && was_playing) stop_disc_spin();
    }
    if (m) refresh_progress(m);
}

static const page_callbacks_t s_cb = {
    .create  = create,
    .destroy = destroy,
    .update  = update,
};

const page_callbacks_t *page_music_detail_get_callbacks(void)
{
    return &s_cb;
}
