#include "page_system_pc.h"
#include "system_app.h"
#include "system_ui_common.h"
#include "app_shell_ui.h"
#include "app_router.h"

#include "system_manager.h"
#include "system_service.h"

#include "esp_log.h"
#include "lvgl.h"
#include "app_fonts.h"
#include "ui_tokens.h"
#include "ui_widgets.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "page_sys_pc";

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *content;       /* y=52~290 内容区 */

    sys_gauge_t cpu, mem, disk;
    lv_obj_t *bat_lbl;
    lv_obj_t *temp_lbl;
    lv_obj_t *dn_lbl;
    lv_obj_t *up_lbl;
    lv_obj_t *uptime_lbl;

    uint32_t last_epoch;
} ui_t;

static ui_t s_ui;

static const lv_color_t WARN_COLOR = LV_COLOR_MAKE(0xFF, 0x95, 0x00);

static void refresh(void)
{
    const system_payload_t *s = system_manager_get_latest();
    if (!s) {
        lv_label_set_text(s_ui.cpu.num_lbl,  "--");
        lv_label_set_text(s_ui.mem.num_lbl,  "--");
        lv_label_set_text(s_ui.disk.num_lbl, "--");
        return;
    }

    sys_apply_gauge(&s_ui.cpu,  s->cpu_percent,  UI_C_ACCENT,   WARN_COLOR, 80);
    sys_apply_gauge(&s_ui.mem,  s->mem_percent,  UI_C_ACCENT_2, WARN_COLOR, 80);
    sys_apply_gauge(&s_ui.disk, s->disk_percent, UI_C_ACCENT,   WARN_COLOR, 80);

    char buf[48];

    if (s->battery_percent == SYSTEM_BATTERY_ABSENT) {
        lv_label_set_text(s_ui.bat_lbl, "N/A");
    } else {
        snprintf(buf, sizeof(buf), "%u%%", s->battery_percent);
        lv_label_set_text(s_ui.bat_lbl, buf);
    }

    if (s->cpu_temp_cx10 == SYSTEM_CPU_TEMP_INVALID) {
        lv_label_set_text(s_ui.temp_lbl, "N/A");
    } else {
        int t = s->cpu_temp_cx10;
        snprintf(buf, sizeof(buf), "%d.%d°C", t / 10, (t < 0 ? -t : t) % 10);
        lv_label_set_text(s_ui.temp_lbl, buf);
    }

    if (s->net_down_kbps >= 1024) {
        snprintf(buf, sizeof(buf), "%u.%u MB/s",
                 s->net_down_kbps / 1024, (s->net_down_kbps % 1024) * 10 / 1024);
    } else {
        snprintf(buf, sizeof(buf), "%u KB/s", s->net_down_kbps);
    }
    lv_label_set_text(s_ui.dn_lbl, buf);

    if (s->net_up_kbps >= 1024) {
        snprintf(buf, sizeof(buf), "%u.%u MB/s",
                 s->net_up_kbps / 1024, (s->net_up_kbps % 1024) * 10 / 1024);
    } else {
        snprintf(buf, sizeof(buf), "%u KB/s", s->net_up_kbps);
    }
    lv_label_set_text(s_ui.up_lbl, buf);

    uint32_t sec = s->uptime_sec;
    uint32_t d = sec / 86400, h = (sec % 86400) / 3600, m = (sec % 3600) / 60;
    if (d) snprintf(buf, sizeof(buf), "%ud %uh", (unsigned)d, (unsigned)h);
    else if (h) snprintf(buf, sizeof(buf), "%uh %um", (unsigned)h, (unsigned)m);
    else snprintf(buf, sizeof(buf), "%um", (unsigned)m);
    lv_label_set_text(s_ui.uptime_lbl, buf);
}

static lv_obj_t *create(void)
{
    ESP_LOGI(TAG, "create");
    memset(&s_ui, 0, sizeof(s_ui));

    s_ui.screen = lv_obj_create(NULL);
    ui_screen_setup(s_ui.screen);
    lv_obj_clear_flag(s_ui.screen, LV_OBJ_FLAG_SCROLLABLE);

    app_shell_attach_statusbar(s_ui.screen, false);
    sys_make_tabbar(s_ui.screen);

    /* 内容容器 */
    s_ui.content = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(s_ui.content);
    lv_obj_set_size(s_ui.content, 240, 320 - SYS_CONTENT_TOP - SYS_HIT_ZONE_H);
    lv_obj_set_pos(s_ui.content, 0, SYS_CONTENT_TOP);
    lv_obj_set_style_bg_opa(s_ui.content, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_ui.content, LV_OBJ_FLAG_SCROLLABLE);

    /* 三圆盘 */
    s_ui.cpu  = sys_make_gauge(s_ui.content, SYS_GAUGE_X[0], 4, UI_C_ACCENT,   "%");
    s_ui.mem  = sys_make_gauge(s_ui.content, SYS_GAUGE_X[1], 4, UI_C_ACCENT_2, "%");
    s_ui.disk = sys_make_gauge(s_ui.content, SYS_GAUGE_X[2], 4, UI_C_ACCENT,   "%");
    sys_make_gauge_label(s_ui.content, SYS_GAUGE_X[0], 70, "CPU");
    sys_make_gauge_label(s_ui.content, SYS_GAUGE_X[1], 70, "内存");
    sys_make_gauge_label(s_ui.content, SYS_GAUGE_X[2], 70, "磁盘");

    /* 信息卡 1：电池 + 温度 */
    lv_obj_t *c1 = sys_make_info_card(s_ui.content, 92, 50);
    sys_kv_t r = sys_make_kv_row(c1, 0,  "电池");      s_ui.bat_lbl  = r.v;
    r          = sys_make_kv_row(c1, 18, "CPU 温度"); s_ui.temp_lbl = r.v;

    /* 信息卡 2：网速 + 运行 */
    lv_obj_t *c2 = sys_make_info_card(s_ui.content, 148, 60);
    r = sys_make_kv_row(c2, 0,  "↓ 下行"); s_ui.dn_lbl     = r.v;
    r = sys_make_kv_row(c2, 16, "↑ 上行"); s_ui.up_lbl     = r.v;
    r = sys_make_kv_row(c2, 32, "运行");   s_ui.uptime_lbl = r.v;

    sys_attach_hit_and_swipe(s_ui.screen);

    refresh();
    s_ui.last_epoch = system_manager_get_epoch();

    /* 进入立刻让 PC 推一帧 */
    system_service_send_request();

    return s_ui.screen;
}

static void destroy(void)
{
    ESP_LOGI(TAG, "destroy");
    if (s_ui.screen) {
        lv_obj_del(s_ui.screen);
    }
    memset(&s_ui, 0, sizeof(s_ui));
}

static void update(void)
{
    uint32_t e = system_manager_get_epoch();
    if (e != s_ui.last_epoch) {
        s_ui.last_epoch = e;
        refresh();
    }
}

static const page_callbacks_t s_cb = {
    .create  = create,
    .destroy = destroy,
    .update  = update,
};

const page_callbacks_t *page_system_pc_get_callbacks(void)
{
    return &s_cb;
}
