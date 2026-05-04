#include "settings_app.h"
#include "app_router.h"
#include "sub_router.h"
#include "esp_log.h"

#include "settings_home.h"
#include "settings_bluetooth.h"
#include "settings_display.h"
#include "settings_time.h"
#include "settings_about.h"

static const char *TAG = "settings_app";

static sub_router_t *s_router = NULL;

/* ============================================================================
 * App 生命周期
 * ========================================================================= */

static lv_obj_t *on_enter(void)
{
    if (s_router) {
        ESP_LOGW(TAG, "router not cleaned up; destroying");
        sub_router_destroy(s_router);
        s_router = NULL;
    }
    s_router = sub_router_create(/*pages=*/ 5, /*history=*/ 4);
    if (!s_router) return NULL;

    sub_router_register(s_router, SETTINGS_PAGE_HOME,      settings_home_get_callbacks());
    sub_router_register(s_router, SETTINGS_PAGE_BLUETOOTH, settings_bluetooth_get_callbacks());
    sub_router_register(s_router, SETTINGS_PAGE_DISPLAY,   settings_display_get_callbacks());
    sub_router_register(s_router, SETTINGS_PAGE_TIME,      settings_time_get_callbacks());
    sub_router_register(s_router, SETTINGS_PAGE_ABOUT,     settings_about_get_callbacks());

    /* sub_router_push 内部会 lv_scr_load 当前 home screen */
    if (sub_router_push(s_router, SETTINGS_PAGE_HOME) != ESP_OK) {
        sub_router_destroy(s_router);
        s_router = NULL;
        return NULL;
    }
    /* 返回当前 active screen 给 app_router 接管显示 */
    return lv_scr_act();
}

static void on_app_exit(void)
{
    if (s_router) {
        sub_router_destroy(s_router);
        s_router = NULL;
    }
}

static void on_tick(void)
{
    if (s_router) sub_router_tick(s_router);
}

const app_descriptor_t SETTINGS_APP = {
    .id              = "settings",
    .display_name    = "设置",
    .menu_icon       = "\xEE\xA2\xB8",       /* ICON_SETTINGS（齿轮）*/
    .menu_icon_color = 0x6E6E73,
    .immersive       = false,
    .show_in_menu    = true,
    .on_enter        = on_enter,
    .on_exit         = on_app_exit,
    .on_tick         = on_tick,
};

/* ============================================================================
 * 子页导航 API
 * ========================================================================= */

void settings_app_push(settings_page_id_t id)
{
    if (!s_router) return;
    sub_router_push(s_router, (int)id);
}

void settings_app_pop_or_exit(void)
{
    if (!s_router) {
        app_router_exit_to_launcher();
        return;
    }
    if (sub_router_pop(s_router) == ESP_ERR_INVALID_STATE) {
        /* 栈空 = 已在 home → 退出 app */
        app_router_exit_to_launcher();
    }
}
