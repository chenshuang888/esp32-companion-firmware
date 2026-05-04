#include "app_main.h"

#include "app_router.h"

#include "lockscreen_app.h"
#include "launcher_app.h"
#include "weather_app.h"
#include "notifications_app.h"
#include "music_app.h"
#include "system_app.h"
#include "settings_app.h"
#include "dynapp_host_app.h"
#include "clock_app.h"

#include "app_fonts.h"
#include "weather_icons.h"
#include "lvgl_port.h"
#include "lcd_panel.h"
#include "time_manager.h"
#include "weather_manager.h"
#include "notify_manager.h"
#include "media_manager.h"
#include "playlist_manager.h"
#include "device_stats.h"
#include "system_manager.h"
#include "backlight_storage.h"
#include "time_storage.h"
#include "dynamic_app.h"
#include "dynamic_app_registry.h"
#include "dynapp_fs_worker.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "app_main";

/* fs_worker 删脚本前问 UI："这个 app 正在跑吗"。
 * 只有当前在 dynapp_host 才可能在跑动态 app。 */
static bool is_app_running(const char *name)
{
    const char *cur = app_router_current_id();
    if (!cur || strcmp(cur, "dynapp_host") != 0) return false;
    const char *running = dynamic_app_registry_current();
    return running && running[0] && strcmp(running, name) == 0;
}

static void ui_task(void *arg)
{
    (void)arg;
    uint32_t loop_cnt = 0;
    while (1) {
        /* Script -> UI 队列桥：每帧消化较多命令 */
        dynamic_app_ui_drain(32);

        time_manager_process_pending();
        weather_manager_process_pending();
        notify_manager_process_pending();
        media_manager_process_pending();
        playlist_manager_process_pending();
        system_manager_process_pending();

        /* ESP 端自身运行数据：1Hz 采集（loop ~10ms × 100 = 1s） */
        if ((loop_cnt++ % 100) == 0) {
            device_stats_tick();
        }

        app_router_tick();           /* 转发到当前 app on_tick */
        lvgl_port_task_handler();

        notify_manager_tick_flush();
        time_storage_tick_save();

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t app_main_init(void)
{
    ESP_LOGI(TAG, "Initializing application");

    ESP_ERROR_CHECK(lvgl_port_init());

    app_fonts_init();
    weather_icons_init();
    lcd_panel_set_backlight(backlight_storage_get());

    ESP_ERROR_CHECK(dynamic_app_init());

    /* ---- App router ---- */
    ESP_ERROR_CHECK(app_router_init());

    ESP_ERROR_CHECK(app_router_register(&LOCKSCREEN_APP));
    ESP_ERROR_CHECK(app_router_register(&LAUNCHER_APP));
    ESP_ERROR_CHECK(app_router_register(&WEATHER_APP));
    ESP_ERROR_CHECK(app_router_register(&NOTIFICATIONS_APP));
    ESP_ERROR_CHECK(app_router_register(&MUSIC_APP));
    ESP_ERROR_CHECK(app_router_register(&SYSTEM_APP));
    ESP_ERROR_CHECK(app_router_register(&SETTINGS_APP));
    ESP_ERROR_CHECK(app_router_register(&CLOCK_APP));
    ESP_ERROR_CHECK(app_router_register(&DYNAPP_HOST_APP));

    /* launcher 一次性 module init（注册 upload 观察者；旧实现里这步在 page on_enter 重复跑） */
    launcher_app_module_init();

    ESP_ERROR_CHECK(app_router_enter("lockscreen"));

    /* fs_worker hook：必须在 app_router_init 之后注册（hook 内读 app_router_current_id）*/
    dynapp_fs_worker_set_running_check(is_app_running);

    BaseType_t ret = xTaskCreatePinnedToCore(ui_task, "ui_task", 8192, NULL, 5, NULL, 1);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create UI task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Application initialized");
    return ESP_OK;
}
