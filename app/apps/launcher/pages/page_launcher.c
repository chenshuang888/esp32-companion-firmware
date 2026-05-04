#include "page_launcher.h"
#include "launcher_modal.h"
#include "page_dynapp_host.h"

#include "esp_log.h"
#include "lvgl.h"

#include "ui_tokens.h"
#include "ui_widgets.h"
#include "ui_anim.h"
#include "ui_statusbar.h"
#include "app_shell_ui.h"
#include "app_fonts.h"
#include "app_router.h"

#include "ble_driver.h"
#include "dynamic_app_registry.h"
#include "dynapp_upload_manager.h"
#include "dynapp_fs_worker.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

static const char *TAG = "page_launcher";

/* ============================================================================
 * Launcher 主页：3×3 九宫格 + 翻页
 *
 * cell 来源：
 *   静态 app  → app_router_get_visible_apps()（show_in_menu=true 的所有 app）
 *   动态 app  → dynamic_app_registry_list()（JS app 列表）
 *
 * 行为：
 *   静态 app cell → app_router_enter(app->id)
 *   动态 app cell → dynapp_host_prepare_and_enter(name)
 * ========================================================================= */

#define CELLS_PER_PAGE   9
#define MAX_DYN_APPS     16
#define MAX_STATIC_APPS  12
#define MAX_PAGES        4
#define MAX_TOTAL_CELLS  (CELLS_PER_PAGE * MAX_PAGES)

typedef enum {
    CELL_STATIC_APP,    /* 进 app_router_enter(app_id) */
    CELL_DYNAPP,        /* dynapp_host_prepare_and_enter(name) */
} cell_kind_t;

typedef struct {
    cell_kind_t   kind;
    const char   *app_id;       /* CELL_STATIC_APP 时 = app->id（指向 app_descriptor 持有的常量） */
    const char   *icon;
    const char   *label;
    lv_color_t    color;
    char         *dyn_name;     /* CELL_DYNAPP 时 strdup */
    char         *dyn_label;    /* CELL_DYNAPP 时 strdup 的 manifest.name，c->label 指向它；
                                 * 静态 app 的 label 来自 app_descriptor 静态区，无需拷贝 */
    char          dyn_icon[8];  /* CELL_DYNAPP 时拷 manifest icon UTF-8 codepoint，
                                 * 与 c->icon 共享语义；这里独立缓冲以便随 cell 一起释放 */
} cell_def_t;

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *statusbar;
    lv_obj_t *pager;
    lv_obj_t *dots_box;

    cell_def_t cells[MAX_TOTAL_CELLS];
    int        cell_count;
    int        page_count;
    int        active_page;
} ui_t;

static ui_t s_ui = {0};
static volatile bool s_dirty = false;

/* 前向 */
static void on_cell_clicked(lv_event_t *e);
static void on_cell_long_pressed(lv_event_t *e);
static void on_pager_scroll(lv_event_t *e);
static void rebuild_cells(void);
static void rebuild_layout(void);
static void update_dots(void);

/* ============================================================================
 * 行为
 * ========================================================================= */

static void on_cell_clicked(lv_event_t *e)
{
    cell_def_t *c = (cell_def_t *)lv_event_get_user_data(e);
    if (!c) return;

    switch (c->kind) {
    case CELL_STATIC_APP:
        dynapp_host_cancel_prepare_if_any();
        app_router_enter(c->app_id);
        break;
    case CELL_DYNAPP:
        if (c->dyn_name) dynapp_host_prepare_and_enter(c->dyn_name);
        break;
    }
}

static void on_local_delete_done(esp_err_t result, void *cb_arg)
{
    (void)cb_arg;
    if (result == ESP_OK || result == ESP_ERR_NOT_FOUND) {
        s_dirty = true;
    } else {
        ESP_LOGW(TAG, "local delete failed: 0x%x", result);
    }
}

static void on_delete_confirmed(void *ud)
{
    const char *name = (const char *)ud;
    if (!name) return;
    if (!dynapp_fs_worker_submit_app_delete(name, on_local_delete_done, NULL)) {
        ESP_LOGW(TAG, "fs_worker queue full, can't delete: %s", name);
    }
}

static void on_cell_long_pressed(lv_event_t *e)
{
    cell_def_t *c = (cell_def_t *)lv_event_get_user_data(e);
    if (!c || c->kind != CELL_DYNAPP || !c->dyn_name) return;

    lv_indev_t *indev = lv_event_get_indev(e);
    if (indev) lv_indev_wait_release(indev);

    launcher_modal_show_delete_confirm(c->dyn_name, on_delete_confirmed, c->dyn_name);
}

/* ============================================================================
 * cell 收集
 * ========================================================================= */

static void cells_clear(void)
{
    for (int i = 0; i < s_ui.cell_count; i++) {
        if (s_ui.cells[i].dyn_name) {
            free(s_ui.cells[i].dyn_name);
            s_ui.cells[i].dyn_name = NULL;
        }
        if (s_ui.cells[i].dyn_label) {
            free(s_ui.cells[i].dyn_label);
            s_ui.cells[i].dyn_label = NULL;
        }
    }
    memset(s_ui.cells, 0, sizeof(s_ui.cells));
    s_ui.cell_count = 0;
}

static void cells_collect(void)
{
    cells_clear();

    /* ---- 静态 app（从 app_router 枚举）---- */
    const app_descriptor_t *apps[MAX_STATIC_APPS];
    uint8_t n_static = app_router_get_visible_apps(apps, MAX_STATIC_APPS);
    for (uint8_t i = 0; i < n_static && s_ui.cell_count < MAX_TOTAL_CELLS; i++) {
        const app_descriptor_t *a = apps[i];
        if (!a->menu_icon) continue;   /* 没图标的不画 */
        cell_def_t *c = &s_ui.cells[s_ui.cell_count++];
        c->kind     = CELL_STATIC_APP;
        c->app_id   = a->id;
        c->icon     = a->menu_icon;
        c->label    = a->display_name ? a->display_name : a->id;
        c->color    = lv_color_hex(a->menu_icon_color);
        c->dyn_name = NULL;
    }

    /* ---- 动态 app（从 dynamic_app_registry）---- */
    dynamic_app_entry_t entries[MAX_DYN_APPS];
    int n = dynamic_app_registry_list(entries, MAX_DYN_APPS);
    ESP_LOGI(TAG, "dynamic apps discovered: %d (static: %d)", n, n_static);
    for (int i = 0; i < n && s_ui.cell_count < MAX_TOTAL_CELLS; i++) {
        cell_def_t *c = &s_ui.cells[s_ui.cell_count++];
        c->kind     = CELL_DYNAPP;
        c->app_id   = NULL;
        /* entries 是栈上局部变量，display 字段必须拷贝；否则函数退出后
         * cell.label 会指向被覆盖的栈内存，launcher 一进二出就显示乱码。 */
        c->dyn_label = strdup(entries[i].display);
        c->label    = c->dyn_label;
        c->dyn_name = strdup(entries[i].id);

        /* manifest 指定了 icon → 拷 UTF-8 字节走字体路径；否则回退 ICON_APPS。
         * 与原生 app 完全一致：lv_label + 36px Material Symbols 字体。 */
        if (entries[i].icon[0]) {
            strncpy(c->dyn_icon, entries[i].icon, sizeof(c->dyn_icon) - 1);
            c->dyn_icon[sizeof(c->dyn_icon) - 1] = '\0';
            c->icon = c->dyn_icon;
        } else {
            c->icon = ICON_APPS;
        }
        /* manifest.iconColor 缺失（=0）回退中性灰，与"通用 app fallback"配色一致 */
        c->color = lv_color_hex(entries[i].icon_color
                                ? entries[i].icon_color : 0x6E6E73);
    }

    s_ui.page_count = (s_ui.cell_count + CELLS_PER_PAGE - 1) / CELLS_PER_PAGE;
    if (s_ui.page_count < 1) s_ui.page_count = 1;
}

/* ============================================================================
 * View（cell + page + dots —— 与原 page_menu 等价）
 * ========================================================================= */

static lv_obj_t *create_cell_obj(lv_obj_t *parent, cell_def_t *c)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 80, 88);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(btn, UI_R_MD, 0);
    lv_obj_set_style_bg_color(btn, UI_C_PANEL_HI, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa  (btn, LV_OPA_60,    LV_STATE_PRESSED);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(btn, UI_SP_XS, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);

    /* 静态 + 动态 app 走同一渲染路径：36px 字体图标 + iOS 浅色文字 */
    lv_obj_t *icon = lv_label_create(btn);
    lv_label_set_text(icon, c->icon);
    lv_obj_set_style_text_font (icon, APP_FONT_ICONS_36, 0);
    lv_obj_set_style_text_color(icon, c->color, 0);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, c->label);
    lv_obj_set_style_text_font (lbl, UI_F_LABEL, 0);
    lv_obj_set_style_text_color(lbl, UI_C_TEXT, 0);

    lv_obj_add_event_cb(btn, on_cell_clicked,      LV_EVENT_CLICKED,      c);
    lv_obj_add_event_cb(btn, on_cell_long_pressed, LV_EVENT_LONG_PRESSED, c);
    return btn;
}

static lv_obj_t *create_page_obj(lv_obj_t *parent, int page_idx)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, 240, lv_pct(100));
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);

    static int32_t col_dsc[] = { 80, 80, 80, LV_GRID_TEMPLATE_LAST };
    static int32_t row_dsc[] = { 88, 88, 88, LV_GRID_TEMPLATE_LAST };
    lv_obj_set_grid_dsc_array(page, col_dsc, row_dsc);
    lv_obj_set_layout(page, LV_LAYOUT_GRID);

    int start = page_idx * CELLS_PER_PAGE;
    int end   = start + CELLS_PER_PAGE;
    if (end > s_ui.cell_count) end = s_ui.cell_count;

    for (int i = start; i < end; i++) {
        int slot = i - start;
        lv_obj_t *cell = create_cell_obj(page, &s_ui.cells[i]);
        lv_obj_set_grid_cell(cell,
            LV_GRID_ALIGN_CENTER, slot % 3, 1,
            LV_GRID_ALIGN_CENTER, slot / 3, 1);
    }
    return page;
}

static void update_dots(void)
{
    if (!s_ui.dots_box) return;
    uint32_t n = lv_obj_get_child_count(s_ui.dots_box);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *dot = lv_obj_get_child(s_ui.dots_box, i);
        bool active = ((int)i == s_ui.active_page);
        lv_obj_set_style_bg_color(dot, active ? UI_C_TEXT : UI_C_BORDER, 0);
    }
}

static void create_dots(lv_obj_t *parent)
{
    s_ui.dots_box = lv_obj_create(parent);
    lv_obj_remove_style_all(s_ui.dots_box);
    lv_obj_set_size(s_ui.dots_box, 240, 16);
    lv_obj_align(s_ui.dots_box, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(s_ui.dots_box, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_ui.dots_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_ui.dots_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_ui.dots_box,
        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_ui.dots_box, 6, 0);

    for (int i = 0; i < s_ui.page_count; i++) {
        lv_obj_t *dot = lv_obj_create(s_ui.dots_box);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 6, 6);
        lv_obj_set_style_radius(dot, 1000, 0);
        lv_obj_set_style_bg_color(dot, UI_C_BORDER, 0);
        lv_obj_set_style_bg_opa  (dot, LV_OPA_COVER, 0);
    }
    update_dots();
}

static void on_pager_scroll(lv_event_t *e)
{
    (void)e;
    int32_t scroll_x = lv_obj_get_scroll_x(s_ui.pager);
    int new_page = (scroll_x + 120) / 240;
    if (new_page < 0) new_page = 0;
    if (new_page >= s_ui.page_count) new_page = s_ui.page_count - 1;
    if (new_page != s_ui.active_page) {
        s_ui.active_page = new_page;
        update_dots();
    }
}

static void rebuild_layout(void)
{
    if (s_ui.pager)    { lv_obj_del(s_ui.pager);    s_ui.pager = NULL; }
    if (s_ui.dots_box) { lv_obj_del(s_ui.dots_box); s_ui.dots_box = NULL; }

    s_ui.pager = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(s_ui.pager);
    lv_obj_set_size(s_ui.pager, 240, 280);
    lv_obj_align(s_ui.pager, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_style_bg_opa(s_ui.pager, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_ui.pager, 0, 0);
    lv_obj_set_flex_flow(s_ui.pager, LV_FLEX_FLOW_ROW);
    lv_obj_set_scroll_dir(s_ui.pager, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(s_ui.pager, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(s_ui.pager, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(s_ui.pager, on_pager_scroll, LV_EVENT_SCROLL, NULL);

    for (int i = 0; i < s_ui.page_count; i++) {
        create_page_obj(s_ui.pager, i);
    }

    if (s_ui.active_page >= s_ui.page_count) s_ui.active_page = s_ui.page_count - 1;
    if (s_ui.active_page < 0) s_ui.active_page = 0;
    lv_obj_scroll_to_x(s_ui.pager, s_ui.active_page * 240, LV_ANIM_OFF);

    create_dots(s_ui.screen);
}

static void rebuild_cells(void)
{
    cells_collect();
    rebuild_layout();
}

/* ============================================================================
 * upload 观察者 —— launcher_app 启动时一次性注册（manager 自身去重），
 * 这里只声明回调；注册在 launcher_app.c 里
 * ========================================================================= */

void page_launcher_mark_dirty(void);
void page_launcher_mark_dirty(void) { s_dirty = true; }

/* ============================================================================
 * 手势：下滑回锁屏
 * ========================================================================= */

static void on_screen_gesture(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_BOTTOM) {
        dynapp_host_cancel_prepare_if_any();
        app_router_exit_to_lockscreen();
    }
}

/* ============================================================================
 * 生命周期
 * ========================================================================= */

static lv_obj_t *create(void)
{
    ESP_LOGI(TAG, "create");

    s_ui.screen = lv_obj_create(NULL);
    ui_screen_setup(s_ui.screen);

    lv_obj_add_event_cb(s_ui.screen, on_screen_gesture, LV_EVENT_GESTURE, NULL);

    s_ui.statusbar = app_shell_attach_statusbar(s_ui.screen, false);

    s_ui.active_page = 0;
    rebuild_cells();

    s_dirty = false;
    return s_ui.screen;
}

static void destroy(void)
{
    ESP_LOGI(TAG, "destroy");
    /* launcher 离开 → 兜底关掉 modal（避免 layer_top 残留）*/
    launcher_modal_dismiss();

    cells_clear();

    if (s_ui.screen) {
        lv_obj_del(s_ui.screen);
        s_ui.screen = NULL;
    }
    s_ui.statusbar = NULL;
    s_ui.pager     = NULL;
    s_ui.dots_box  = NULL;
}

static void update_cb(void)
{
    if (s_dirty) {
        s_dirty = false;
        rebuild_cells();
    }
}

static const page_callbacks_t s_callbacks = {
    .create  = create,
    .destroy = destroy,
    .update  = update_cb,
};

const page_callbacks_t *page_launcher_get_callbacks(void)
{
    return &s_callbacks;
}
