#include "page_system_device.h"
#include "system_app.h"
#include "system_ui_common.h"
#include "app_shell_ui.h"
#include "app_router.h"

#include "device_stats.h"

#include "esp_log.h"
#include "lvgl.h"
#include "app_fonts.h"
#include "ui_tokens.h"
#include "ui_widgets.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "page_sys_dev";

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *content;

    sys_gauge_t psram, sram, temp;
    lv_obj_t *uptime_lbl;
    lv_obj_t *tasks_lbl;
    lv_obj_t *ble_lbl;
    lv_obj_t *fs_lbl;
    lv_obj_t *reset_lbl;
    lv_obj_t *fw_lbl;

    uint32_t last_epoch;
} ui_t;

static ui_t s_ui;

static const lv_color_t WARN_ORANGE = LV_COLOR_MAKE(0xFF, 0x95, 0x00);
static const lv_color_t WARN_RED    = LV_COLOR_MAKE(0xFF, 0x3B, 0x30);

static const char *reset_reason_str(uint8_t r)
{
    switch (r) {
    case 0:  return "未知";
    case 1:  return "上电";
    case 2:  return "外部";
    case 3:  return "软重启";
    case 4:  return "崩溃";
    case 5:  return "SDIO";
    case 6:  return "看门狗";
    case 7:  return "INT看门狗";
    case 8:  return "TASK看门狗";
    case 9:  return "其他WDT";
    case 10: return "深睡唤醒";
    case 11: return "BOR";
    case 12: return "USB";
    case 13: return "JTAG";
    default: return "其他";
    }
}

static void refresh(void)
{
    const device_stats_t *d = device_stats_get();
    if (!d) return;

    char buf[48];

    int psram_pct = 0;
    if (d->psram_total > 0) {
        psram_pct = (int)(((d->psram_total - d->psram_free) * 100ULL) / d->psram_total);
    }
    sys_apply_gauge(&s_ui.psram, psram_pct, UI_C_ACCENT_2, WARN_ORANGE, 85);

    int sram_pct = 0;
    if (d->sram_total > 0) {
        sram_pct = (int)(((d->sram_total - d->sram_free) * 100ULL) / d->sram_total);
    }
    sys_apply_gauge(&s_ui.sram, sram_pct, UI_C_ACCENT, WARN_RED, 90);

    /* 温度：直显数字（°C） */
    int temp_int = -1;
    if (d->chip_temp_cx10 != -32768) {
        temp_int = d->chip_temp_cx10 / 10;
        if (temp_int < 0) temp_int = 0;
        if (temp_int > 100) temp_int = 100;
    }
    if (temp_int < 0) {
        lv_label_set_text(s_ui.temp.num_lbl, "--");
        lv_arc_set_value(s_ui.temp.arc, 0);
    } else {
        snprintf(buf, sizeof(buf), "%d", temp_int);
        lv_label_set_text(s_ui.temp.num_lbl, buf);
        lv_arc_set_value(s_ui.temp.arc, temp_int);
        lv_color_t c = (temp_int >= 70) ? WARN_ORANGE : UI_C_ACCENT;
        lv_obj_set_style_arc_color(s_ui.temp.arc, c, LV_PART_INDICATOR);
    }

    /* 运行时长 */
    uint32_t sec = d->uptime_sec;
    uint32_t h = sec / 3600, m = (sec % 3600) / 60, ss = sec % 60;
    if (h) snprintf(buf, sizeof(buf), "%uh %um", (unsigned)h, (unsigned)m);
    else if (m) snprintf(buf, sizeof(buf), "%um %us", (unsigned)m, (unsigned)ss);
    else snprintf(buf, sizeof(buf), "%us", (unsigned)ss);
    lv_label_set_text(s_ui.uptime_lbl, buf);

    snprintf(buf, sizeof(buf), "%u 个", (unsigned)d->task_count);
    lv_label_set_text(s_ui.tasks_lbl, buf);

    if (d->ble_connected) {
        lv_label_set_text(s_ui.ble_lbl, "● 已连接");
        lv_obj_set_style_text_color(s_ui.ble_lbl, lv_color_hex(0x34C759), 0);
    } else {
        lv_label_set_text(s_ui.ble_lbl, "○ 未连接");
        lv_obj_set_style_text_color(s_ui.ble_lbl, UI_C_TEXT_MUTED, 0);
    }

    if (d->fs_total > 0) {
        unsigned ukb = d->fs_used / 1024, tkb = d->fs_total / 1024;
        if (tkb >= 1024) snprintf(buf, sizeof(buf), "%u/%uM", ukb / 1024, tkb / 1024);
        else             snprintf(buf, sizeof(buf), "%u/%uK", ukb, tkb);
        lv_label_set_text(s_ui.fs_lbl, buf);
    } else {
        lv_label_set_text(s_ui.fs_lbl, "--");
    }

    lv_label_set_text(s_ui.reset_lbl, reset_reason_str(d->reset_reason));
    lv_label_set_text(s_ui.fw_lbl, d->fw_version[0] ? d->fw_version : "--");
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

    s_ui.content = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(s_ui.content);
    lv_obj_set_size(s_ui.content, 240, 320 - SYS_CONTENT_TOP - SYS_HIT_ZONE_H);
    lv_obj_set_pos(s_ui.content, 0, SYS_CONTENT_TOP);
    lv_obj_set_style_bg_opa(s_ui.content, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_ui.content, LV_OBJ_FLAG_SCROLLABLE);

    s_ui.psram = sys_make_gauge(s_ui.content, SYS_GAUGE_X[0], 4, UI_C_ACCENT_2, "%");
    s_ui.sram  = sys_make_gauge(s_ui.content, SYS_GAUGE_X[1], 4, UI_C_ACCENT,   "%");
    s_ui.temp  = sys_make_gauge(s_ui.content, SYS_GAUGE_X[2], 4, UI_C_ACCENT,   "°C");
    sys_make_gauge_label(s_ui.content, SYS_GAUGE_X[0], 70, "PSRAM");
    sys_make_gauge_label(s_ui.content, SYS_GAUGE_X[1], 70, "SRAM");
    sys_make_gauge_label(s_ui.content, SYS_GAUGE_X[2], 70, "温度");

    /* 信息卡 1：运行 / 任务 / BLE */
    lv_obj_t *c1 = sys_make_info_card(s_ui.content, 92, 60);
    sys_kv_t r = sys_make_kv_row(c1, 0,  "运行时长"); s_ui.uptime_lbl = r.v;
    r          = sys_make_kv_row(c1, 16, "任务数");  s_ui.tasks_lbl  = r.v;
    r          = sys_make_kv_row(c1, 32, "BLE");     s_ui.ble_lbl    = r.v;

    /* 信息卡 2：存储 / 复位 / 固件 */
    lv_obj_t *c2 = sys_make_info_card(s_ui.content, 158, 60);
    r = sys_make_kv_row(c2, 0,  "存储");     s_ui.fs_lbl    = r.v;
    r = sys_make_kv_row(c2, 16, "复位原因"); s_ui.reset_lbl = r.v;
    r = sys_make_kv_row(c2, 32, "固件");     s_ui.fw_lbl    = r.v;

    sys_attach_hit_and_swipe(s_ui.screen);

    refresh();
    s_ui.last_epoch = device_stats_epoch();

    return s_ui.screen;
}

static void destroy(void)
{
    ESP_LOGI(TAG, "destroy");
    if (s_ui.screen) lv_obj_del(s_ui.screen);
    memset(&s_ui, 0, sizeof(s_ui));
}

static void update(void)
{
    uint32_t e = device_stats_epoch();
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

const page_callbacks_t *page_system_device_get_callbacks(void)
{
    return &s_cb;
}
