#include "page_notifications.h"
#include "app_router.h"
#include "app_shell_ui.h"
#include "notify_manager.h"

#include "esp_log.h"
#include "lvgl.h"
#include "app_fonts.h"
#include "ui_tokens.h"
#include "ui_widgets.h"
#include "ui_anim.h"
#include "ui_modal_card.h"
#include <stdio.h>
#include <time.h>

static const char *TAG = "page_notify";

/* ============================================================================
 * 布局规划（240×320 屏，statusbar 占 0~24）
 *
 *   y=24   statusbar
 *   y=32   "通知" 16px 标题 + 右侧徽章 "3"
 *   y=58   list 容器（220×212，可滚动）
 *   y=270  底部 50px 透明 hit zone（截获上滑手势退出）
 *
 * list 是 LVGL scroll 容器，会吞 GESTURE 给自己做滚动，EVENT_BUBBLE 也无效。
 * 所以底部专门放一个独立的 hit zone，自己处理 PRESSED + GESTURE。
 * ========================================================================= */

#define HIT_ZONE_HEIGHT  28

/* ============================================================================
 * UI 元素
 * ========================================================================= */

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *statusbar;
    lv_obj_t *title_lbl;
    lv_obj_t *count_badge;
    lv_obj_t *count_lbl;
    lv_obj_t *list;
    lv_obj_t *empty_box;
    lv_obj_t *hit_zone;        /* 底部透明手势区 */

    uint32_t last_version;
    int      press_y0;         /* 起手 y */
    int      press_y_last;     /* 最近一次 y（用于松开时算位移） */
} page_notify_ui_t;

static page_notify_ui_t s_ui = { .press_y0 = -1 };

/* 前向声明：模态相关回调（实现在文件下方） */
static void on_item_clicked(lv_event_t *e);
static void on_count_long_pressed(lv_event_t *e);

/* ============================================================================
 * category → (Material 图标, 主题色)
 * ========================================================================= */

typedef struct {
    const char *icon;        /* Material Symbols UTF-8 */
    lv_color_t  color;
} cat_style_t;

static cat_style_t cat_style_for(uint8_t cat)
{
    switch (cat) {
    case NOTIFY_CAT_MESSAGE:  return (cat_style_t){ ICON_NOTIFICATIONS, lv_color_hex(0x007AFF) };
    case NOTIFY_CAT_EMAIL:    return (cat_style_t){ ICON_NOTIFICATIONS, lv_color_hex(0xFF9500) };
    case NOTIFY_CAT_CALL:     return (cat_style_t){ ICON_NOTIFICATIONS, lv_color_hex(0x34C759) };
    case NOTIFY_CAT_CALENDAR: return (cat_style_t){ ICON_SCHEDULE,      lv_color_hex(0xAF52DE) };
    case NOTIFY_CAT_SOCIAL:   return (cat_style_t){ ICON_NOTIFICATIONS, lv_color_hex(0x5AC8FA) };
    case NOTIFY_CAT_NEWS:     return (cat_style_t){ ICON_NOTIFICATIONS, lv_color_hex(0x6E6E73) };
    case NOTIFY_CAT_ALERT:    return (cat_style_t){ ICON_NOTIFICATIONS, lv_color_hex(0xFF3B30) };
    case NOTIFY_CAT_GENERIC:
    default:                  return (cat_style_t){ ICON_NOTIFICATIONS, lv_color_hex(0x6E6E73) };
    }
}

/* ============================================================================
 * 头部：标题 + 徽章
 * ========================================================================= */

static void create_header(void)
{
    s_ui.title_lbl = lv_label_create(s_ui.screen);
    lv_label_set_text(s_ui.title_lbl, "通知");
    lv_obj_set_style_text_color(s_ui.title_lbl, UI_C_TEXT, 0);
    lv_obj_set_style_text_font (s_ui.title_lbl, UI_F_TITLE, 0);
    lv_obj_align(s_ui.title_lbl, LV_ALIGN_TOP_LEFT, UI_SP_MD, 32);

    /* 圆角徽章：UI_C_ACCENT 底 + 白字 */
    s_ui.count_badge = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(s_ui.count_badge);
    lv_obj_set_size(s_ui.count_badge, 36, 20);
    lv_obj_align(s_ui.count_badge, LV_ALIGN_TOP_RIGHT, -UI_SP_MD, 32);
    lv_obj_set_style_radius(s_ui.count_badge, UI_R_PILL, 0);
    lv_obj_set_style_bg_color(s_ui.count_badge, UI_C_ACCENT, 0);
    lv_obj_set_style_bg_opa  (s_ui.count_badge, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_ui.count_badge, 0, 0);
    lv_obj_clear_flag(s_ui.count_badge, LV_OBJ_FLAG_SCROLLABLE);

    s_ui.count_lbl = lv_label_create(s_ui.count_badge);
    lv_label_set_text(s_ui.count_lbl, "0");
    lv_obj_set_style_text_color(s_ui.count_lbl, UI_C_PANEL, 0);
    lv_obj_set_style_text_font (s_ui.count_lbl, UI_F_LABEL, 0);
    lv_obj_center(s_ui.count_lbl);

    /* 长按徽章弹"清空所有"确认 */
    lv_obj_add_flag(s_ui.count_badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_ui.count_badge, on_count_long_pressed,
                        LV_EVENT_LONG_PRESSED, NULL);
}

/* ============================================================================
 * list 容器
 * ========================================================================= */

static void create_list(void)
{
    s_ui.list = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(s_ui.list);
    lv_obj_set_size(s_ui.list, 220, 234);
    lv_obj_align(s_ui.list, LV_ALIGN_TOP_MID, 0, 58);

    lv_obj_set_flex_flow (s_ui.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_ui.list, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(s_ui.list, UI_SP_SM, 0);
    lv_obj_set_style_pad_all(s_ui.list, 0, 0);

    lv_obj_set_scroll_dir(s_ui.list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_ui.list, LV_SCROLLBAR_MODE_AUTO);
}

/* ============================================================================
 * 上滑退出 —— 自己算 dy（CLICKABLE 容器会抑制 LVGL gesture，不能用 EVENT_GESTURE）
 *
 * PRESSED   记起手 y0 / y_last
 * PRESSING  每帧更新 y_last
 * RELEASED  dy = y0 - y_last；dy >= UPSWIPE_THRESHOLD → 退出
 * ========================================================================= */

#define UPSWIPE_THRESHOLD 30   /* 累计上移 30px 视为有效上滑 */

static void on_pressed(lv_event_t *e)
{
    const char *src = (const char *)lv_event_get_user_data(e);
    lv_indev_t *indev = lv_indev_active();
    if (!indev) { s_ui.press_y0 = -1; return; }
    lv_point_t p; lv_indev_get_point(indev, &p);
    s_ui.press_y0 = p.y;
    s_ui.press_y_last = p.y;
    ESP_LOGI(TAG, "PRESS@%s  y=%d", src ? src : "?", (int)p.y);
}

static void on_pressing(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p; lv_indev_get_point(indev, &p);
    s_ui.press_y_last = p.y;
}

static void on_released(lv_event_t *e)
{
    const char *src = (const char *)lv_event_get_user_data(e);
    if (s_ui.press_y0 < 0) return;
    int dy = s_ui.press_y0 - s_ui.press_y_last;
    ESP_LOGI(TAG, "RELEASE@%s  y0=%d y1=%d dy=%d",
             src ? src : "?", s_ui.press_y0, s_ui.press_y_last, dy);
    s_ui.press_y0 = -1;
    if (dy >= UPSWIPE_THRESHOLD) {
        ESP_LOGI(TAG, "EXIT triggered (dy=%d)", dy);
        app_router_exit_to_launcher();
    }
}

static void create_hit_zone(void)
{
    s_ui.hit_zone = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(s_ui.hit_zone);
    lv_obj_set_size(s_ui.hit_zone, 240, HIT_ZONE_HEIGHT);
    lv_obj_align(s_ui.hit_zone, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(s_ui.hit_zone, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ui.hit_zone, 0, 0);
    lv_obj_clear_flag(s_ui.hit_zone, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ui.hit_zone, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_ui.hit_zone, on_pressed,  LV_EVENT_PRESSED,  (void *)"hit");
    lv_obj_add_event_cb(s_ui.hit_zone, on_pressing, LV_EVENT_PRESSING, (void *)"hit");
    lv_obj_add_event_cb(s_ui.hit_zone, on_released, LV_EVENT_RELEASED, (void *)"hit");
}

/* ============================================================================
 * 模态详情：点击列表项 / 长按徽章
 * ========================================================================= */

/* 当前打开的模态对应的通知 index（用于"删除"按钮回调） */
static size_t s_modal_index = 0;

static void on_modal_delete(void *user_data)
{
    (void)user_data;
    notify_manager_remove_at(s_modal_index);
    /* version 自增，update 周期会重建列表 */
}

static void on_modal_clear_all(void *user_data)
{
    (void)user_data;
    notify_manager_clear();
}

/* 把 unix ts 格式化成 "2026-05-01 14:32" */
static void format_full_time(uint32_t ts, char *out, size_t out_size)
{
    time_t t = (time_t)ts;
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(out, out_size, "%04d-%02d-%02d %02d:%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min);
}

static void show_detail_modal(size_t index)
{
    const notification_payload_t *n = notify_manager_get_at(index);
    if (!n) return;
    s_modal_index = index;

    cat_style_t cs = cat_style_for(n->category);

    ui_modal_card_t *m = ui_modal_card_create();
    if (!m) return;
    lv_obj_t *cnt = ui_modal_card_content(m);

    /* 顶部：图标块 + 完整时间戳（图标在左，时间在右下） */
    lv_obj_t *header = lv_obj_create(cnt);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, LV_PCT(100), 36);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(header, 0, 0);

    lv_obj_t *icon_box = lv_obj_create(header);
    lv_obj_remove_style_all(icon_box);
    lv_obj_set_size(icon_box, 36, 36);
    lv_obj_align(icon_box, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(icon_box, cs.color, 0);
    lv_obj_set_style_bg_opa  (icon_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius  (icon_box, UI_R_MD, 0);
    lv_obj_set_style_border_width(icon_box, 0, 0);
    lv_obj_clear_flag(icon_box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon = lv_label_create(icon_box);
    lv_label_set_text(icon, cs.icon);
    lv_obj_set_style_text_color(icon, UI_C_PANEL, 0);
    lv_obj_set_style_text_font (icon, APP_FONT_ICONS_24, 0);
    lv_obj_center(icon);

    char tbuf[24];
    format_full_time(n->timestamp, tbuf, sizeof(tbuf));
    lv_obj_t *time_lbl = lv_label_create(header);
    lv_label_set_text(time_lbl, tbuf);
    lv_obj_set_style_text_color(time_lbl, UI_C_TEXT_MUTED, 0);
    lv_obj_set_style_text_font (time_lbl, UI_F_LABEL, 0);
    lv_obj_align(time_lbl, LV_ALIGN_RIGHT_MID, 0, 0);

    /* 标题（多行 wrap，自适应高度）*/
    lv_obj_t *title = lv_label_create(cnt);
    lv_label_set_text(title, n->title[0] ? n->title : "(无标题)");
    lv_obj_set_style_text_color(title, UI_C_TEXT, 0);
    lv_obj_set_style_text_font (title, UI_F_TITLE, 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(title, LV_PCT(100));

    /* 分隔线 */
    lv_obj_t *sep = lv_obj_create(cnt);
    lv_obj_remove_style_all(sep);
    lv_obj_set_size(sep, LV_PCT(100), 1);
    lv_obj_set_style_bg_color(sep, UI_C_BORDER, 0);
    lv_obj_set_style_bg_opa  (sep, LV_OPA_50, 0);

    /* 正文（多行 wrap，自适应高度，超过模态最大高时整张卡内部滚动）*/
    if (n->body[0]) {
        lv_obj_t *body = lv_label_create(cnt);
        lv_label_set_text(body, n->body);
        lv_obj_set_style_text_color(body, UI_C_TEXT_DIM, 0);
        lv_obj_set_style_text_font (body, UI_F_BODY, 0);
        lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(body, LV_PCT(100));
    }

    ui_modal_card_add_action(m, "删除", on_modal_delete, NULL);
    ui_modal_card_add_action(m, "关闭", NULL, NULL);
    ui_modal_card_show(m);
}

static void show_clear_all_modal(void)
{
    if (notify_manager_count() == 0) return;

    ui_modal_card_t *m = ui_modal_card_create();
    if (!m) return;
    lv_obj_t *cnt = ui_modal_card_content(m);

    lv_obj_t *title = lv_label_create(cnt);
    lv_label_set_text(title, "清空所有通知？");
    lv_obj_set_style_text_color(title, UI_C_TEXT, 0);
    lv_obj_set_style_text_font (title, UI_F_TITLE, 0);

    lv_obj_t *desc = lv_label_create(cnt);
    lv_label_set_text(desc, "此操作将删除全部已收到的通知，且无法恢复。");
    lv_obj_set_style_text_color(desc, UI_C_TEXT_DIM, 0);
    lv_obj_set_style_text_font (desc, UI_F_BODY, 0);
    lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(desc, LV_PCT(100));

    ui_modal_card_add_action(m, "取消", NULL, NULL);
    ui_modal_card_add_action(m, "清空", on_modal_clear_all, NULL);
    ui_modal_card_show(m);
}

static void on_item_clicked(lv_event_t *e)
{
    size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
    show_detail_modal(idx);
}

static void on_count_long_pressed(lv_event_t *e)
{
    (void)e;
    show_clear_all_modal();
}

/* ============================================================================
 * 单条通知卡片（固定 64px 高 / 单行省略，保证列表节奏整齐）
 *
 *   ┌──────────────────────────────────────┐
 *   │ ┌──┐  Title (单行省略...)      14:32 │  y=2  20px
 *   │ │🔔 │                                │
 *   │ └──┘  body single line ellipsis...   │  y=26 18px
 *   └──────────────────────────────────────┘
 * ========================================================================= */

#define ITEM_H            64
#define ITEM_ICON         36
#define ITEM_PAD          UI_SP_SM       /* 8 */
#define ITEM_GAP          UI_SP_SM       /* icon 与文字间隔 */
#define ITEM_TIME_W       40
#define ITEM_TITLE_H      20
#define ITEM_BODY_H       18
#define ITEM_TITLE_Y      2
#define ITEM_BODY_Y       28

static void build_item(lv_obj_t *parent, const notification_payload_t *n, size_t index)
{
    cat_style_t cs = cat_style_for(n->category);

    lv_obj_t *card = ui_card(parent);
    lv_obj_set_size(card, lv_pct(100), ITEM_H);
    lv_obj_set_style_pad_all(card, ITEM_PAD, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    /* 整张卡可点 → 弹模态详情 */
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(card, (void *)(uintptr_t)index);
    lv_obj_add_event_cb(card, on_item_clicked, LV_EVENT_CLICKED, (void *)(uintptr_t)index);
    lv_obj_set_style_bg_color(card, UI_C_PANEL_HI, LV_STATE_PRESSED);

    /* 左侧 36×36 圆角图标块（垂直居中） */
    lv_obj_t *icon_box = lv_obj_create(card);
    lv_obj_remove_style_all(icon_box);
    lv_obj_set_size(icon_box, ITEM_ICON, ITEM_ICON);
    lv_obj_align(icon_box, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(icon_box, cs.color, 0);
    lv_obj_set_style_bg_opa  (icon_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius  (icon_box, UI_R_MD, 0);
    lv_obj_set_style_border_width(icon_box, 0, 0);
    lv_obj_clear_flag(icon_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all (icon_box, 0, 0);

    lv_obj_t *icon = lv_label_create(icon_box);
    lv_label_set_text(icon, cs.icon);
    lv_obj_set_style_text_color(icon, UI_C_PANEL, 0);
    lv_obj_set_style_text_font (icon, APP_FONT_ICONS_24, 0);
    lv_obj_center(icon);

    /* 时间 HH:MM —— 右上角，固定 40 宽 */
    char tbuf[8];
    time_t ts = (time_t)n->timestamp;
    struct tm tm;
    localtime_r(&ts, &tm);
    snprintf(tbuf, sizeof(tbuf), "%02d:%02d", tm.tm_hour, tm.tm_min);

    lv_obj_t *time_lbl = lv_label_create(card);
    lv_label_set_text(time_lbl, tbuf);
    lv_obj_set_style_text_color(time_lbl, UI_C_TEXT_MUTED, 0);
    lv_obj_set_style_text_font (time_lbl, UI_F_LABEL, 0);
    lv_obj_set_style_text_align(time_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_size(time_lbl, ITEM_TIME_W, ITEM_TITLE_H);
    lv_obj_align(time_lbl, LV_ALIGN_TOP_RIGHT, 0, ITEM_TITLE_Y);

    /* 文本区域水平边界
     * card 内宽 = 220 - 2*pad(16) = 204
     * 标题宽 = 204 - 36(icon) - 8(gap) - 40(time) - 4(留白) = 116
     * 正文宽 = 204 - 36(icon) - 8(gap)               = 160（占满第二行）
     */
    const int text_x = ITEM_ICON + ITEM_GAP;

    /* 标题：固定高度 + LONG_DOT 强制单行省略 */
    lv_obj_t *title = lv_label_create(card);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_size(title, 116, ITEM_TITLE_H);
    lv_label_set_text(title, n->title[0] ? n->title : "(无标题)");
    lv_obj_set_style_text_color(title, UI_C_TEXT, 0);
    lv_obj_set_style_text_font (title, UI_F_TITLE, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, text_x, ITEM_TITLE_Y);

    /* 正文：固定高度 + LONG_DOT 单行省略 */
    lv_obj_t *body = lv_label_create(card);
    lv_label_set_long_mode(body, LV_LABEL_LONG_DOT);
    lv_obj_set_size(body, 160, ITEM_BODY_H);
    lv_label_set_text(body, n->body[0] ? n->body : "");
    lv_obj_set_style_text_color(body, UI_C_TEXT_DIM, 0);
    lv_obj_set_style_text_font (body, UI_F_BODY, 0);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, text_x, ITEM_BODY_Y);
}

/* ============================================================================
 * 空状态
 * ========================================================================= */

static void show_empty(void)
{
    if (!s_ui.empty_box) {
        s_ui.empty_box = lv_obj_create(s_ui.screen);
        lv_obj_remove_style_all(s_ui.empty_box);
        lv_obj_set_size(s_ui.empty_box, 220, 120);
        lv_obj_align(s_ui.empty_box, LV_ALIGN_TOP_MID, 0, 100);
        lv_obj_clear_flag(s_ui.empty_box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow (s_ui.empty_box, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(s_ui.empty_box, LV_FLEX_ALIGN_CENTER,
                               LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(s_ui.empty_box, UI_SP_MD, 0);

        lv_obj_t *icon = lv_label_create(s_ui.empty_box);
        lv_label_set_text(icon, ICON_NOTIFICATIONS);
        lv_obj_set_style_text_color(icon, UI_C_TEXT_MUTED, 0);
        lv_obj_set_style_text_font (icon, APP_FONT_ICONS_36, 0);

        lv_obj_t *txt = lv_label_create(s_ui.empty_box);
        lv_label_set_text(txt, "暂无通知");
        lv_obj_set_style_text_color(txt, UI_C_TEXT_MUTED, 0);
        lv_obj_set_style_text_font (txt, UI_F_TITLE, 0);
    }
    lv_obj_clear_flag(s_ui.empty_box, LV_OBJ_FLAG_HIDDEN);
}

static void hide_empty(void)
{
    if (s_ui.empty_box) {
        lv_obj_add_flag(s_ui.empty_box, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ============================================================================
 * 刷新
 * ========================================================================= */

static void refresh_list(void)
{
    lv_obj_clean(s_ui.list);

    size_t n = notify_manager_count();

    /* 徽章数字（最多两位 + 缩放）*/
    char cnt_buf[8];
    if (n > 99) {
        snprintf(cnt_buf, sizeof(cnt_buf), "99+");
    } else {
        snprintf(cnt_buf, sizeof(cnt_buf), "%u", (unsigned)n);
    }
    lv_label_set_text(s_ui.count_lbl, cnt_buf);

    /* n=0 时徽章变灰 */
    if (n == 0) {
        lv_obj_set_style_bg_color(s_ui.count_badge, UI_C_BORDER, 0);
        show_empty();
        return;
    }
    lv_obj_set_style_bg_color(s_ui.count_badge, UI_C_ACCENT, 0);
    hide_empty();

    for (size_t i = 0; i < n; i++) {
        const notification_payload_t *item = notify_manager_get_at(i);
        if (!item) continue;
        build_item(s_ui.list, item, i);
    }
}

static void update_display(void)
{
    uint32_t v = notify_manager_version();
    if (v == s_ui.last_version) return;
    s_ui.last_version = v;
    refresh_list();
}

/* ============================================================================
 * 页面生命周期
 * ========================================================================= */

static lv_obj_t *page_notifications_create(void)
{
    ESP_LOGI(TAG, "Creating notifications page");

    s_ui.screen = lv_obj_create(NULL);
    ui_screen_setup(s_ui.screen);
    s_ui.press_y0 = -1;

    create_header();
    create_list();
    create_hit_zone();   /* 必须在 list 之后创建，z-order 在 list 上 */

    /* statusbar 最后挂；浅色页 dark=false */
    s_ui.statusbar = app_shell_attach_statusbar(s_ui.screen, false);

    s_ui.last_version = (uint32_t)-1;
    update_display();

    /* 入场动画 */
    ui_anim_fade_in(s_ui.title_lbl,   0);
    ui_anim_fade_in(s_ui.count_badge, 60);
    ui_anim_fade_in(s_ui.list,        120);

    return s_ui.screen;
}

static void page_notifications_destroy(void)
{
    ESP_LOGI(TAG, "Destroying notifications page");

    if (s_ui.screen) {
        lv_obj_del(s_ui.screen);
        s_ui.screen = NULL;
    }

    s_ui.statusbar = NULL;
    s_ui.title_lbl = NULL;
    s_ui.count_badge = NULL;
    s_ui.count_lbl = NULL;
    s_ui.list = NULL;
    s_ui.empty_box = NULL;
    s_ui.hit_zone = NULL;
    s_ui.press_y0 = -1;
}

static void page_notifications_update(void)
{
    update_display();
}

static const page_callbacks_t s_callbacks = {
    .create  = page_notifications_create,
    .destroy = page_notifications_destroy,
    .update  = page_notifications_update,
};

const page_callbacks_t *page_notifications_get_callbacks(void)
{
    return &s_callbacks;
}
