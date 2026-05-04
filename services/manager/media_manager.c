#include "media_manager.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <string.h>

static const char *TAG = "media_mgr";

#define MEDIA_QUEUE_DEPTH 2

/* ------------------------------------------------------------------
 * 状态（仅 UI 线程读写 latest/received_at/version；
 *       BLE host 线程只通过 s_queue 投递，不直接碰下面的变量）
 * ------------------------------------------------------------------ */

static QueueHandle_t     s_queue = NULL;
static media_payload_t   s_latest = {0};
static int64_t           s_received_at_us = 0;
static bool              s_has_data = false;
static uint32_t          s_version = 0;

/* ------------------------------------------------------------------
 * 对外 API
 * ------------------------------------------------------------------ */

esp_err_t media_manager_init(void)
{
    if (s_queue) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    s_queue = xQueueCreate(MEDIA_QUEUE_DEPTH, sizeof(media_payload_t));
    if (!s_queue) {
        ESP_LOGE(TAG, "Failed to create queue");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Media manager initialized (queue depth %d)", MEDIA_QUEUE_DEPTH);
    return ESP_OK;
}

esp_err_t media_manager_push(const media_payload_t *payload)
{
    if (!s_queue) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (!payload) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 队列满时丢弃最旧的 — 最新状态总是最重要，不需要历史 */
    if (xQueueSend(s_queue, payload, 0) != pdTRUE) {
        media_payload_t dropped;
        xQueueReceive(s_queue, &dropped, 0);
        if (xQueueSend(s_queue, payload, 0) != pdTRUE) {
            ESP_LOGW(TAG, "Queue push failed after drop");
            return ESP_FAIL;
        }
        ESP_LOGD(TAG, "Queue was full, oldest entry dropped");
    }

    return ESP_OK;
}

void media_manager_process_pending(void)
{
    if (!s_queue) {
        return;
    }

    media_payload_t tmp;
    bool changed = false;
    while (xQueueReceive(s_queue, &tmp, 0) == pdTRUE) {
        tmp.title[MEDIA_TITLE_MAX - 1]   = '\0';
        tmp.artist[MEDIA_ARTIST_MAX - 1] = '\0';

        /* 检测"实质性"变化（标题/作者/播放状态）。
         * companion 1Hz 推 NOWPLAYING 心跳，即便内容空也来——
         * 只在真变化时打 INFO，避免日志被心跳刷屏。 */
        bool real_change = !s_has_data
            || (tmp.playing != s_latest.playing)
            || (strncmp(tmp.title,  s_latest.title,  MEDIA_TITLE_MAX)  != 0)
            || (strncmp(tmp.artist, s_latest.artist, MEDIA_ARTIST_MAX) != 0);

        memcpy(&s_latest, &tmp, sizeof(tmp));
        s_received_at_us = esp_timer_get_time();
        s_has_data = true;
        changed = true;

        if (real_change) {
            ESP_LOGI(TAG, "media: [%s] \"%s\" - \"%s\", pos=%d/%d",
                     tmp.playing ? "PLAY" : "PAUSE",
                     tmp.title, tmp.artist,
                     tmp.position_sec, tmp.duration_sec);
        } else {
            ESP_LOGD(TAG, "media: [%s] tick pos=%d/%d",
                     tmp.playing ? "PLAY" : "PAUSE",
                     tmp.position_sec, tmp.duration_sec);
        }
    }

    if (changed) {
        s_version++;
    }
}

const media_payload_t *media_manager_get_latest(void)
{
    return s_has_data ? &s_latest : NULL;
}

bool media_manager_has_data(void)
{
    return s_has_data;
}

int16_t media_manager_get_position_now(void)
{
    if (!s_has_data || s_latest.position_sec < 0) {
        return -1;
    }
    if (!s_latest.playing) {
        return s_latest.position_sec;
    }

    int64_t elapsed_us = esp_timer_get_time() - s_received_at_us;
    int32_t pos = (int32_t)s_latest.position_sec + (int32_t)(elapsed_us / 1000000);

    if (s_latest.duration_sec > 0 && pos > s_latest.duration_sec) {
        pos = s_latest.duration_sec;
    }
    if (pos < 0) {
        pos = 0;
    }
    return (int16_t)pos;
}

uint32_t media_manager_version(void)
{
    return s_version;
}
