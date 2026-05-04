#include "weather_manager.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <string.h>

static const char *TAG = "weather_mgr";

#define WEATHER_QUEUE_DEPTH 2

static QueueHandle_t s_queue = NULL;
static weather_payload_t s_latest = {0};
static bool s_has_data = false;

esp_err_t weather_manager_init(void)
{
    if (s_queue) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    s_queue = xQueueCreate(WEATHER_QUEUE_DEPTH, sizeof(weather_payload_t));
    if (!s_queue) {
        ESP_LOGE(TAG, "Failed to create queue");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Weather manager initialized (queue depth %d)", WEATHER_QUEUE_DEPTH);
    return ESP_OK;
}

esp_err_t weather_manager_push(const weather_payload_t *data)
{
    if (!s_queue) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (!data) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 队列满时丢弃最旧的 — 天气数据越新越好，不需要保留历史 */
    if (xQueueSend(s_queue, data, 0) != pdTRUE) {
        weather_payload_t dropped;
        xQueueReceive(s_queue, &dropped, 0);
        if (xQueueSend(s_queue, data, 0) != pdTRUE) {
            ESP_LOGW(TAG, "Queue push failed after drop");
            return ESP_FAIL;
        }
        ESP_LOGD(TAG, "Queue was full, oldest entry dropped");
    }

    return ESP_OK;
}

void weather_manager_process_pending(void)
{
    if (!s_queue) {
        return;
    }

    weather_payload_t tmp;
    while (xQueueReceive(s_queue, &tmp, 0) == pdTRUE) {
        memcpy(&s_latest, &tmp, sizeof(s_latest));
        /* 防御：确保字符串有 \0 结尾 */
        s_latest.city[WEATHER_CITY_MAX - 1] = '\0';
        s_latest.description[WEATHER_DESC_MAX - 1] = '\0';
        s_has_data = true;

        int t = s_latest.temp_c_x10;
        int t_int = t / 10;
        int t_frac = (t < 0 ? -t : t) % 10;
        ESP_LOGI(TAG, "Weather updated: %d.%dC, %d%%, code=%d, %s @ %s",
                 t_int, t_frac,
                 s_latest.humidity,
                 s_latest.weather_code,
                 s_latest.description,
                 s_latest.city);
    }
}

const weather_payload_t *weather_manager_get_latest(void)
{
    return s_has_data ? &s_latest : NULL;
}

bool weather_manager_has_data(void)
{
    return s_has_data;
}
