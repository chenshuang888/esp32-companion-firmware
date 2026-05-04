#include "time_manager.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static const char *TAG = "time_manager";

#define TIME_QUEUE_DEPTH 4

static QueueHandle_t s_queue = NULL;

esp_err_t time_manager_init(void)
{
    if (s_queue) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    s_queue = xQueueCreate(TIME_QUEUE_DEPTH, sizeof(struct timeval));
    if (!s_queue) {
        ESP_LOGE(TAG, "Failed to create queue");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Time manager initialized (queue depth %d)", TIME_QUEUE_DEPTH);
    return ESP_OK;
}

esp_err_t time_manager_request_set_time(const struct timeval *tv)
{
    if (!s_queue) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (!tv) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xQueueSend(s_queue, tv, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Queue full, set time request dropped");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void time_manager_process_pending(void)
{
    if (!s_queue) {
        return;
    }

    struct timeval tv;
    while (xQueueReceive(s_queue, &tv, 0) == pdTRUE) {
        if (settimeofday(&tv, NULL) != 0) {
            ESP_LOGE(TAG, "settimeofday failed");
            continue;
        }
        ESP_LOGI(TAG, "System time updated (tv_sec=%lld)", (long long)tv.tv_sec);
    }
}
