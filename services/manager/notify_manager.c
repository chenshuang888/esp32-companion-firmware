#include "notify_manager.h"
#include "notify_storage.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <string.h>

static const char *TAG = "notify_mgr";

#define NOTIFY_QUEUE_DEPTH 4

/* ------------------------------------------------------------------
 * 持久化 blob 版本
 * ------------------------------------------------------------------ */

#define NOTIFY_PERSIST_VERSION 1

typedef struct {
    uint8_t  version;       /* = NOTIFY_PERSIST_VERSION */
    uint8_t  head;
    uint8_t  count;
    uint8_t  _pad;
    notification_payload_t ring[NOTIFY_STORE_MAX];
} __attribute__((packed)) notify_persist_v1_t;   /* 4 + 10 * 136 = 1364 B */

/* ------------------------------------------------------------------
 * 防抖落盘参数
 * ------------------------------------------------------------------ */

#define FLUSH_DEBOUNCE_US (2LL * 1000000)   /* dirty 后等 2 秒合并写入 */

/* ------------------------------------------------------------------
 * 状态
 * ------------------------------------------------------------------ */

static QueueHandle_t s_queue = NULL;

static notification_payload_t s_ring[NOTIFY_STORE_MAX];
static size_t   s_head = 0;
static size_t   s_count = 0;
static uint32_t s_version = 0;

static bool    s_dirty = false;
static int64_t s_dirty_since_us = 0;

/* ------------------------------------------------------------------
 * 内部：NVS 读写
 * ------------------------------------------------------------------ */

static void load_from_nvs(void)
{
    static notify_persist_v1_t blob;  /* 1364 B，避免占栈 */
    size_t len = sizeof(blob);
    esp_err_t err = notify_storage_load(&blob, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no snapshot in NVS, starting empty");
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "load blob failed: 0x%x, starting empty", err);
        return;
    }
    if (len != sizeof(blob) || blob.version != NOTIFY_PERSIST_VERSION) {
        ESP_LOGW(TAG, "blob invalid (ver=%u, len=%u), discarding",
                 blob.version, (unsigned)len);
        return;
    }

    s_head  = blob.head  % NOTIFY_STORE_MAX;
    s_count = blob.count > NOTIFY_STORE_MAX ? NOTIFY_STORE_MAX : blob.count;
    memcpy(s_ring, blob.ring, sizeof(s_ring));
    s_version++;   /* 触发 UI 下一帧刷新 */

    ESP_LOGI(TAG, "loaded %u entries from NVS", (unsigned)s_count);
}

static esp_err_t save_to_nvs(void)
{
    static notify_persist_v1_t blob;  /* 1364 B，避免占栈 */
    memset(&blob, 0, sizeof(blob));
    blob.version = NOTIFY_PERSIST_VERSION;
    blob.head    = (uint8_t)s_head;
    blob.count   = (uint8_t)s_count;
    memcpy(blob.ring, s_ring, sizeof(s_ring));

    return notify_storage_save(&blob, sizeof(blob));
}

/* ------------------------------------------------------------------
 * 对外 API
 * ------------------------------------------------------------------ */

esp_err_t notify_manager_init(void)
{
    if (s_queue) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    s_queue = xQueueCreate(NOTIFY_QUEUE_DEPTH, sizeof(notification_payload_t));
    if (!s_queue) {
        ESP_LOGE(TAG, "Failed to create queue");
        return ESP_ERR_NO_MEM;
    }

    s_head = 0;
    s_count = 0;
    s_version = 0;
    s_dirty = false;

    load_from_nvs();

    ESP_LOGI(TAG, "Notify manager initialized (queue=%d, store=%d, loaded=%u)",
             NOTIFY_QUEUE_DEPTH, NOTIFY_STORE_MAX, (unsigned)s_count);
    return ESP_OK;
}

esp_err_t notify_manager_push(const notification_payload_t *n)
{
    if (!s_queue) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (!n) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 队列满时丢最旧 — 与 weather_manager 一致，避免 BLE 回调阻塞 */
    if (xQueueSend(s_queue, n, 0) != pdTRUE) {
        notification_payload_t dropped;
        xQueueReceive(s_queue, &dropped, 0);
        if (xQueueSend(s_queue, n, 0) != pdTRUE) {
            ESP_LOGW(TAG, "Queue push failed after drop");
            return ESP_FAIL;
        }
        ESP_LOGD(TAG, "Queue was full, oldest entry dropped");
    }

    return ESP_OK;
}

void notify_manager_process_pending(void)
{
    if (!s_queue) {
        return;
    }

    notification_payload_t tmp;
    bool changed = false;
    while (xQueueReceive(s_queue, &tmp, 0) == pdTRUE) {
        tmp.title[NOTIFY_TITLE_MAX - 1] = '\0';
        tmp.body[NOTIFY_BODY_MAX - 1]   = '\0';

        memcpy(&s_ring[s_head], &tmp, sizeof(tmp));
        s_head = (s_head + 1) % NOTIFY_STORE_MAX;
        if (s_count < NOTIFY_STORE_MAX) {
            s_count++;
        }
        changed = true;

        ESP_LOGI(TAG, "Notification stored: [%u] %s", tmp.category, tmp.title);
    }

    if (changed) {
        s_version++;
        if (!s_dirty) {
            s_dirty = true;
            s_dirty_since_us = esp_timer_get_time();
        }
    }
}

void notify_manager_tick_flush(void)
{
    if (!s_dirty) {
        return;
    }
    int64_t now = esp_timer_get_time();
    if (now - s_dirty_since_us < FLUSH_DEBOUNCE_US) {
        return;
    }

    esp_err_t err = save_to_nvs();
    if (err == ESP_OK) {
        s_dirty = false;
        ESP_LOGI(TAG, "flushed %u entries to NVS", (unsigned)s_count);
    } else {
        /* 写入失败：推迟下次尝试，不卡死在每帧重试 */
        s_dirty_since_us = now;
        ESP_LOGW(TAG, "flush failed: 0x%x, will retry later", err);
    }
}

size_t notify_manager_count(void)
{
    return s_count;
}

const notification_payload_t *notify_manager_get_at(size_t index)
{
    if (index >= s_count) {
        return NULL;
    }
    size_t slot = (s_head + NOTIFY_STORE_MAX - 1 - index) % NOTIFY_STORE_MAX;
    return &s_ring[slot];
}

uint32_t notify_manager_version(void)
{
    return s_version;
}

void notify_manager_clear(void)
{
    s_head = 0;
    s_count = 0;
    s_version++;
    s_dirty = false;

    esp_err_t err = save_to_nvs();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "all notifications cleared and persisted");
    } else {
        ESP_LOGW(TAG, "clear persist failed: 0x%x", err);
    }
}

esp_err_t notify_manager_remove_at(size_t index)
{
    if (index >= s_count) {
        return ESP_ERR_INVALID_ARG;
    }

    /* get_at 序：slot = (head - 1 - index) mod N
     * 删除该 slot：把比它"更旧"的所有 slot 朝 head 方向逐个前移一格。
     *
     * 环形缓冲示意（head 指向下一个写入位置，逻辑序：head-1 最新 → ...）：
     *   逻辑 i=0    1    2 ...  count-1
     *   slot head-1, head-2, ..., head-count
     *
     * 删 index 后：
     *   把 [head-2, ..., head-count] 中比目标更旧的元素朝 head-1 方向平移
     *   即：从 idx=index..count-2，把 i+1 槽位的内容拷到 i 槽位。
     *   最后 head 回退一格、count-- 。
     */
    for (size_t i = index; i + 1 < s_count; i++) {
        size_t dst = (s_head + NOTIFY_STORE_MAX - 1 - i)     % NOTIFY_STORE_MAX;
        size_t src = (s_head + NOTIFY_STORE_MAX - 1 - (i+1)) % NOTIFY_STORE_MAX;
        memcpy(&s_ring[dst], &s_ring[src], sizeof(s_ring[0]));
    }

    /* 最旧那个槽位被腾出，head 回退一格 */
    s_head = (s_head + NOTIFY_STORE_MAX - 1) % NOTIFY_STORE_MAX;
    memset(&s_ring[s_head], 0, sizeof(s_ring[0]));
    s_count--;

    s_version++;
    if (!s_dirty) {
        s_dirty = true;
        s_dirty_since_us = esp_timer_get_time();
    }

    ESP_LOGI(TAG, "removed index=%u, remaining=%u",
             (unsigned)index, (unsigned)s_count);
    return ESP_OK;
}
