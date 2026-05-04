#include "system_manager.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <string.h>

static const char *TAG = "system_mgr";

#define SYSTEM_QUEUE_DEPTH 2

static QueueHandle_t s_queue = NULL;
static system_payload_t s_latest = {0};
static bool s_has_data = false;
static uint32_t s_epoch = 0;

esp_err_t system_manager_init(void)
{
    if (s_queue) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    s_queue = xQueueCreate(SYSTEM_QUEUE_DEPTH, sizeof(system_payload_t));
    if (!s_queue) {
        ESP_LOGE(TAG, "Failed to create queue");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "System manager initialized (queue depth %d)", SYSTEM_QUEUE_DEPTH);
    return ESP_OK;
}

esp_err_t system_manager_push(const system_payload_t *data)
{
    if (!s_queue) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (!data) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 队列满丢最旧——系统监控数据越新越好 */
    if (xQueueSend(s_queue, data, 0) != pdTRUE) {
        system_payload_t dropped;
        xQueueReceive(s_queue, &dropped, 0);
        if (xQueueSend(s_queue, data, 0) != pdTRUE) {
            ESP_LOGW(TAG, "Queue push failed after drop");
            return ESP_FAIL;
        }
        ESP_LOGD(TAG, "Queue was full, oldest entry dropped");
    }

    return ESP_OK;
}

void system_manager_process_pending(void)
{
    if (!s_queue) {
        return;
    }

    system_payload_t tmp;
    while (xQueueReceive(s_queue, &tmp, 0) == pdTRUE) {
        memcpy(&s_latest, &tmp, sizeof(s_latest));
        s_has_data = true;
        s_epoch++;

        ESP_LOGD(TAG, "System: CPU=%u%% MEM=%u%% DISK=%u%% BAT=%u%% up=%us net=%u/%u KB/s",
                 s_latest.cpu_percent, s_latest.mem_percent,
                 s_latest.disk_percent, s_latest.battery_percent,
                 (unsigned)s_latest.uptime_sec,
                 s_latest.net_down_kbps, s_latest.net_up_kbps);
    }
}

const system_payload_t *system_manager_get_latest(void)
{
    return s_has_data ? &s_latest : NULL;
}

bool system_manager_has_data(void)
{
    return s_has_data;
}

uint32_t system_manager_get_epoch(void)
{
    return s_epoch;
}
