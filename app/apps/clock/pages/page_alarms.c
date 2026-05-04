#include "page_alarms.h"
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

static const char *TAG = "page_alarms";

/* ============================================================================
 * 闹钟列表（仅 UI；后续接 NVS）
 *
 * 假数据：3 条（07:30 工作日、12:00 每天 关、22:30 每天）
 * 右上角悬浮 + 圆形 FAB → push 到 alarm_edit 子页
 * 卡片右侧 switch 切换开关（点击切换 enabled，纯本地状态）
 * ========================================================================= */

#define MAX_ALARMS 8

typedef struct {
    uint8_t hour;
    uint8_t minute;
    bool    enabled;
    char    label[24];
} alarm_t;

static alarm_t s_alarms[MAX_ALARMS] = {
    { 7,  30, true,  "工作日 · 起床" },
    { 12,  0, false, "每天 · 午休" },
    { 22, 30, true,  "每天 · 睡觉" },
};
static int s_alarm_count = 3;

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *list;
    lv_obj_t *fab;
} ui_t;
static ui_t s_ui;

/* 让按钮事件能 bubble 到 screen（左右滑切 tab） */
#define BUBBLE_FLAG  LV_OBJ_FLAG_EVENT_BUBBLE

/* ============================================================================
 * 卡片
 * ========================================================================= */

static void rebuild_list(void);

static void on_switch_clicked(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_alarm_count) return;
    s_alarms[idx].enabled = !s_alarms[idx].enabled;
    rebuild_list();
}

static void on_card_clicked(lv_event_t *e)
{
    (void)e;
    /* 编辑现有闹钟 —— 暂未实现，先和"添加"走同一页 */
    clock_app_open_alarm_edit();
}

static lv_obj_t *make_card(lv_obj_t *parent, int idx, const alarm_t *a)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, lv_pct(100), 64);
    lv_obj_set_style_bg_color(card, UI_C_PANEL, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, UI_C_BORDER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_opa(card, LV_OPA_50, 0);
    lv_obj_set_style_radius(card, UI_R_LG, 0);
    lv_obj_set_style_pad_left(card, UI_SP_MD, 0);
    lv_obj_set_style_pad_right(card, UI_SP_MD, 0);
    lv_obj_set_style_bg_color(card, UI_C_PANEL_HI, LV_STATE_PRESSED);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(card, BUBBLE_FLAG);
    lv_obj_add_event_cb(card, on_card_clicked, LV_EVENT_CLICKED, NULL);

    /* 时间（大字号）*/
    lv_obj_t *time_lbl = lv_label_create(card);
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d", a->hour, a->minute);
    lv_label_set_text(time_lbl, buf);
    lv_obj_set_style_text_font(time_lbl, APP_FONT_LARGE, 0);
    lv_obj_set_style_text_color(time_lbl,
        a->enabled ? UI_C_TEXT : UI_C_TEXT_MUTED, 0);
    lv_obj_align(time_lbl, LV_ALIGN_LEFT_MID, 0, -8);
    lv_obj_add_flag(time_lbl, BUBBLE_FLAG);

    /* 备注 */
    lv_obj_t *meta_lbl = lv_label_create(card);
    lv_label_set_text(meta_lbl, a->label);
    lv_obj_set_style_text_font(meta_lbl, APP_FONT_TEXT, 0);
    lv_obj_set_style_text_color(meta_lbl, UI_C_TEXT_MUTED, 0);
    lv_obj_align(meta_lbl, LV_ALIGN_LEFT_MID, 0, 14);
    lv_obj_add_flag(meta_lbl, BUBBLE_FLAG);

    /* 右侧自绘 switch（36×20 胶囊 + 16×16 圆球）*/
    lv_obj_t *sw = lv_obj_create(card);
    lv_obj_remove_style_all(sw);
    lv_obj_set_size(sw, 36, 20);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(sw, a->enabled ? UI_C_ACCENT : UI_C_BORDER, 0);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(sw, 10, 0);
    lv_obj_clear_flag(sw, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(sw, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sw, on_switch_clicked, LV_EVENT_CLICKED,
                         (void *)(intptr_t)idx);

    lv_obj_t *knob = lv_obj_create(sw);
    lv_obj_remove_style_all(knob);
    lv_obj_set_size(knob, 16, 16);
    lv_obj_set_style_radius(knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(knob, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(knob, LV_OPA_COVER, 0);
    lv_obj_align(knob, a->enabled ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID,
                 a->enabled ? -2 : 2, 0);
    lv_obj_clear_flag(knob, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(knob, LV_OBJ_FLAG_CLICKABLE);
    return card;
}

static void rebuild_list(void)
{
    if (!s_ui.list) return;
    lv_obj_clean(s_ui.list);
    if (s_alarm_count == 0) {
        lv_obj_t *empty = lv_label_create(s_ui.list);
        lv_obj_set_style_text_font(empty, APP_FONT_TITLE, 0);
        lv_obj_set_style_text_color(empty, UI_C_TEXT_MUTED, 0);
        lv_label_set_text(empty, "没有闹钟");
        lv_obj_center(empty);
        return;
    }
    for (int i = 0; i < s_alarm_count; i++) {
        make_card(s_ui.list, i, &s_alarms[i]);
    }
}

/* ============================================================================
 * FAB（右上角 + 按钮）
 * ========================================================================= */

static void on_fab_clicked(lv_event_t *e)
{
    (void)e;
    clock_app_open_alarm_edit();
}

static void create_fab(lv_obj_t *parent)
{
    s_ui.fab = lv_obj_create(parent);
    lv_obj_remove_style_all(s_ui.fab);
    lv_obj_set_size(s_ui.fab, 28, 28);
    /* 在内容区右上角，紧贴 tabbar 下方 */
    lv_obj_set_pos(s_ui.fab, 240 - 28 - UI_SP_SM, UI_SP_SM);
    lv_obj_set_style_radius(s_ui.fab, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_ui.fab, UI_C_ACCENT, 0);
    lv_obj_set_style_bg_opa(s_ui.fab, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_ui.fab, UI_C_ACCENT_2, LV_STATE_PRESSED);
    lv_obj_clear_flag(s_ui.fab, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ui.fab, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_ui.fab, on_fab_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *plus = lv_label_create(s_ui.fab);
    lv_obj_set_style_text_font(plus, APP_FONT_TITLE, 0);
    lv_obj_set_style_text_color(plus, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(plus, "+");
    lv_obj_center(plus);
}

/* ============================================================================
 * 生命周期
 * ========================================================================= */

static lv_obj_t *create(void)
{
    ESP_LOGI(TAG, "create");
    memset(&s_ui, 0, sizeof(s_ui));

    s_ui.screen = lv_obj_create(NULL);
    ui_screen_setup(s_ui.screen);
    lv_obj_clear_flag(s_ui.screen, LV_OBJ_FLAG_SCROLLABLE);

    app_shell_attach_statusbar(s_ui.screen, false);
    clk_make_tabbar(s_ui.screen);

    /* 列表容器（可滚） */
    s_ui.list = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(s_ui.list);
    lv_obj_set_size(s_ui.list, 240, CLK_CONTENT_H);
    lv_obj_set_pos(s_ui.list, 0, CLK_CONTENT_TOP);
    lv_obj_set_style_pad_left(s_ui.list, UI_SP_MD, 0);
    lv_obj_set_style_pad_right(s_ui.list, UI_SP_MD, 0);
    lv_obj_set_style_pad_top(s_ui.list, UI_SP_SM, 0);
    lv_obj_set_style_pad_bottom(s_ui.list, UI_SP_SM, 0);
    lv_obj_set_flex_flow(s_ui.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_ui.list, UI_SP_SM, 0);
    lv_obj_set_scroll_dir(s_ui.list, LV_DIR_VER);

    rebuild_list();
    create_fab(s_ui.screen);

    clk_attach_hit_and_swipe(s_ui.screen);
    return s_ui.screen;
}

static void destroy(void)
{
    ESP_LOGI(TAG, "destroy");
    if (s_ui.screen) lv_obj_del(s_ui.screen);
    memset(&s_ui, 0, sizeof(s_ui));
}

static const page_callbacks_t s_cb = {
    .create  = create,
    .destroy = destroy,
    .update  = NULL,
};

const page_callbacks_t *page_alarms_get_callbacks(void)
{
    return &s_cb;
}
