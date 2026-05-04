#include "clock_ui_common.h"
#include "clock_app.h"
#include "app_router.h"
#include "app_fonts.h"
#include "ui_tokens.h"
#include "esp_log.h"

/* ============================================================================
 * Tab bar：4 等分，60px / 个，文字 + 选中下划线
 * children 顺序对齐 PAGE_ID：
 *   child(0) = 闹钟  child(1) = 世界  child(2) = 秒表  child(3) = 计时
 * ========================================================================= */

static void on_tab_clicked(lv_event_t *e)
{
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    clock_app_switch_to((clock_page_id_t)id);
}

static lv_obj_t *make_tab(lv_obj_t *parent, int x, const char *text, int page_id)
{
    lv_obj_t *t = lv_obj_create(parent);
    lv_obj_remove_style_all(t);
    lv_obj_set_size(t, 60, CLK_TABBAR_H);
    lv_obj_set_pos(t, x, 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(t, on_tab_clicked, LV_EVENT_CLICKED,
                         (void *)(intptr_t)page_id);

    lv_obj_t *lbl = lv_label_create(t);
    lv_obj_set_style_text_font(lbl, APP_FONT_TEXT, 0);
    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);

    /* 选中下划线（短线，对齐 system app） */
    lv_obj_t *under = lv_obj_create(t);
    lv_obj_remove_style_all(under);
    lv_obj_set_size(under, 24, 2);
    lv_obj_align(under, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(under, UI_C_ACCENT, 0);
    lv_obj_set_style_bg_opa(under, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(under, 1, 0);
    return t;
}

lv_obj_t *clk_make_tabbar(lv_obj_t *screen)
{
    lv_obj_t *bar = lv_obj_create(screen);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 240, CLK_TABBAR_H);
    lv_obj_set_pos(bar, 0, 24);
    lv_obj_set_style_bg_color(bar, UI_C_BG, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(bar, UI_C_BORDER, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_opa(bar, LV_OPA_40, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    make_tab(bar, 0,   "闹钟", CLOCK_PAGE_ALARMS);
    make_tab(bar, 60,  "世界", CLOCK_PAGE_WORLD);
    make_tab(bar, 120, "秒表", CLOCK_PAGE_STOPWATCH);
    make_tab(bar, 180, "计时", CLOCK_PAGE_TIMER);

    clk_tabbar_update_active(bar);
    return bar;
}

void clk_tabbar_update_active(lv_obj_t *bar)
{
    if (!bar) return;
    clock_page_id_t cur = clock_app_current_page();
    for (int i = 0; i < 4; i++) {
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
 * Hit zone + 左右滑切 tab —— 复用 system app 的成熟模式
 * ========================================================================= */

typedef struct {
    int press_y0;
    int press_y_last;
    int swipe_x0;
    int swipe_y0;
} swipe_state_t;

static swipe_state_t *get_state(lv_obj_t *screen)
{
    swipe_state_t *st = lv_obj_get_user_data(screen);
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
    swipe_state_t *st = lv_obj_get_user_data(screen);
    if (st) {
        lv_free(st);
        lv_obj_set_user_data(screen, NULL);
    }
}

static void on_hit_pressed(lv_event_t *e)
{
    lv_obj_t *screen = lv_event_get_user_data(e);
    swipe_state_t *st = get_state(screen);
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
    swipe_state_t *st = get_state(screen);
    if (!st) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p; lv_indev_get_point(indev, &p);
    st->press_y_last = p.y;
}
static void on_hit_released(lv_event_t *e)
{
    lv_obj_t *screen = lv_event_get_user_data(e);
    swipe_state_t *st = get_state(screen);
    if (!st) return;
    if (st->press_y0 < 0) return;
    int dy = st->press_y0 - st->press_y_last;
    st->press_y0 = -1;
    if (dy >= 30) clock_app_exit();
}

static void on_screen_pressed(lv_event_t *e)
{
    lv_obj_t *screen = lv_event_get_target(e);
    swipe_state_t *st = get_state(screen);
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
    swipe_state_t *st = get_state(screen);
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

    clock_page_id_t cur = clock_app_current_page();
    if (dx < 0 && cur < CLOCK_PAGE_TIMER) {
        clock_app_switch_to((clock_page_id_t)(cur + 1));
    } else if (dx > 0 && cur > CLOCK_PAGE_ALARMS) {
        clock_app_switch_to((clock_page_id_t)(cur - 1));
    }
}

void clk_attach_hit_and_swipe(lv_obj_t *screen)
{
    lv_obj_t *hit = lv_obj_create(screen);
    lv_obj_remove_style_all(hit);
    lv_obj_set_size(hit, 240, CLK_HIT_ZONE_H);
    lv_obj_align(hit, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(hit, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(hit, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(hit, on_hit_pressed,  LV_EVENT_PRESSED,  screen);
    lv_obj_add_event_cb(hit, on_hit_pressing, LV_EVENT_PRESSING, screen);
    lv_obj_add_event_cb(hit, on_hit_released, LV_EVENT_RELEASED, screen);

    /* 让所有 child（除 hit）冒泡到 screen */
    uint32_t cnt = lv_obj_get_child_cnt(screen);
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t *c = lv_obj_get_child(screen, i);
        if (c == hit) continue;
        lv_obj_add_flag(c, LV_OBJ_FLAG_EVENT_BUBBLE);
        uint32_t cc = lv_obj_get_child_cnt(c);
        for (uint32_t j = 0; j < cc; j++) {
            lv_obj_add_flag(lv_obj_get_child(c, j), LV_OBJ_FLAG_EVENT_BUBBLE);
        }
    }

    lv_obj_add_event_cb(screen, on_screen_pressed,  LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(screen, on_screen_released, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(screen, on_state_free,      LV_EVENT_DELETE,   NULL);
    (void)get_state(screen);
}
