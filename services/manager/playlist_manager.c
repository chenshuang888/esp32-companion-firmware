#include "playlist_manager.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <string.h>

static const char *TAG = "playlist_mgr";

/* 内部入队消息：以 type 区分 */
typedef enum {
    PLEV_BEGIN = 1,
    PLEV_ITEM  = 2,
    PLEV_END   = 3,
} playlist_evt_type_t;

typedef struct {
    uint8_t                  type;
    uint16_t                 total_count;
    uint16_t                 version;
    media_playlist_item_t    item;     // 仅 type=ITEM 有效
} playlist_evt_t;

#define PLAYLIST_QUEUE_DEPTH 16

static QueueHandle_t s_queue = NULL;

/* 双缓冲：pending 在装载，live 给 UI 读 */
static media_playlist_item_t s_pending[MEDIA_PLAYLIST_MAX_ITEMS];
static uint16_t              s_pending_total   = 0;
static uint16_t              s_pending_version = 0;
static bool                  s_loading         = false;

static media_playlist_item_t s_live[MEDIA_PLAYLIST_MAX_ITEMS];
static size_t                s_live_count      = 0;
static uint32_t              s_live_version    = 0;
static bool                  s_has_data        = false;

esp_err_t playlist_manager_init(void)
{
    if (s_queue) return ESP_OK;
    s_queue = xQueueCreate(PLAYLIST_QUEUE_DEPTH, sizeof(playlist_evt_t));
    if (!s_queue) {
        ESP_LOGE(TAG, "queue alloc failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "playlist_manager initialized (cap=%d, queue=%d)",
             MEDIA_PLAYLIST_MAX_ITEMS, PLAYLIST_QUEUE_DEPTH);
    return ESP_OK;
}

/* 队列满时丢最旧；歌单流是有序的，丢最旧会让 UI 拒收当前批次（END 后整体废）—
 * 但比 BLE 卡死好。实际正常情况下 16 深度足够缓冲一次推送。 */
static esp_err_t enqueue(const playlist_evt_t *e)
{
    if (!s_queue) return ESP_ERR_INVALID_STATE;
    if (xQueueSend(s_queue, e, 0) == pdTRUE) return ESP_OK;
    playlist_evt_t dropped;
    xQueueReceive(s_queue, &dropped, 0);
    if (xQueueSend(s_queue, e, 0) != pdTRUE) {
        ESP_LOGW(TAG, "enqueue failed even after drop");
        return ESP_FAIL;
    }
    ESP_LOGW(TAG, "queue full, dropped oldest");
    return ESP_OK;
}

esp_err_t playlist_manager_push_begin(uint16_t total_count, uint16_t version)
{
    playlist_evt_t e = {
        .type = PLEV_BEGIN, .total_count = total_count, .version = version,
    };
    return enqueue(&e);
}

esp_err_t playlist_manager_push_item(const media_playlist_item_t *item)
{
    if (!item) return ESP_ERR_INVALID_ARG;
    playlist_evt_t e = { .type = PLEV_ITEM };
    memcpy(&e.item, item, sizeof(*item));
    return enqueue(&e);
}

esp_err_t playlist_manager_push_end(void)
{
    playlist_evt_t e = { .type = PLEV_END };
    return enqueue(&e);
}

void playlist_manager_process_pending(void)
{
    if (!s_queue) return;
    playlist_evt_t e;
    while (xQueueReceive(s_queue, &e, 0) == pdTRUE) {
        switch (e.type) {
        case PLEV_BEGIN:
            s_pending_total   = e.total_count;
            s_pending_version = e.version;
            s_loading         = true;
            memset(s_pending, 0, sizeof(s_pending));
            ESP_LOGI(TAG, "BEGIN total=%u ver=%u", e.total_count, e.version);
            break;

        case PLEV_ITEM: {
            if (!s_loading) {
                ESP_LOGW(TAG, "ITEM without BEGIN, drop");
                break;
            }
            uint16_t idx = e.item.index;
            if (idx >= MEDIA_PLAYLIST_MAX_ITEMS) {
                ESP_LOGW(TAG, "ITEM idx=%u out of cap, drop", idx);
                break;
            }
            memcpy(&s_pending[idx], &e.item, sizeof(e.item));
            /* 强制 \0 结尾 */
            s_pending[idx].title[MEDIA_PLAYLIST_TITLE_MAX - 1]   = '\0';
            s_pending[idx].artist[MEDIA_PLAYLIST_ARTIST_MAX - 1] = '\0';
            break;
        }

        case PLEV_END: {
            if (!s_loading) {
                ESP_LOGW(TAG, "END without BEGIN, drop");
                break;
            }
            size_t n = s_pending_total;
            if (n > MEDIA_PLAYLIST_MAX_ITEMS) n = MEDIA_PLAYLIST_MAX_ITEMS;
            memcpy(s_live, s_pending, n * sizeof(media_playlist_item_t));
            s_live_count = n;
            s_live_version++;
            s_has_data = true;
            s_loading  = false;
            ESP_LOGI(TAG, "END committed: count=%u ver=%lu",
                     (unsigned)n, (unsigned long)s_live_version);
            break;
        }

        default:
            break;
        }
    }
}

size_t playlist_manager_get_count(void)
{
    return s_live_count;
}

const media_playlist_item_t *playlist_manager_get_track_at(size_t index)
{
    if (index >= s_live_count) return NULL;
    return &s_live[index];
}

uint32_t playlist_manager_version(void)
{
    return s_live_version;
}

bool playlist_manager_has_data(void)
{
    return s_has_data;
}
