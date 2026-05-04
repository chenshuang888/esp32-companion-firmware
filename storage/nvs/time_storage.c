#include "time_storage.h"
#include "persist.h"

#include <time.h>
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "time_storage";

#define NS_TIME               "time"
#define KEY_LAST_TIME_SEC     "last_ts"

#define TIME_SAVE_INTERVAL_US (5LL * 60 * 1000000)  /* 5 分钟 */

static int64_t s_last_save_us = 0;

esp_err_t time_storage_init(void)
{
    s_last_save_us = esp_timer_get_time();  /* 避免启动立刻就写 */
    return ESP_OK;
}

esp_err_t time_storage_load_last(struct timeval *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;

    int64_t ts = 0;
    esp_err_t err = persist_get_i64(NS_TIME, KEY_LAST_TIME_SEC, &ts);
    if (err != ESP_OK) {
        return err;
    }
    if (ts <= 0) {
        return ESP_ERR_NVS_NOT_FOUND;
    }

    out->tv_sec = (time_t)ts;
    out->tv_usec = 0;
    ESP_LOGI(TAG, "loaded last_ts=%lld", (long long)ts);
    return ESP_OK;
}

void time_storage_tick_save(void)
{
    int64_t now_us = esp_timer_get_time();
    if (now_us - s_last_save_us < TIME_SAVE_INTERVAL_US) {
        return;
    }
    s_last_save_us = now_us;

    time_t now = time(NULL);
    if (now <= 0) {
        return;
    }

    esp_err_t err = persist_set_i64(NS_TIME, KEY_LAST_TIME_SEC, (int64_t)now);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "last_ts periodically saved=%lld", (long long)now);
    } else {
        ESP_LOGW(TAG, "save last_ts failed: 0x%x", err);
    }
}
