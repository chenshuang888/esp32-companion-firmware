#include "lockscreen_app.h"
#include "page_lockscreen.h"

/* 锁屏 = 单 page app；on_enter / on_app_exit / on_tick 直接转发到 page 回调 */

static lv_obj_t *on_enter(void)
{
    return page_lockscreen_get_callbacks()->create();
}

static void on_app_exit(void)
{
    const page_callbacks_t *cb = page_lockscreen_get_callbacks();
    if (cb->destroy) cb->destroy();
}

static void on_tick(void)
{
    const page_callbacks_t *cb = page_lockscreen_get_callbacks();
    if (cb->update) cb->update();
}

const app_descriptor_t LOCKSCREEN_APP = {
    .id              = "lockscreen",
    .display_name    = NULL,
    .menu_icon       = NULL,
    .menu_icon_color = 0,
    .immersive       = true,
    .show_in_menu    = false,
    .on_enter        = on_enter,
    .on_exit         = on_app_exit,
    .on_tick         = on_tick,
};
