#include "system_ui_common.h"
#include "system_app.h"
#include "app_router.h"
#include "app_fonts.h"
#include "ui_tokens.h"
#include "esp_log.h"

const int SYS_GAUGE_X[3] = { 16, 88, 160 };

/* ============================================================================
 * 圆盘
 * ========================================================================= */

sys_gauge_t sys_make_gauge(lv_obj_t *parent, int x, int y,
                            lv_color_t arc_color, const char *unit_str)
{
    sys_gauge_t g = {0};
    g.arc = lv_arc_create(parent);
    lv_obj_set_size(g.arc, SYS_GAUGE_SIZE, SYS_GAUGE_SIZE);
    lv_obj_set_pos(g.arc, x, y);
    lv_obj_remove_style(g.arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(g.arc, LV_OBJ_FLAG_CLICKABLE);
    lv_arc_set_rotation(g.arc, 270);
    lv_arc_set_bg_angles(g.arc, 0, 360);
    lv_arc_set_range(g.arc, 0, 100);
    lv_arc_set_value(g.arc, 0);
    lv_obj_set_style_arc_color(g.arc, UI_C_BORDER, LV_PART_MAIN);
    lv_obj_set_style_arc_width(g.arc, SYS_GAUGE_ARC_W, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(g.arc, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_arc_color(g.arc, arc_color, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(g.arc, SYS_GAUGE_ARC_W, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(g.arc, LV_OPA_COVER, LV_PART_INDICATOR);

    g.num_lbl = lv_label_create(g.arc);
    lv_obj_set_style_text_font(g.num_lbl, APP_FONT_TITLE, 0);
    lv_obj_set_style_text_color(g.num_lbl, UI_C_TEXT, 0);
    lv_label_set_text(g.num_lbl, "--");
    lv_obj_align(g.num_lbl, LV_ALIGN_CENTER, 0, -5);

    g.unit_lbl = lv_label_create(g.arc);
    lv_obj_set_style_text_font(g.unit_lbl, APP_FONT_TEXT, 0);
    lv_obj_set_style_text_color(g.unit_lbl, UI_C_TEXT_MUTED, 0);
    lv_label_set_text(g.unit_lbl, unit_str);
    lv_obj_align(g.unit_lbl, LV_ALIGN_CENTER, 0, 11);

    return g;
}

lv_obj_t *sys_make_gauge_label(lv_obj_t *parent, int x, int y, const char *txt)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_set_size(lbl, SYS_GAUGE_SIZE, 16);
    lv_obj_set_pos(lbl, x, y);
    lv_obj_set_style_text_font(lbl, APP_FONT_TEXT, 0);
    lv_obj_set_style_text_color(lbl, UI_C_TEXT_MUTED, 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(lbl, txt);
    return lbl;
}

void sys_apply_gauge(sys_gauge_t *g, int value, lv_color_t color_normal,
                      lv_color_t color_warn, int warn_at)
{
    if (value < 0) value = 0;
    if (value > 100) value = 100;
    lv_arc_set_value(g->arc, value);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", value);
    lv_label_set_text(g->num_lbl, buf);
    lv_color_t c = (warn_at > 0 && value >= warn_at) ? color_warn : color_normal;
    lv_obj_set_style_arc_color(g->arc, c, LV_PART_INDICATOR);
}

/* ============================================================================
 * 信息卡 + KV 行
 * ========================================================================= */

lv_obj_t *sys_make_info_card(lv_obj_t *parent, int y_offset, int height)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, 220, height);
    lv_obj_set_pos(c, (240 - 220) / 2, y_offset);
    lv_obj_set_style_bg_color(c, UI_C_PANEL, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(c, UI_C_BORDER, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_opa(c, LV_OPA_50, 0);
    lv_obj_set_style_radius(c, UI_R_LG, 0);
    lv_obj_set_style_pad_left(c, UI_SP_MD, 0);
    lv_obj_set_style_pad_right(c, UI_SP_MD, 0);
    lv_obj_set_style_pad_top(c, UI_SP_SM, 0);
    lv_obj_set_style_pad_bottom(c, UI_SP_SM, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE);
    return c;
}

sys_kv_t sys_make_kv_row(lv_obj_t *card, int y, const char *key)
{
    sys_kv_t r = {0};
    r.k = lv_label_create(card);
    lv_obj_set_pos(r.k, 0, y);
    lv_obj_set_style_text_font(r.k, APP_FONT_TEXT, 0);
    lv_obj_set_style_text_color(r.k, UI_C_TEXT_MUTED, 0);
    lv_label_set_text(r.k, key);

    r.v = lv_label_create(card);
    lv_obj_set_size(r.v, 150, 16);
    lv_obj_set_pos(r.v, 196 - 150, y);
    lv_obj_set_style_text_font(r.v, APP_FONT_TEXT, 0);
    lv_obj_set_style_text_color(r.v, UI_C_TEXT, 0);
    lv_obj_set_style_text_align(r.v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(r.v, LV_LABEL_LONG_DOT);
    lv_label_set_text(r.v, "--");
    return r;
}

/* ============================================================================
 * Tabbar
 *   layout:  [电脑]  [设备]
 *   两半各 120px，高 28，accent 文字 + 底部 2px 短下划线高亮当前
 *   children 顺序与 PAGE_ID 对齐：child(0)=PC, child(1)=DEV
 * ========================================================================= */

static void on_tab_pc_clicked(lv_event_t *e)  { (void)e; system_app_switch_to(SYSTEM_PAGE_PC); }
static void on_tab_dev_clicked(lv_event_t *e) { (void)e; system_app_switch_to(SYSTEM_PAGE_DEVICE); }

static lv_obj_t *make_tab_button(lv_obj_t *parent, int x, const char *text,
                                   lv_event_cb_t cb)
{
    lv_obj_t *t = lv_obj_create(parent);
    lv_obj_remove_style_all(t);
    lv_obj_set_size(t, 120, SYS_TABBAR_H);
    lv_obj_set_pos(t, x, 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(t, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(t);
    lv_obj_set_style_text_font(lbl, APP_FONT_TEXT, 0);
    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);

    lv_obj_t *under = lv_obj_create(t);
    lv_obj_remove_style_all(under);
    lv_obj_set_size(under, 32, 2);
    lv_obj_align(under, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(under, UI_C_ACCENT, 0);
    lv_obj_set_style_bg_opa(under, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(under, 1, 0);
    return t;
}

lv_obj_t *sys_make_tabbar(lv_obj_t *screen)
{
    lv_obj_t *bar = lv_obj_create(screen);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 240, SYS_TABBAR_H);
    lv_obj_set_pos(bar, 0, 24);
    lv_obj_set_style_bg_color(bar, UI_C_BG, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(bar, UI_C_BORDER, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_opa(bar, LV_OPA_40, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    make_tab_button(bar, 0,   "电脑", on_tab_pc_clicked);
    make_tab_button(bar, 120, "设备", on_tab_dev_clicked);

    sys_tabbar_update_active(bar);
    return bar;
}

void sys_tabbar_update_active(lv_obj_t *bar)
{
    if (!bar) return;
    system_page_id_t cur = system_app_current_page();
    for (int i = 0; i < 2; i++) {
        lv_obj_t *t = lv_obj_get_child(bar, i);
        if (!t) continue;
        bool active = (i == (int)cur);
        lv_obj_t *lbl   = lv_obj_get_child(t, 0);
        lv_obj_t *under = lv_obj_get_child(t, 1);
        if (lbl) {
            lv_obj_set_style_text_color(lbl,
                active ? UI_C_ACCENT : UI_C_TEXT_MUTED, 0);
        }
        if (under) {
            if (active) lv_obj_clear_flag(under, LV_OBJ_FLAG_HIDDEN);
            else        lv_obj_add_flag(under,   LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* ============================================================================
 * Hit zone（上滑退出）+ 整屏左右滑（切 tab）
 * ========================================================================= */

typedef struct {
    int press_y0;
    int press_y_last;
    int swipe_x0;
    int swipe_y0;
} sys_swipe_state_t;

/* 每个 page 一份 state（绑在 screen 的 user_data 上）。
 * page 销毁时随 screen 一起释放，因为 user_data 我们用 lv_malloc/堆分配 */
static sys_swipe_state_t *get_state(lv_obj_t *screen)
{
    sys_swipe_state_t *st = lv_obj_get_user_data(screen);
    if (!st) {
        st = lv_malloc(sizeof(*st));
        if (st) {
            st->press_y0 = -1;
            st->swipe_x0 = -1;
            lv_obj_set_user_data(screen, st);
        }
    }
    return st;
}

static void on_state_free(lv_event_t *e)
{
    lv_obj_t *screen = lv_event_get_target(e);
    sys_swipe_state_t *st = lv_obj_get_user_data(screen);
    if (st) {
        lv_free(st);
        lv_obj_set_user_data(screen, NULL);
    }
}

/* 底部 hit zone 事件：只看垂直 dy → 上滑退出 */
static void on_hit_pressed(lv_event_t *e)
{
    lv_obj_t *screen = lv_event_get_user_data(e);
    sys_swipe_state_t *st = get_state(screen);
    if (!st) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) { st->press_y0 = -1; return; }
    lv_point_t p; lv_indev_get_point(indev, &p);
    st->press_y0 = p.y;
    st->press_y_last = p.y;
}
static void on_hit_pressing(lv_event_t *e)
{
    lv_obj_t *screen = lv_event_get_user_data(e);
    sys_swipe_state_t *st = get_state(screen);
    if (!st) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p; lv_indev_get_point(indev, &p);
    st->press_y_last = p.y;
}
static void on_hit_released(lv_event_t *e)
{
    lv_obj_t *screen = lv_event_get_user_data(e);
    sys_swipe_state_t *st = get_state(screen);
    if (!st) return;
    if (st->press_y0 < 0) return;
    int dy = st->press_y0 - st->press_y_last;
    st->press_y0 = -1;
    if (dy >= 30) {
        system_app_exit();
    }
}

/* 整屏左右滑：识别 tab 切换 */
static void on_screen_pressed(lv_event_t *e)
{
    lv_obj_t *screen = lv_event_get_target(e);
    sys_swipe_state_t *st = get_state(screen);
    if (!st) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) { st->swipe_x0 = -1; return; }
    lv_point_t p; lv_indev_get_point(indev, &p);
    if (p.y > 280) { st->swipe_x0 = -1; return; }
    st->swipe_x0 = p.x;
    st->swipe_y0 = p.y;
}
static void on_screen_released(lv_event_t *e)
{
    lv_obj_t *screen = lv_event_get_target(e);
    sys_swipe_state_t *st = get_state(screen);
    if (!st) return;
    if (st->swipe_x0 < 0) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) { st->swipe_x0 = -1; return; }
    lv_point_t p; lv_indev_get_point(indev, &p);
    int dx = p.x - st->swipe_x0;
    int dy = p.y - st->swipe_y0;
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;
    st->swipe_x0 = -1;
    if (adx <= 50 || adx <= ady) return;
    if (dx > 0 && system_app_current_page() == SYSTEM_PAGE_DEVICE) {
        system_app_switch_to(SYSTEM_PAGE_PC);
    } else if (dx < 0 && system_app_current_page() == SYSTEM_PAGE_PC) {
        system_app_switch_to(SYSTEM_PAGE_DEVICE);
    }
}

void sys_attach_hit_and_swipe(lv_obj_t *screen)
{
    /* 创建底部 hit zone */
    lv_obj_t *hit = lv_obj_create(screen);
    lv_obj_remove_style_all(hit);
    lv_obj_set_size(hit, 240, SYS_HIT_ZONE_H);
    lv_obj_align(hit, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(hit, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(hit, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(hit, on_hit_pressed,  LV_EVENT_PRESSED,  screen);
    lv_obj_add_event_cb(hit, on_hit_pressing, LV_EVENT_PRESSING, screen);
    lv_obj_add_event_cb(hit, on_hit_released, LV_EVENT_RELEASED, screen);

    /* screen 自身：左右滑切 tab。
     * LVGL 9 默认 PRESSED/RELEASED 不冒泡 → child（卡/圆盘/容器）吞事件。
     * 解决：递归给 content / cards / 圆盘 加 EVENT_BUBBLE，让事件冒到 screen。
     * tabbar 是 screen 的直接 child，但内部 tab 按钮各自处理 CLICK；
     * 它们的 PRESSED/RELEASED 也会冒泡，但 screen 自己判断 dx>50 才切 tab，
     * 点 tab 时 dx≈0 不会误触。
     */
    uint32_t cnt = lv_obj_get_child_cnt(screen);
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t *c = lv_obj_get_child(screen, i);
        if (c == hit) continue;            /* hit 自己已绑回调，不要冒泡 */
        lv_obj_add_flag(c, LV_OBJ_FLAG_EVENT_BUBBLE);
        /* 一层够用 — content 容器内的 cards / arcs 都是 content 的 child，
         * 它们的事件会先冒到 content，content 再冒到 screen */
        uint32_t cc = lv_obj_get_child_cnt(c);
        for (uint32_t j = 0; j < cc; j++) {
            lv_obj_t *gc = lv_obj_get_child(c, j);
            lv_obj_add_flag(gc, LV_OBJ_FLAG_EVENT_BUBBLE);
        }
    }

    lv_obj_add_event_cb(screen, on_screen_pressed,  LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(screen, on_screen_released, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(screen, on_state_free,      LV_EVENT_DELETE,   NULL);

    (void)get_state(screen);
}
