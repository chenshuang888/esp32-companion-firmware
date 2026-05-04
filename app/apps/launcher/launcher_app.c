#include "launcher_app.h"
#include "page_launcher.h"
#include "dynapp_upload_manager.h"

/* 由 page_launcher 暴露的"标脏 → 下一帧重建"接口 */
void page_launcher_mark_dirty(void);

/* upload 观察者：在 launcher_app_module_init 注册一次。
 * 不在 launcher on_enter 注册：避免重建 launcher screen 时重复注册（manager 容量 4）。 */
static void on_upload_status(upload_op_t op, upload_result_t result,
                             uint8_t seq, const char *name, uint32_t extra,
                             const uint8_t *list_buf, size_t list_len)
{
    (void)seq; (void)name; (void)extra; (void)list_buf; (void)list_len;
    if (result != UPL_RESULT_OK) return;
    if (op == UPL_OP_END || op == UPL_OP_DELETE) {
        page_launcher_mark_dirty();
    }
}

void launcher_app_module_init(void)
{
    (void)dynapp_upload_manager_register_status_cb(on_upload_status);
}

static lv_obj_t *on_enter(void) { return page_launcher_get_callbacks()->create(); }
static void on_app_exit(void)       { const page_callbacks_t *cb = page_launcher_get_callbacks(); if (cb->destroy) cb->destroy(); }
static void on_tick(void)       { const page_callbacks_t *cb = page_launcher_get_callbacks(); if (cb->update) cb->update(); }

const app_descriptor_t LAUNCHER_APP = {
    .id              = "launcher",
    .display_name    = NULL,
    .menu_icon       = NULL,
    .menu_icon_color = 0,
    .immersive       = true,    /* launcher 自管全屏（统计栏自挂） */
    .show_in_menu    = false,
    .on_enter        = on_enter,
    .on_exit         = on_app_exit,
    .on_tick         = on_tick,
};
