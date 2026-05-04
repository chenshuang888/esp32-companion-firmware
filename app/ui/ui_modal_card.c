#include "ui_modal_card.h"
#include "ui_tokens.h"
#include "ui_anim.h"

#include <stdlib.h>

/* ============================================================================
 * 布局参数
 * ========================================================================= */

#define MODAL_W              224
#define MODAL_MAX_H          270         /* 卡片最大高，超过滚动 */
#define MODAL_MIN_H          80
#define MODAL_PAD            UI_SP_MD    /* 12 */
#define MODAL_BTN_BAR_H      44
#define MODAL_BTN_GAP        UI_SP_SM    /* 8 */

/* ============================================================================
 * 内部结构
 * ========================================================================= */

#define MAX_ACTIONS 2

typedef struct {
    ui_modal_action_cb_t cb;
    void *user_data;
} action_t;

struct ui_modal_card {
    lv_obj_t *overlay;        /* 全屏半透明遮罩 */
    lv_obj_t *card;           /* 居中卡片 */
    lv_obj_t *content;        /* 内容滚动容器（用户往这里加东西） */
    lv_obj_t *btn_bar;        /* 底部按钮栏（NULL 表示无按钮） */

    action_t  actions[MAX_ACTIONS];
    int       action_count;

    int       press_y0;       /* 卡片下滑关闭 */
};

/* ============================================================================
 * 关闭与销毁
 * ========================================================================= */

static void destroy_modal(ui_modal_card_t *m)
{
    if (!m) return;
    if (m->overlay) {
        lv_obj_del(m->overlay);
        m->overlay = NULL;
    }
    free(m);
}

void ui_modal_card_close(ui_modal_card_t *m)
{
    if (!m || !m->overlay) return;
    /* 简单淡出后销毁 */
    lv_obj_set_style_opa(m->overlay, LV_OPA_TRANSP, 0);
    /* 让淡出有一帧机会，再销毁；这里直接销毁也可，肉眼可接受 */
    destroy_modal(m);
}

/* ============================================================================
 * 事件
 * ========================================================================= */

/* 点击遮罩：关闭 */
static void on_overlay_clicked(lv_event_t *e)
{
    ui_modal_card_t *m = (ui_modal_card_t *)lv_event_get_user_data(e);
    /* 只有点到 overlay 自身（不是 card）才关闭 */
    lv_obj_t *target = lv_event_get_target(e);
    if (target == m->overlay) {
        ui_modal_card_close(m);
    }
}

/* 卡片记起手 y */
static void on_card_pressed(lv_event_t *e)
{
    ui_modal_card_t *m = (ui_modal_card_t *)lv_event_get_user_data(e);
    lv_indev_t *indev = lv_indev_active();
    if (!indev) { m->press_y0 = -1; return; }
    lv_point_t p; lv_indev_get_point(indev, &p);
    m->press_y0 = p.y;
}

/* 卡片下滑：关闭 */
static void on_card_gesture(lv_event_t *e)
{
    ui_modal_card_t *m = (ui_modal_card_t *)lv_event_get_user_data(e);
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_BOTTOM) {
        ui_modal_card_close(m);
    }
    m->press_y0 = -1;
}

/* 按钮点击 */
static void on_action_clicked(lv_event_t *e)
{
    ui_modal_card_t *m = (ui_modal_card_t *)lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
    if (idx < 0 || idx >= m->action_count) {
        ui_modal_card_close(m);
        return;
    }
    action_t a = m->actions[idx];   /* 拷贝，因为下面 close 会释放 m */
    /* 先关闭再回调，避免回调里再次操作即将销毁的对象 */
    ui_modal_card_close(m);
    if (a.cb) a.cb(a.user_data);
}

/* ============================================================================
 * 公开 API
 * ========================================================================= */

ui_modal_card_t *ui_modal_card_create(void)
{
    ui_modal_card_t *m = (ui_modal_card_t *)calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->press_y0 = -1;

    /* 全屏遮罩（覆盖到当前 active screen） */
    m->overlay = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(m->overlay);
    lv_obj_set_size(m->overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_align(m->overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(m->overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa  (m->overlay, LV_OPA_60, 0);
    lv_obj_clear_flag(m->overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag  (m->overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(m->overlay, on_overlay_clicked, LV_EVENT_CLICKED, m);

    /* 居中卡片 */
    m->card = lv_obj_create(m->overlay);
    lv_obj_remove_style_all(m->card);
    lv_obj_set_width(m->card, MODAL_W);
    lv_obj_set_height(m->card, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(m->card, MODAL_MAX_H, 0);
    lv_obj_set_style_min_height(m->card, MODAL_MIN_H, 0);
    lv_obj_align(m->card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(m->card, UI_C_PANEL, 0);
    lv_obj_set_style_bg_opa  (m->card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius  (m->card, UI_R_LG, 0);
    lv_obj_set_style_border_width(m->card, 1, 0);
    lv_obj_set_style_border_color(m->card, UI_C_BORDER, 0);
    lv_obj_set_style_border_opa(m->card, LV_OPA_50, 0);
    lv_obj_set_style_shadow_width(m->card, 0, 0);
    lv_obj_set_style_pad_all(m->card, MODAL_PAD, 0);

    /* 卡片自身纵向 flex：[content] + [btn_bar] */
    lv_obj_set_flex_flow(m->card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(m->card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(m->card, UI_SP_SM, 0);

    /* 卡片关闭事件：点 card 不冒泡到 overlay，按下记 y，下滑关闭 */
    lv_obj_add_flag(m->card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(m->card, on_card_pressed, LV_EVENT_PRESSED, m);
    lv_obj_add_event_cb(m->card, on_card_gesture, LV_EVENT_GESTURE, m);

    /* 内容容器 —— 调用方往这里加 */
    m->content = lv_obj_create(m->card);
    lv_obj_remove_style_all(m->content);
    lv_obj_set_width(m->content, LV_PCT(100));
    lv_obj_set_height(m->content, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(m->content, MODAL_MAX_H - 2 * MODAL_PAD - MODAL_BTN_BAR_H, 0);
    lv_obj_set_flex_flow(m->content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(m->content, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(m->content, UI_SP_SM, 0);
    lv_obj_set_style_pad_all(m->content, 0, 0);
    lv_obj_set_scroll_dir(m->content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(m->content, LV_SCROLLBAR_MODE_AUTO);

    return m;
}

lv_obj_t *ui_modal_card_content(ui_modal_card_t *m)
{
    return m ? m->content : NULL;
}

static void ensure_btn_bar(ui_modal_card_t *m)
{
    if (m->btn_bar) return;
    m->btn_bar = lv_obj_create(m->card);
    lv_obj_remove_style_all(m->btn_bar);
    lv_obj_set_size(m->btn_bar, LV_PCT(100), MODAL_BTN_BAR_H);
    lv_obj_clear_flag(m->btn_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(m->btn_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(m->btn_bar, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(m->btn_bar, MODAL_BTN_GAP, 0);
    lv_obj_set_style_pad_top(m->btn_bar, UI_SP_XS, 0);
}

void ui_modal_card_add_action(ui_modal_card_t *m, const char *label,
                                ui_modal_action_cb_t cb, void *user_data)
{
    if (!m || !label) return;
    if (m->action_count >= MAX_ACTIONS) return;

    ensure_btn_bar(m);

    int idx = m->action_count;
    m->actions[idx].cb = cb;
    m->actions[idx].user_data = user_data;
    m->action_count++;

    lv_obj_t *btn = lv_btn_create(m->btn_bar);
    lv_obj_remove_style_all(btn);
    lv_obj_set_height(btn, 32);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_style_radius(btn, UI_R_PILL, 0);
    lv_obj_set_style_bg_color(btn, idx == 0 ? UI_C_BG : UI_C_ACCENT, 0);
    lv_obj_set_style_bg_opa  (btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, idx == 0 ? 1 : 0, 0);
    lv_obj_set_style_border_color(btn, UI_C_BORDER, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_user_data(btn, (void *)(intptr_t)idx);
    lv_obj_add_event_cb(btn, on_action_clicked, LV_EVENT_CLICKED, m);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, idx == 0 ? UI_C_TEXT : UI_C_PANEL, 0);
    lv_obj_set_style_text_font (lbl, UI_F_TITLE, 0);
    lv_obj_center(lbl);
}

void ui_modal_card_show(ui_modal_card_t *m)
{
    if (!m || !m->overlay) return;
    /* 简单淡入 */
    lv_obj_set_style_opa(m->overlay, LV_OPA_TRANSP, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, m->overlay);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&a, UI_DUR_FAST);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_style_opa);
    lv_anim_start(&a);
}
