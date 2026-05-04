#include "device_stats.h"
#include "fs_littlefs.h"
#include "ble_driver.h"

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/temperature_sensor.h"

#include <string.h>

static const char *TAG = "dev_stats";

static device_stats_t s_stats;
static uint32_t       s_epoch;
static temperature_sensor_handle_t s_temp = NULL;
static int64_t        s_last_fs_us;       /* littlefs 查询不需要 1Hz，10s 一次足够 */
static int64_t        s_last_temp_us;     /* 温度 1Hz 即可 */

#define FS_QUERY_INTERVAL_US     (10LL * 1000 * 1000)
#define TEMP_QUERY_INTERVAL_US   (1LL  * 1000 * 1000)

esp_err_t device_stats_init(void)
{
    /* 先把不变量 / 静态字段填了 */
    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.chip_temp_cx10 = -32768;
    s_stats.psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    s_stats.sram_total  = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    s_stats.reset_reason = (uint8_t)esp_reset_reason();

    const esp_app_desc_t *desc = esp_app_get_description();
    if (desc) {
        strncpy(s_stats.fw_version, desc->version, sizeof(s_stats.fw_version) - 1);
        s_stats.fw_version[sizeof(s_stats.fw_version) - 1] = '\0';
    }

    /* 温度 sensor 装一次，整个生命周期常开 */
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    esp_err_t r = temperature_sensor_install(&cfg, &s_temp);
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "temp sensor install failed: 0x%x", r);
        s_temp = NULL;
    } else {
        temperature_sensor_enable(s_temp);
    }

    ESP_LOGI(TAG, "device_stats init: PSRAM=%uKB SRAM=%uKB FW=%s reset=%u",
             (unsigned)(s_stats.psram_total / 1024),
             (unsigned)(s_stats.sram_total / 1024),
             s_stats.fw_version,
             s_stats.reset_reason);
    return ESP_OK;
}

void device_stats_tick(void)
{
    int64_t now_us = esp_timer_get_time();

    /* 内存：每次都查（cheap） */
    s_stats.psram_free    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    s_stats.sram_free     = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    s_stats.min_free_ever = esp_get_minimum_free_heap_size();

    /* uptime */
    s_stats.uptime_sec = (uint32_t)(now_us / 1000000);

    /* 任务数 */
    s_stats.task_count = (uint16_t)uxTaskGetNumberOfTasks();

    /* BLE 连接 */
    s_stats.ble_connected = ble_driver_is_connected();

    /* 温度（1Hz，避免 ADC 频繁触发）*/
    if (s_temp && (now_us - s_last_temp_us) >= TEMP_QUERY_INTERVAL_US) {
        s_last_temp_us = now_us;
        float c = 0;
        if (temperature_sensor_get_celsius(s_temp, &c) == ESP_OK) {
            int v = (int)(c * 10.0f);
            if (v < -32767) v = -32767;
            if (v >  32767) v =  32767;
            s_stats.chip_temp_cx10 = (int16_t)v;
        }
    }

    /* littlefs（10s 一次，挂载层有 cache 也不便宜）*/
    if ((now_us - s_last_fs_us) >= FS_QUERY_INTERVAL_US || s_stats.fs_total == 0) {
        s_last_fs_us = now_us;
        size_t total = 0, used = 0;
        if (fs_littlefs_info(&total, &used) == ESP_OK) {
            s_stats.fs_total = (uint32_t)total;
            s_stats.fs_used  = (uint32_t)used;
        }
    }

    s_epoch++;
}

const device_stats_t *device_stats_get(void) { return &s_stats; }
uint32_t device_stats_epoch(void) { return s_epoch; }
