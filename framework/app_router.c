#include "app_router.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "app_router";

#define APP_ROUTER_MAX_APPS  16

typedef struct {
    bool initialized;
    const app_descriptor_t *apps[APP_ROUTER_MAX_APPS];
    uint8_t app_count;

    const app_descriptor_t *current;   /* NULL 表示未进入任何 app */
    lv_obj_t               *current_screen;
} router_state_t;

static router_state_t s_router = {0};

/* ============================================================================
 * 内部
 * ========================================================================= */

static const app_descriptor_t *find_app(const char *id)
{
    if (!id) return NULL;
    for (uint8_t i = 0; i < s_router.app_count; i++) {
        const app_descriptor_t *a = s_router.apps[i];
        if (a && a->id && strcmp(a->id, id) == 0) return a;
    }
    return NULL;
}

/* 销毁当前 app；调用后 current/current_screen 仍由调用方更新。 */
static void destroy_current_locked(void)
{
    if (!s_router.current) return;

    const app_descriptor_t *app = s_router.current;
    if (app->on_exit) {
        ESP_LOGD(TAG, "Calling on_exit for app '%s'", app->id);
        app->on_exit();
    } else if (s_router.current_screen) {
        ESP_LOGD(TAG, "Auto deleting screen for app '%s'", app->id);
        lv_obj_del(s_router.current_screen);
    }
    s_router.current = NULL;
    s_router.current_screen = NULL;
}

/* ============================================================================
 * 公开 API
 * ========================================================================= */

esp_err_t app_router_init(void)
{
    if (s_router.initialized) {
        ESP_LOGW(TAG, "Router already initialized");
        return ESP_OK;
    }
    memset(&s_router, 0, sizeof(s_router));
    s_router.initialized = true;
    ESP_LOGI(TAG, "App router initialized (cap=%d)", APP_ROUTER_MAX_APPS);
    return ESP_OK;
}

esp_err_t app_router_register(const app_descriptor_t *app)
{
    if (!s_router.initialized) {
        ESP_LOGE(TAG, "Router not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (!app || !app->id || !app->id[0]) {
        ESP_LOGE(TAG, "App descriptor invalid");
        return ESP_ERR_INVALID_ARG;
    }
    if (!app->on_enter && !app->on_enter_with_arg) {
        ESP_LOGE(TAG, "App '%s' has neither on_enter nor on_enter_with_arg", app->id);
        return ESP_ERR_INVALID_ARG;
    }
    if (find_app(app->id)) {
        ESP_LOGE(TAG, "App '%s' already registered", app->id);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_router.app_count >= APP_ROUTER_MAX_APPS) {
        ESP_LOGE(TAG, "App registry full");
        return ESP_ERR_NO_MEM;
    }
    s_router.apps[s_router.app_count++] = app;
    ESP_LOGI(TAG, "App '%s' registered (immersive=%d, in_menu=%d)",
             app->id, (int)app->immersive, (int)app->show_in_menu);
    return ESP_OK;
}

static esp_err_t enter_internal(const char *app_id, const char *arg, bool with_arg)
{
    if (!s_router.initialized) return ESP_ERR_INVALID_STATE;

    const app_descriptor_t *app = find_app(app_id);
    if (!app) {
        ESP_LOGE(TAG, "enter: app '%s' not found", app_id ? app_id : "<null>");
        return ESP_ERR_NOT_FOUND;
    }
    if (s_router.current == app) {
        ESP_LOGW(TAG, "Already in app '%s'", app->id);
        return ESP_OK;
    }

    /* 先销毁旧 app */
    destroy_current_locked();

    /* 创建新 screen */
    lv_obj_t *screen = NULL;
    if (with_arg && app->on_enter_with_arg) {
        ESP_LOGI(TAG, "Entering app '%s' with arg '%s'", app->id, arg ? arg : "<null>");
        screen = app->on_enter_with_arg(arg);
    } else if (app->on_enter) {
        ESP_LOGI(TAG, "Entering app '%s'", app->id);
        screen = app->on_enter();
    } else if (app->on_enter_with_arg) {
        /* 无参回退到带参（arg=NULL） */
        screen = app->on_enter_with_arg(NULL);
    }

    if (!screen) {
        ESP_LOGE(TAG, "App '%s' on_enter returned NULL", app->id);
        return ESP_FAIL;
    }

    lv_scr_load(screen);
    s_router.current = app;
    s_router.current_screen = screen;
    ESP_LOGI(TAG, "Entered app '%s'", app->id);
    return ESP_OK;
}

esp_err_t app_router_enter(const char *app_id)
{
    return enter_internal(app_id, NULL, false);
}

esp_err_t app_router_enter_with_arg(const char *app_id, const char *arg)
{
    return enter_internal(app_id, arg, true);
}

esp_err_t app_router_commit_prepared(const char *app_id, lv_obj_t *prepared_screen)
{
    if (!s_router.initialized) return ESP_ERR_INVALID_STATE;

    const app_descriptor_t *app = find_app(app_id);
    if (!app) {
        ESP_LOGE(TAG, "commit_prepared: app '%s' not found", app_id ? app_id : "<null>");
        return ESP_ERR_NOT_FOUND;
    }
    if (!prepared_screen) {
        ESP_LOGE(TAG, "commit_prepared: prepared_screen NULL");
        return ESP_ERR_INVALID_ARG;
    }
    if (s_router.current == app) {
        ESP_LOGW(TAG, "commit_prepared: already in app '%s'", app->id);
        return ESP_ERR_INVALID_STATE;
    }

    destroy_current_locked();

    ESP_LOGI(TAG, "Committing prepared screen for app '%s'", app->id);
    lv_scr_load(prepared_screen);
    s_router.current = app;
    s_router.current_screen = prepared_screen;
    return ESP_OK;
}

const char *app_router_current_id(void)
{
    return s_router.current ? s_router.current->id : NULL;
}

const app_descriptor_t *app_router_current(void)
{
    return s_router.current;
}

void app_router_tick(void)
{
    if (!s_router.initialized || !s_router.current) return;
    if (s_router.current->on_tick) s_router.current->on_tick();
}

uint8_t app_router_get_visible_apps(const app_descriptor_t **out, uint8_t cap)
{
    if (!out || cap == 0) return 0;
    uint8_t n = 0;
    for (uint8_t i = 0; i < s_router.app_count && n < cap; i++) {
        const app_descriptor_t *a = s_router.apps[i];
        if (a && a->show_in_menu) out[n++] = a;
    }
    return n;
}

esp_err_t app_router_exit_to_launcher(void)
{
    return app_router_enter("launcher");
}

esp_err_t app_router_exit_to_lockscreen(void)
{
    return app_router_enter("lockscreen");
}
