#include "system_app.h"
#include "sub_router.h"
#include "esp_log.h"
#include "app_fonts.h"

#include "page_system_pc.h"
#include "page_system_device.h"

static const char *TAG = "system_app";
static sub_router_t *s_router = NULL;
static system_page_id_t s_cur = SYSTEM_PAGE_PC;

static lv_obj_t *on_enter(void)
{
    if (s_router) {
        sub_router_destroy(s_router);
        s_router = NULL;
    }
    s_router = sub_router_create(/*pages=*/ 2, /*history=*/ 4);
    if (!s_router) return NULL;

    sub_router_register(s_router, SYSTEM_PAGE_PC,     page_system_pc_get_callbacks());
    sub_router_register(s_router, SYSTEM_PAGE_DEVICE, page_system_device_get_callbacks());

    s_cur = SYSTEM_PAGE_PC;
    if (sub_router_push(s_router, SYSTEM_PAGE_PC) != ESP_OK) {
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

const app_descriptor_t SYSTEM_APP = {
    .id              = "system",
    .display_name    = "系统",
    .menu_icon       = ICON_TUNE,
    .menu_icon_color = 0x3C3C43,
    .immersive       = false,
    .show_in_menu    = true,
    .on_enter        = on_enter,
    .on_exit         = on_app_exit,
    .on_tick         = on_tick,
};

/* ============================================================================
 * 子页 API
 * ========================================================================= */

void system_app_switch_to(system_page_id_t id)
{
    if (!s_router) return;
    if (id == s_cur) return;
    s_cur = id;
    /* replace：不入栈，避免 pop 历史长度膨胀 */
    sub_router_replace(s_router, (int)id);
}

void system_app_exit(void)
{
    app_router_exit_to_launcher();
}

system_page_id_t system_app_current_page(void)
{
    return s_cur;
}
