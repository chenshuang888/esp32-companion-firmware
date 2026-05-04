#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include "esp_log.h"
#include "app_main.h"
#include "ble_driver.h"
#include "time_service.h"
#include "weather_service.h"
#include "notify_service.h"
#include "media_service.h"
#include "system_service.h"
#include "dynapp_bridge_service.h"
#include "dynapp_upload_service.h"
#include "dynapp_script_store.h"
#include "dynapp_fs_worker.h"
#include "dynapp_mailbox.h"
#include "time_manager.h"
#include "weather_manager.h"
#include "notify_manager.h"
#include "media_manager.h"
#include "playlist_manager.h"
#include "system_manager.h"
#include "device_stats.h"
#include "persist.h"
#include "backlight_storage.h"
#include "time_storage.h"
#include "fs_littlefs.h"

static const char *TAG = "main";

static void init_default_time(void)
{
    struct tm timeinfo = {
        .tm_year = 2026 - 1900,
        .tm_mon = 4 - 1,
        .tm_mday = 17,
        .tm_hour = 12,
        .tm_min = 0,
        .tm_sec = 0
    };
    time_t t = mktime(&timeinfo);
    struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
    settimeofday(&tv, NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting application");

    // 最先初始化持久化，后续模块都可能用 NVS
    ESP_ERROR_CHECK(persist_init());
    ESP_ERROR_CHECK(backlight_storage_init());
    ESP_ERROR_CHECK(time_storage_init());

    // 挂载 LittleFS（动态 App 脚本、未来日志/缓存都从这里走 fopen/fread）
    ESP_ERROR_CHECK(fs_littlefs_init());
    ESP_ERROR_CHECK(dynapp_script_store_init());
    // 启动 LittleFS 串行写入 worker（任何线程都能非阻塞地丢任务）
    // 必须在 BLE upload service / dynamic_app 之前，否则 submit 会失败
    ESP_ERROR_CHECK(dynapp_fs_worker_init());

    // 恢复上次关机前的时间；失败走硬编码默认
    struct timeval tv;
    if (time_storage_load_last(&tv) == ESP_OK) {
        settimeofday(&tv, NULL);
        ESP_LOGI(TAG, "system time restored from NVS");
    } else {
        init_default_time();
        ESP_LOGI(TAG, "system time initialized to default");
    }

    // 初始化时间管理器（BLE 需要在初始化前就绪）
    ESP_ERROR_CHECK(time_manager_init());

    // 初始化天气管理器（同上）
    ESP_ERROR_CHECK(weather_manager_init());

    // 初始化通知管理器（同上；内部会从 NVS 加载上次快照）
    ESP_ERROR_CHECK(notify_manager_init());

    // 初始化媒体管理器（同上；BLE 回调会 push 到它）
    ESP_ERROR_CHECK(media_manager_init());

    // 初始化歌单管理器（PC 流式推歌单）
    ESP_ERROR_CHECK(playlist_manager_init());

    // 初始化系统监控管理器（PC 推 CPU/MEM/DISK/BAT/NET/Temp）
    ESP_ERROR_CHECK(system_manager_init());

    // 初始化 ESP32 端运行状态采集（温度 sensor + heap/uptime/...）
    ESP_ERROR_CHECK(device_stats_init());

    // 初始化 BLE：先拉起 NimBLE 协议栈，再让各 service 自行注册 GATT 表，最后启动 host task
    ESP_ERROR_CHECK(ble_driver_nimble_init());
    ESP_ERROR_CHECK(time_service_init());
    ESP_ERROR_CHECK(weather_service_init());
    ESP_ERROR_CHECK(notify_service_init());
    ESP_ERROR_CHECK(media_service_init());
    ESP_ERROR_CHECK(system_service_init());
    ESP_ERROR_CHECK(dynapp_bridge_service_init());
    ESP_ERROR_CHECK(dynapp_upload_service_init());
    ESP_ERROR_CHECK(ble_driver_nimble_start());

    // 动态 app 离线消息兜底：JS task 不在跑时把 BLE 消息归档到 NVS，启动时回灌
    // 必须在 dynapp_bridge_service_init 之后（用 inbox 接口）
    ESP_ERROR_CHECK(dynapp_mailbox_init());

    // 再初始化应用
    ESP_ERROR_CHECK(app_main_init());

    ESP_LOGI(TAG, "Application started");
}