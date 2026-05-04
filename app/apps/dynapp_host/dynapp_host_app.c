#include "dynapp_host_app.h"
#include "page_dynapp_host.h"

/* dynapp_host = 沉浸式容器 app；不在 launcher 显示（动态 app 由 launcher 单独枚举注册表） */

static lv_obj_t *on_enter(void)                  { return page_dynapp_host_get_callbacks()->create(); }
static lv_obj_t *on_enter_with_arg(const char *a){ if (a) dynapp_host_set_pending(a); return page_dynapp_host_get_callbacks()->create(); }
static void on_app_exit(void)                        { const page_callbacks_t *cb = page_dynapp_host_get_callbacks(); if (cb->destroy) cb->destroy(); }

const app_descriptor_t DYNAPP_HOST_APP = {
    .id                 = "dynapp_host",
    .display_name       = NULL,
    .menu_icon          = NULL,
    .menu_icon_color    = 0,
    .immersive          = true,
    .show_in_menu       = false,
    .on_enter           = on_enter,
    .on_enter_with_arg  = on_enter_with_arg,
    .on_exit            = on_app_exit,
    .on_tick            = NULL,
};
