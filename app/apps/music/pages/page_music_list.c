#include "page_music_list.h"
#include "music_app.h"
#include "app_router.h"
#include "app_shell_ui.h"

#include "playlist_manager.h"
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

static const char *TAG = "page_music_list";

/* ============================================================================
 * 布局规划（240×320）
 *
 *   y=0~24    statusbar
 *   y=24~56   header（"本地音乐" 16px + 右侧 "N 首" 计数）
 *   y=56~240  list 容器（184px，可滚动）
 *   y=240~290 mini-player（50px，常驻）
 *   y=290~320 底部 hit zone（30px 透明，纯上滑退出区，不放任何点击元素）
 *
 * 设计原则：屏幕最底 30px 留白给上滑手势，参考通知页和现代手机 app
 * 底部"home indicator"区域，避免误触。
 * ========================================================================= */

#define MINI_PLAYER_H   50
#define HIT_ZONE_H      30
#define LIST_ITEM_H     44

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *statusbar;
    lv_obj_t *header;
    lv_obj_t *count_lbl;

    lv_obj_t *list;            /* 滚动容器 */
    lv_obj_t *empty_box;

    lv_obj_t *mini;            /* 50px 容器 */
    lv_obj_t *mini_cover;
    lv_obj_t *mini_title;
    lv_obj_t *mini_artist;
    lv_obj_t *mini_pp_icon;
    lv_obj_t *mini_prev_btn;
    lv_obj_t *mini_pp_btn;
    lv_obj_t *mini_next_btn;
    lv_obj_t *mini_tap_zone;   /* 中间区点击 → 详情页 */
    lv_obj_t *hit_zone;        /* 底部 40px，纯上滑退出区 */

    uint32_t  last_playlist_ver;
    uint32_t  last_media_ver;
    char      last_title[MEDIA_TITLE_MAX];   /* 上次高亮的歌；title 变才重建列表 */
    bool      mini_visible;

    int       press_y0;
    int       press_y_last;
} ui_t;

static ui_t s_ui;

/* ============================================================================
 * helpers
 * ========================================================================= */

static void on_mini_tap(lv_event_t *e);
static void on_mini_btn_clicked(lv_event_t *e);
static void on_list_item_clicked(lv_event_t *e);
static void on_hit_pressed(lv_event_t *e);
static void on_hit_pressing(lv_event_t *e);
static void on_hit_released(lv_event_t *e);

/* ============================================================================
 * 列表构建
 * ========================================================================= */

static lv_obj_t *make_list_item(lv_obj_t *parent, size_t idx,
                                 const char *title, const char *artist,
                                 bool is_playing)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LIST_ITEM_H);
    lv_obj_set_style_pad_left(row, UI_SP_SM, 0);
    lv_obj_set_style_pad_right(row, UI_SP_SM, 0);
    lv_obj_set_style_radius(row, UI_R_SM, 0);
    lv_obj_set_style_bg_opa(row, is_playing ? LV_OPA_20 : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(row, UI_C_ACCENT, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(row, (void *)(uintptr_t)idx);
    lv_obj_add_event_cb(row, on_list_item_clicked, LV_EVENT_CLICKED, NULL);

    /* 序号 / 播放图标（左侧 16px） */
    lv_obj_t *idx_lbl = lv_label_create(row);
    lv_obj_set_size(idx_lbl, 18, 18);
    lv_obj_align(idx_lbl, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_align(idx_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(idx_lbl, APP_FONT_TEXT, 0);
    if (is_playing) {
        lv_obj_set_style_text_color(idx_lbl, UI_C_ACCENT, 0);
        lv_label_set_text(idx_lbl, LV_SYMBOL_PLAY);
    } else {
        lv_obj_set_style_text_color(idx_lbl, UI_C_TEXT_MUTED, 0);
        char buf[6];
        snprintf(buf, sizeof(buf), "%u", (unsigned)(idx + 1));
        lv_label_set_text(idx_lbl, buf);
    }

    /* 标题 */
    lv_obj_t *t = lv_label_create(row);
    lv_obj_set_size(t, 178, 18);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 24, 4);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(t, APP_FONT_TEXT, 0);
    lv_obj_set_style_text_color(t, is_playing ? UI_C_ACCENT : UI_C_TEXT, 0);
    lv_label_set_text(t, (title && title[0]) ? title : "Unknown");

    /* 作者 */
    lv_obj_t *a = lv_label_create(row);
    lv_obj_set_size(a, 178, 14);
    lv_obj_align(a, LV_ALIGN_TOP_LEFT, 24, 24);
    lv_label_set_long_mode(a, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(a, APP_FONT_TEXT, 0);
    lv_obj_set_style_text_color(a, UI_C_TEXT_MUTED, 0);
    lv_label_set_text(a, (artist && artist[0]) ? artist : "--");

    return row;
}

/* 找当前播放曲在歌单里的下标（按 title 前缀匹配），找不到返回 -1
 *
 * 注意：歌单协议 title 字段 40B（39 字符 + \0），NOWPLAYING title 48B（47 字符）。
 * 长曲名在歌单端会被截断，所以用"歌单 title 作为前缀去匹配 nowplaying"，
 * 不能反过来 strncmp(40B) — 长名第 40 位 nowplaying 还有字符，歌单已 \0，永远不等。
 */
static int find_playing_index(const media_payload_t *m)
{
    if (!m || !m->title[0]) return -1;
    size_t n = playlist_manager_get_count();
    for (size_t i = 0; i < n; i++) {
        const media_playlist_item_t *it = playlist_manager_get_track_at(i);
        if (!it) continue;
        size_t pl_len = strnlen(it->title, MEDIA_PLAYLIST_TITLE_MAX);
        if (pl_len == 0) continue;
        if (strncmp(it->title, m->title, pl_len) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void rebuild_list(void)
{
    lv_obj_clean(s_ui.list);

    size_t n = playlist_manager_get_count();
    if (n == 0) {
        lv_obj_add_flag(s_ui.list, LV_OBJ_FLAG_HIDDEN);
        if (!s_ui.empty_box) {
            s_ui.empty_box = lv_obj_create(s_ui.screen);
            lv_obj_remove_style_all(s_ui.empty_box);
            lv_obj_set_size(s_ui.empty_box, 200, 100);
            lv_obj_align(s_ui.empty_box, LV_ALIGN_CENTER, 0, -10);
            lv_obj_clear_flag(s_ui.empty_box, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *icon = lv_label_create(s_ui.empty_box);
            lv_obj_set_style_text_font(icon, APP_FONT_ICONS_36, 0);
            lv_obj_set_style_text_color(icon, UI_C_TEXT_MUTED, 0);
            lv_label_set_text(icon, ICON_MUSIC);
            lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 0);

            lv_obj_t *t = lv_label_create(s_ui.empty_box);
            lv_obj_set_style_text_font(t, APP_FONT_TITLE, 0);
            lv_obj_set_style_text_color(t, UI_C_TEXT_MUTED, 0);
            lv_label_set_text(t, "等待 PC 推送歌单");
            lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 50);
        }
        lv_obj_clear_flag(s_ui.empty_box, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(s_ui.list, LV_OBJ_FLAG_HIDDEN);
    if (s_ui.empty_box) lv_obj_add_flag(s_ui.empty_box, LV_OBJ_FLAG_HIDDEN);

    int playing_idx = find_playing_index(media_manager_get_latest());
    for (size_t i = 0; i < n; i++) {
        const media_playlist_item_t *it = playlist_manager_get_track_at(i);
        if (!it) continue;
        make_list_item(s_ui.list, i, it->title, it->artist,
                       (int)i == playing_idx);
    }

    /* 滚到正在播放项 */
    if (playing_idx >= 0) {
        lv_obj_t *target = lv_obj_get_child(s_ui.list, playing_idx);
        if (target) lv_obj_scroll_to_view_recursive(target, LV_ANIM_OFF);
    }

    char buf[16];
    snprintf(buf, sizeof(buf), "%u 首", (unsigned)n);
    lv_label_set_text(s_ui.count_lbl, buf);
}

/* ============================================================================
 * mini-player
 * ========================================================================= */

static void update_mini_player(void)
{
    const media_payload_t *m = media_manager_get_latest();
    bool has_track = (m && m->title[0] != '\0');

    /* mini-player 始终显示；hit zone 也始终在（贴在 mini-player 上方 6px）*/
    if (!s_ui.mini_visible) {
        s_ui.mini_visible = true;
        lv_obj_clear_flag(s_ui.mini, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(s_ui.list, 320 - 24 - 32 - MINI_PLAYER_H - HIT_ZONE_H);
    }

    if (has_track) {
        lv_label_set_text(s_ui.mini_title,  m->title);
        lv_label_set_text(s_ui.mini_artist, m->artist[0] ? m->artist : "--");
        lv_label_set_text(s_ui.mini_pp_icon,
            m->playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
        lv_obj_set_style_text_color(s_ui.mini_title, UI_C_TEXT, 0);
    } else {
        lv_label_set_text(s_ui.mini_title,  "暂无播放");
        lv_label_set_text(s_ui.mini_artist, "点击歌曲开始播放");
        lv_label_set_text(s_ui.mini_pp_icon, LV_SYMBOL_PLAY);
        lv_obj_set_style_text_color(s_ui.mini_title, UI_C_TEXT_MUTED, 0);
    }
}

static lv_obj_t *make_mini_btn(lv_obj_t *parent, const char *symbol,
                                lv_color_t color, uint8_t btn_id,
                                lv_obj_t **out_icon)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 28, 28);
    lv_obj_set_style_radius(btn, UI_R_SM, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(btn, UI_C_PANEL_HI, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(btn, (void *)(uintptr_t)btn_id);

    lv_obj_t *ic = lv_label_create(btn);
    lv_label_set_text(ic, symbol);
    lv_obj_set_style_text_color(ic, color, 0);
    lv_obj_set_style_text_font(ic, APP_FONT_TITLE, 0);
    lv_obj_center(ic);
    if (out_icon) *out_icon = ic;

    lv_obj_add_event_cb(btn, on_mini_btn_clicked, LV_EVENT_CLICKED, NULL);
    return btn;
}

static void create_mini_player(void)
{
    s_ui.mini = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(s_ui.mini);
    lv_obj_set_size(s_ui.mini, 240, MINI_PLAYER_H);
    lv_obj_align(s_ui.mini, LV_ALIGN_BOTTOM_MID, 0, -HIT_ZONE_H);
    lv_obj_set_style_bg_color(s_ui.mini, UI_C_PANEL, 0);
    lv_obj_set_style_bg_opa(s_ui.mini, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_ui.mini, UI_C_BORDER, 0);
    lv_obj_set_style_border_width(s_ui.mini, 1, 0);
    lv_obj_set_style_border_side(s_ui.mini, LV_BORDER_SIDE_TOP, 0);
    lv_obj_clear_flag(s_ui.mini, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ui.mini, LV_OBJ_FLAG_HIDDEN);

    /* tap zone：左侧 145px 区域整个可点（最先建，处于最底层；按钮和 cover 后建覆盖在上）*/
    s_ui.mini_tap_zone = lv_obj_create(s_ui.mini);
    lv_obj_remove_style_all(s_ui.mini_tap_zone);
    lv_obj_set_size(s_ui.mini_tap_zone, 145, MINI_PLAYER_H);
    lv_obj_align(s_ui.mini_tap_zone, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_flag(s_ui.mini_tap_zone, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_ui.mini_tap_zone, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(s_ui.mini_tap_zone, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(s_ui.mini_tap_zone, on_mini_tap, LV_EVENT_CLICKED, NULL);

    /* cover 36×36（紫色方块占位），覆盖在 tap zone 之上但不 clickable —
     * 设置 bubble + 移除 clickable，让点击穿透到 tap zone */
    s_ui.mini_cover = lv_obj_create(s_ui.mini);
    lv_obj_remove_style_all(s_ui.mini_cover);
    lv_obj_set_size(s_ui.mini_cover, 36, 36);
    lv_obj_align(s_ui.mini_cover, LV_ALIGN_LEFT_MID, UI_SP_SM, 0);
    lv_obj_set_style_radius(s_ui.mini_cover, UI_R_SM, 0);
    lv_obj_set_style_bg_color(s_ui.mini_cover, UI_C_ACCENT_2, 0);
    lv_obj_set_style_bg_opa(s_ui.mini_cover, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_ui.mini_cover, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_ui.mini_cover, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *cv_icon = lv_label_create(s_ui.mini_cover);
    lv_label_set_text(cv_icon, ICON_MUSIC);
    lv_obj_set_style_text_color(cv_icon, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(cv_icon, APP_FONT_ICONS_24, 0);
    lv_obj_center(cv_icon);

    s_ui.mini_title = lv_label_create(s_ui.mini);
    lv_obj_set_size(s_ui.mini_title, 95, 16);
    lv_obj_align(s_ui.mini_title, LV_ALIGN_LEFT_MID, 50, -8);
    lv_label_set_long_mode(s_ui.mini_title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(s_ui.mini_title, APP_FONT_TEXT, 0);
    lv_obj_set_style_text_color(s_ui.mini_title, UI_C_TEXT, 0);
    lv_label_set_text(s_ui.mini_title, "--");

    s_ui.mini_artist = lv_label_create(s_ui.mini);
    lv_obj_set_size(s_ui.mini_artist, 95, 14);
    lv_obj_align(s_ui.mini_artist, LV_ALIGN_LEFT_MID, 50, 8);
    lv_label_set_long_mode(s_ui.mini_artist, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(s_ui.mini_artist, APP_FONT_TEXT, 0);
    lv_obj_set_style_text_color(s_ui.mini_artist, UI_C_TEXT_MUTED, 0);
    lv_label_set_text(s_ui.mini_artist, "--");

    /* 三个按钮（贴右侧，自带 clickable，z-order 在 tap zone 之上）*/
    s_ui.mini_prev_btn = make_mini_btn(s_ui.mini, LV_SYMBOL_PREV,
                                       UI_C_TEXT, MEDIA_BTN_PREV, NULL);
    lv_obj_align(s_ui.mini_prev_btn, LV_ALIGN_RIGHT_MID, -64, 0);

    s_ui.mini_pp_btn = make_mini_btn(s_ui.mini, LV_SYMBOL_PLAY,
                                     UI_C_ACCENT, MEDIA_BTN_PLAY_PAUSE, &s_ui.mini_pp_icon);
    lv_obj_align(s_ui.mini_pp_btn, LV_ALIGN_RIGHT_MID, -34, 0);

    s_ui.mini_next_btn = make_mini_btn(s_ui.mini, LV_SYMBOL_NEXT,
                                       UI_C_TEXT, MEDIA_BTN_NEXT, NULL);
    lv_obj_align(s_ui.mini_next_btn, LV_ALIGN_RIGHT_MID, -4, 0);
}

/* ============================================================================
 * 事件
 * ========================================================================= */

static void on_list_item_clicked(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_target(e);
    size_t idx = (size_t)(uintptr_t)lv_obj_get_user_data(row);
    ESP_LOGI(TAG, "play track idx=%u", (unsigned)idx);
    media_service_send_play_track((uint16_t)idx);
}

static void on_mini_btn_clicked(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    uint8_t id = (uint8_t)(uintptr_t)lv_obj_get_user_data(btn);
    media_service_send_button(id);
}

static void on_mini_tap(lv_event_t *e)
{
    (void)e;
    music_app_push(MUSIC_PAGE_DETAIL);
}

/* hit zone：底部 6px 透明，按下→记 y0；松开→ dy>=30 退出 launcher */
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
        ESP_LOGI(TAG, "swipe-up exit (dy=%d)", dy);
        app_router_exit_to_launcher();
    }
}

/* ============================================================================
 * 生命周期
 * ========================================================================= */

static lv_obj_t *create(void)
{
    memset(&s_ui, 0, sizeof(s_ui));
    s_ui.press_y0 = -1;
    s_ui.last_playlist_ver = 0;
    s_ui.last_media_ver = 0;
    s_ui.mini_visible = false;

    s_ui.screen = lv_obj_create(NULL);
    ui_screen_setup(s_ui.screen);
    lv_obj_clear_flag(s_ui.screen, LV_OBJ_FLAG_SCROLLABLE);

    s_ui.statusbar = app_shell_attach_statusbar(s_ui.screen, false);

    /* header */
    s_ui.header = lv_label_create(s_ui.screen);
    lv_obj_set_style_text_font(s_ui.header, APP_FONT_TITLE, 0);
    lv_obj_set_style_text_color(s_ui.header, UI_C_TEXT, 0);
    lv_label_set_text(s_ui.header, "本地音乐");
    lv_obj_align(s_ui.header, LV_ALIGN_TOP_LEFT, UI_SP_MD, 24 + UI_SP_SM);

    s_ui.count_lbl = lv_label_create(s_ui.screen);
    lv_obj_set_style_text_font(s_ui.count_lbl, APP_FONT_TEXT, 0);
    lv_obj_set_style_text_color(s_ui.count_lbl, UI_C_TEXT_MUTED, 0);
    lv_label_set_text(s_ui.count_lbl, "0 首");
    lv_obj_align(s_ui.count_lbl, LV_ALIGN_TOP_RIGHT, -UI_SP_MD, 24 + UI_SP_SM + 2);

    /* list 滚动容器 */
    s_ui.list = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(s_ui.list);
    lv_obj_set_width(s_ui.list, 240);
    lv_obj_set_height(s_ui.list, 320 - 24 - 32 - MINI_PLAYER_H);
    lv_obj_align(s_ui.list, LV_ALIGN_TOP_LEFT, 0, 24 + 32);
    lv_obj_set_style_pad_left(s_ui.list, UI_SP_SM, 0);
    lv_obj_set_style_pad_right(s_ui.list, UI_SP_SM, 0);
    lv_obj_set_flex_flow(s_ui.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_ui.list, 2, 0);
    lv_obj_set_scroll_dir(s_ui.list, LV_DIR_VER);

    /* mini-player（默认隐藏） */
    create_mini_player();

    /* 底部 40px hit zone：纯透明、不放任何点击元素，专管上滑退出。
     * 参考通知页的做法 + 现代手机 home indicator 区域，避免误触。 */
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

    rebuild_list();
    update_mini_player();

    s_ui.last_playlist_ver = playlist_manager_version();
    s_ui.last_media_ver = media_manager_version();
    const media_payload_t *m0 = media_manager_get_latest();
    if (m0 && m0->title[0]) {
        strncpy(s_ui.last_title, m0->title, MEDIA_TITLE_MAX - 1);
        s_ui.last_title[MEDIA_TITLE_MAX - 1] = '\0';
    } else {
        s_ui.last_title[0] = '\0';
    }

    ui_anim_fade_in(s_ui.header, 0);
    ui_anim_fade_in(s_ui.count_lbl, 60);
    ui_anim_fade_in(s_ui.list, 120);

    return s_ui.screen;
}

static void destroy(void)
{
    if (s_ui.screen) {
        lv_obj_del(s_ui.screen);
    }
    memset(&s_ui, 0, sizeof(s_ui));
    s_ui.press_y0 = -1;
}

static void update(void)
{
    uint32_t pv = playlist_manager_version();
    uint32_t mv = media_manager_version();

    if (pv != s_ui.last_playlist_ver) {
        s_ui.last_playlist_ver = pv;
        rebuild_list();
        update_mini_player();
        return;
    }
    if (mv != s_ui.last_media_ver) {
        s_ui.last_media_ver = mv;
        update_mini_player();
        /* 仅当"当前播放的歌"换了才重建列表（重建会重置滚动位置）*/
        const media_payload_t *m = media_manager_get_latest();
        const char *cur_title = (m && m->title[0]) ? m->title : "";
        if (strncmp(s_ui.last_title, cur_title, MEDIA_TITLE_MAX) != 0) {
            strncpy(s_ui.last_title, cur_title, MEDIA_TITLE_MAX - 1);
            s_ui.last_title[MEDIA_TITLE_MAX - 1] = '\0';
            rebuild_list();
        }
    }
}

static const page_callbacks_t s_cb = {
    .create  = create,
    .destroy = destroy,
    .update  = update,
};

const page_callbacks_t *page_music_list_get_callbacks(void)
{
    return &s_cb;
}
