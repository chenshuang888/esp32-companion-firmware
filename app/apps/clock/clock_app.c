#include "clock_app.h"
#include "sub_router.h"
#include "esp_log.h"
#include "app_fonts.h"

#include "page_alarms.h"
#include "page_world.h"
#include "page_stopwatch.h"
#include "page_timer.h"
#include "page_alarm_edit.h"

static const char *TAG = "clock_app";

/* alarm_edit 不在 tab 序列里，给个独立 page id */
#define CLOCK_PAGE_ALARM_EDIT  100

static sub_router_t *s_router = NULL;
static clock_page_id_t s_cur = CLOCK_PAGE_ALARMS;

static lv_obj_t *on_enter(void)
{
    if (s_router) {
        sub_router_destroy(s_router);
        s_router = NULL;
    }
    s_router = sub_router_create(/*pages=*/ 5, /*history=*/ 4);
    if (!s_router) return NULL;

    sub_router_register(s_router, CLOCK_PAGE_ALARMS,     page_alarms_get_callbacks());
    sub_router_register(s_router, CLOCK_PAGE_WORLD,      page_world_get_callbacks());
    sub_router_register(s_router, CLOCK_PAGE_STOPWATCH,  page_stopwatch_get_callbacks());
    sub_router_register(s_router, CLOCK_PAGE_TIMER,      page_timer_get_callbacks());
    sub_router_register(s_router, CLOCK_PAGE_ALARM_EDIT, page_alarm_edit_get_callbacks());

    s_cur = CLOCK_PAGE_ALARMS;
    if (sub_router_push(s_router, CLOCK_PAGE_ALARMS) != ESP_OK) {
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

const app_descriptor_t CLOCK_APP = {
    .id              = "clock",
    .display_name    = "时钟",
    .menu_icon       = ICON_SCHEDULE,
    .menu_icon_color = 0xFF9500,
    .immersive       = false,
    .show_in_menu    = true,
    .on_enter        = on_enter,
    .on_exit         = on_app_exit,
    .on_tick         = on_tick,
};

/* ============================================================================
 * 子页 API
 * ========================================================================= */

void clock_app_switch_to(clock_page_id_t id)
{
    if (!s_router) return;
    if (id == s_cur) return;
    s_cur = id;
    sub_router_replace(s_router, (int)id);
}

void clock_app_exit(void)
{
    app_router_exit_to_launcher();
}

clock_page_id_t clock_app_current_page(void)
{
    return s_cur;
}

/* alarms 页内部用：进编辑子页（入栈，能 pop 回） */
void clock_app_open_alarm_edit(void)
{
    if (!s_router) return;
    sub_router_push(s_router, CLOCK_PAGE_ALARM_EDIT);
}

/* edit 页内部用：保存或取消都 pop 回 alarms */
void clock_app_close_alarm_edit(void)
{
    if (!s_router) return;
    if (sub_router_pop(s_router) == ESP_ERR_INVALID_STATE) {
        /* 兜底：栈空说明状态异常，直接退出 app */
        app_router_exit_to_launcher();
    }
}
