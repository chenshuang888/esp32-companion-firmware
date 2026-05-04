#include "music_app.h"
#include "sub_router.h"
#include "app_fonts.h"
#include "esp_log.h"

#include "page_music_list.h"
#include "page_music_detail.h"

static const char *TAG = "music_app";

static sub_router_t *s_router = NULL;

/* ============================================================================
 * App 生命周期（参考 settings_app）
 * ========================================================================= */

static lv_obj_t *on_enter(void)
{
    if (s_router) {
        ESP_LOGW(TAG, "router not cleaned up; destroying");
        sub_router_destroy(s_router);
        s_router = NULL;
    }
    s_router = sub_router_create(/*pages=*/ 2, /*history=*/ 4);
    if (!s_router) return NULL;

    sub_router_register(s_router, MUSIC_PAGE_LIST,   page_music_list_get_callbacks());
    sub_router_register(s_router, MUSIC_PAGE_DETAIL, page_music_detail_get_callbacks());

    if (sub_router_push(s_router, MUSIC_PAGE_LIST) != ESP_OK) {
        sub_router_destroy(s_router);
        s_router = NULL;
        return NULL;
    }
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

const app_descriptor_t MUSIC_APP = {
    .id              = "music",
    .display_name    = "音乐",
    .menu_icon       = ICON_MUSIC,
    .menu_icon_color = 0xAF52DE,
    .immersive       = false,
    .show_in_menu    = true,
    .on_enter        = on_enter,
    .on_exit         = on_app_exit,
    .on_tick         = on_tick,
};

/* ============================================================================
 * 子页导航 API
 * ========================================================================= */

void music_app_push(music_page_id_t id)
{
    if (!s_router) return;
    sub_router_push(s_router, (int)id);
}

void music_app_pop_or_exit(void)
{
    if (!s_router) {
        app_router_exit_to_launcher();
        return;
    }
    if (sub_router_pop(s_router) == ESP_ERR_INVALID_STATE) {
        app_router_exit_to_launcher();
    }
}
