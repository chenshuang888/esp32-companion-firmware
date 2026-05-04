#include "page_dynapp_host.h"

#include <string.h>

#include "app_fonts.h"
#include "app_router.h"
#include "app_shell_ui.h"
#include "ui_tokens.h"
#include "ui_widgets.h"
#include "dynamic_app.h"
#include "dynamic_app_ui.h"
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "dynapp_host";

/* ============================================================================
 * 动态 App 宿主：
 *
 * 路径 A）后台 prepare + 瞬切（推荐，launcher 用）：
 *   dynapp_host_prepare_and_enter("calc")
 *   → off-screen build subtree + 起脚本
 *   → script ready / 800ms 超时 → app_router_commit_prepared("dynapp_host", screen)
 *
 * 路径 B）同步 enter（兜底）：
 *   dynapp_host_set_pending("calc"); app_router_enter("dynapp_host")
 *   → on_enter 同步 build + start，先切屏后 build（用户看到组件冒出来）
 *
 * 退出：on_exit → 关 root + 停脚本 + 删 screen
 * 返回 launcher：屏内"返回"按钮
 * ========================================================================= */

#define PREPARE_TIMEOUT_MS 800

typedef enum {
    PREP_IDLE = 0,
    PREP_PREPARING,
    PREP_COMMITTED,
} prep_state_t;

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *statusbar;       /* 24px 顶部状态栏（电池/蓝牙/时间） */
    lv_obj_t *list_root;       /* 动态 app UI 挂载点（screen 下移 24px） */
    lv_obj_t *hit_zone;        /* 屏底 30px 上滑退出区（统一规则） */

    /* 上滑退出：自算 dy（参考 page_notifications 的做法） */
    int       press_y0;
    int       press_y_last;

    prep_state_t  state;
    lv_timer_t   *timeout_timer;
} ui_t;

static ui_t s_ui = {0};
static char s_pending_app[16] = "";

/* 前向 */
static void build_screen_subtree(void);
static void teardown_subtree(void);
static void on_ready_cb(void *ud);
static void on_timeout_cb(lv_timer_t *t);
static void commit_now(const char *reason);

/* ============================================================================
 * 公开 API
 * ========================================================================= */

void dynapp_host_set_pending(const char *app_name)
{
    if (!app_name || !app_name[0]) return;
    strncpy(s_pending_app, app_name, sizeof(s_pending_app) - 1);
    s_pending_app[sizeof(s_pending_app) - 1] = '\0';
}

void dynapp_host_cancel_prepare_if_any(void)
{
    if (s_ui.state != PREP_PREPARING) return;

    ESP_LOGI(TAG, "cancel prepare for app: %s", s_pending_app);

    dynamic_app_ui_set_root(NULL);
    dynamic_app_stop();
    dynamic_app_ui_unregister_all();

    if (s_ui.timeout_timer) {
        lv_timer_del(s_ui.timeout_timer);
        s_ui.timeout_timer = NULL;
    }

    if (s_ui.screen) {
        lv_obj_del(s_ui.screen);
    }
    s_ui.screen     = NULL;
    s_ui.statusbar  = NULL;
    s_ui.list_root  = NULL;
    s_ui.hit_zone   = NULL;

    s_ui.state = PREP_IDLE;
}

void dynapp_host_prepare_and_enter(const char *app_name)
{
    if (!app_name || !app_name[0]) return;

    if (s_ui.state == PREP_PREPARING) dynapp_host_cancel_prepare_if_any();
    if (s_ui.state == PREP_COMMITTED) {
        ESP_LOGW(TAG, "already on dynapp_host, ignore prepare for %s", app_name);
        return;
    }

    dynapp_host_set_pending(app_name);
    ESP_LOGI(TAG, "prepare app in background: %s", s_pending_app);

    build_screen_subtree();

    dynamic_app_ui_set_fonts(APP_FONT_TEXT, APP_FONT_TITLE, APP_FONT_HUGE,
                             APP_FONT_ICONS_24, APP_FONT_ICONS_36, APP_FONT_LARGE);
    dynamic_app_ui_set_root(s_ui.list_root);

    dynamic_app_ui_set_ready_cb(on_ready_cb, NULL);
    s_ui.timeout_timer = lv_timer_create(on_timeout_cb, PREPARE_TIMEOUT_MS, NULL);
    lv_timer_set_repeat_count(s_ui.timeout_timer, 1);

    s_ui.state = PREP_PREPARING;
    dynamic_app_start(s_pending_app);
}

/* ============================================================================
 * 内部
 * ========================================================================= */

#define HIT_ZONE_H        28      /* 屏底退出 hit zone 高度（pixel） */
#define UPSWIPE_THRESHOLD 30      /* 累计上滑 ≥30px 视为退出意图 */

static void on_pressed_hit(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) { s_ui.press_y0 = -1; return; }
    lv_point_t p; lv_indev_get_point(indev, &p);
    s_ui.press_y0     = p.y;
    s_ui.press_y_last = p.y;
}

static void on_pressing_hit(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p; lv_indev_get_point(indev, &p);
    s_ui.press_y_last = p.y;
}

static void on_released_hit(lv_event_t *e)
{
    (void)e;
    if (s_ui.press_y0 < 0) return;
    int dy = s_ui.press_y0 - s_ui.press_y_last;
    s_ui.press_y0 = -1;
    if (dy >= UPSWIPE_THRESHOLD) {
        ESP_LOGI(TAG, "upswipe detected (dy=%d), exit to launcher", dy);
        app_router_exit_to_launcher();
    }
}

static void build_screen_subtree(void)
{
    s_ui.screen = lv_obj_create(NULL);
    ui_screen_setup(s_ui.screen);   /* iOS 浅色 */
    s_ui.press_y0 = -1;

    /* 顶部 24px 状态栏（dark=false 浅色页文字偏黑）。
     * 默认 dark=false；若动态 app 自己用深底，状态栏文字会和底色冲突——
     * 当前所有 prelude UI.* 都基于浅色 token，保持 false 即可。 */
    s_ui.statusbar = app_shell_attach_statusbar(s_ui.screen, false);

    /* 动态 app 的挂载点：从 24px 起到屏底（保留 28px hit zone）。
     * 整屏 240×320，可用区 = 320-24-28 = 268px 高。 */
    s_ui.list_root = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(s_ui.list_root);
    lv_obj_set_size(s_ui.list_root, 240, 320 - 24 - HIT_ZONE_H);
    lv_obj_align(s_ui.list_root, LV_ALIGN_TOP_LEFT, 0, 24);
    lv_obj_set_style_pad_all(s_ui.list_root, 0, 0);
    lv_obj_set_style_bg_opa(s_ui.list_root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ui.list_root, 0, 0);
    lv_obj_clear_flag(s_ui.list_root, LV_OBJ_FLAG_SCROLLABLE);

    /* 屏底 28px hit zone：透明，不可点（CLICKABLE 才能拿到 PRESSED/RELEASED）。
     * 这是宿主层提供的"统一上滑退出"——所有动态 app 自动获得，无需 JS 处理。
     * z-order 在 list_root 之后，确保覆盖在内容之上。 */
    s_ui.hit_zone = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(s_ui.hit_zone);
    lv_obj_set_size(s_ui.hit_zone, 240, HIT_ZONE_H);
    lv_obj_align(s_ui.hit_zone, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(s_ui.hit_zone, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ui.hit_zone, 0, 0);
    lv_obj_clear_flag(s_ui.hit_zone, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ui.hit_zone, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_ui.hit_zone, on_pressed_hit,  LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(s_ui.hit_zone, on_pressing_hit, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_ui.hit_zone, on_released_hit, LV_EVENT_RELEASED, NULL);
}

static void teardown_subtree(void)
{
    if (s_ui.screen) {
        lv_obj_del(s_ui.screen);
        s_ui.screen = NULL;
    }
    s_ui.statusbar = NULL;
    s_ui.list_root = NULL;
    s_ui.hit_zone  = NULL;
    s_ui.press_y0  = -1;
}

static void on_ready_cb(void *ud)
{
    (void)ud;
    if (s_ui.state != PREP_PREPARING) return;
    commit_now("ready");
}

static void on_timeout_cb(lv_timer_t *t)
{
    (void)t;
    if (s_ui.state != PREP_PREPARING) return;
    ESP_LOGW(TAG, "prepare timeout (%dms) for app: %s, force commit",
             PREPARE_TIMEOUT_MS, s_pending_app);
    commit_now("timeout");
}

static void commit_now(const char *reason)
{
    if (s_ui.timeout_timer) {
        lv_timer_del(s_ui.timeout_timer);
        s_ui.timeout_timer = NULL;
    }
    dynamic_app_ui_set_ready_cb(NULL, NULL);

    lv_obj_t *prepared = s_ui.screen;

    esp_err_t err = app_router_commit_prepared("dynapp_host", prepared);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "commit_prepared failed err=0x%x, rolling back", err);
        dynamic_app_ui_set_root(NULL);
        dynamic_app_stop();
        dynamic_app_ui_unregister_all();
        teardown_subtree();
        s_ui.state = PREP_IDLE;
        return;
    }

    s_ui.state = PREP_COMMITTED;
    ESP_LOGI(TAG, "committed dynamic app=%s reason=%s", s_pending_app, reason);
}

/* ============================================================================
 * page_callbacks_t —— 给 dynapp_host_app 壳调
 * ========================================================================= */

static lv_obj_t *create(void)
{
    /* 同步路径兜底（路径 B）。新代码应走 prepare_and_enter（commit 路径）。 */
    if (s_ui.state == PREP_PREPARING || s_ui.state == PREP_COMMITTED) {
        ESP_LOGW(TAG, "create() called while state=%d, returning current screen", s_ui.state);
        s_ui.state = PREP_COMMITTED;
        return s_ui.screen;
    }

    ESP_LOGI(TAG, "Creating dynapp_host (sync path) for: %s", s_pending_app);

    build_screen_subtree();

    dynamic_app_ui_set_fonts(APP_FONT_TEXT, APP_FONT_TITLE, APP_FONT_HUGE,
                             APP_FONT_ICONS_24, APP_FONT_ICONS_36, APP_FONT_LARGE);
    dynamic_app_ui_set_root(s_ui.list_root);

    s_ui.state = PREP_COMMITTED;
    dynamic_app_start(s_pending_app);

    return s_ui.screen;
}

static void destroy(void)
{
    ESP_LOGI(TAG, "Destroying dynapp_host");

    dynamic_app_ui_set_root(NULL);
    dynamic_app_stop();
    dynamic_app_ui_unregister_all();

    if (s_ui.timeout_timer) {
        lv_timer_del(s_ui.timeout_timer);
        s_ui.timeout_timer = NULL;
    }

    teardown_subtree();
    s_ui.state = PREP_IDLE;
}

static const page_callbacks_t s_callbacks = {
    .create  = create,
    .destroy = destroy,
    .update  = NULL,
};

const page_callbacks_t *page_dynapp_host_get_callbacks(void)
{
    return &s_callbacks;
}
