#include "dynapp_mailbox.h"
#include "persist.h"
#include "dynapp_bridge_service.h"

#include <stdatomic.h>
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

static const char *TAG = "dynapp_mb";

#define NS_MAILBOX     "dynapp_mb"

/* blob 编码：
 *   [u8 version=1][u8 count][entries...]
 *   entry: [u16 len_le][len bytes raw payload]
 *
 * 头 2B + 每条 (2B + payload)；30 × (2 + 200) = 6060B + 2B 头 ≈ 6KB。 */
#define BLOB_VERSION   1
#define BLOB_HDR_LEN   2
#define ENTRY_HDR_LEN  2
#define BLOB_MAX_LEN   (BLOB_HDR_LEN + DYNAPP_MAILBOX_PER_APP_MAX * \
                        (ENTRY_HDR_LEN + DYNAPP_BRIDGE_MAX_PAYLOAD))

#define APP_ID_MAX     16   /* 含 \0；与 DYNAPP_REGISTRY_NAME_MAX+1 对齐 */

/* ---- replay 请求 ---- */
typedef struct {
    char app_id[APP_ID_MAX];
    SemaphoreHandle_t done;
} replay_req_t;

static QueueHandle_t  s_replay_q;
static atomic_bool    s_js_active = ATOMIC_VAR_INIT(false);
static TaskHandle_t   s_task;

/* ============================================================================
 * §1. 工具：peek "to":"xxx" → app_id
 * ========================================================================= */

static bool is_id_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/* 在 payload 里找 `"to"` <ws>* `:` <ws>* `"` <id> `"`，把 id 拷到 out。
 * 容忍 JSON 标准的可选空白（json.dumps 默认会加 ", "/ ": "）。 */
static bool peek_to_app_id(const uint8_t *data, size_t len,
                           char *out, size_t out_size)
{
    static const char NEEDLE[] = "\"to\"";
    const size_t nlen = sizeof(NEEDLE) - 1;
    if (len < nlen + 4 || out_size < 2) return false;

    for (size_t i = 0; i + nlen <= len; ++i) {
        if (memcmp(data + i, NEEDLE, nlen) != 0) continue;
        size_t p = i + nlen;
        /* 跳空白 */
        while (p < len && (data[p] == ' ' || data[p] == '\t')) p++;
        if (p >= len || data[p] != ':') continue;
        p++;
        while (p < len && (data[p] == ' ' || data[p] == '\t')) p++;
        if (p >= len || data[p] != '"') continue;
        p++;
        size_t k = 0;
        while (p < len && k < out_size - 1) {
            char c = (char)data[p];
            if (c == '"') break;
            if (!is_id_char(c)) return false;
            out[k++] = c;
            p++;
        }
        if (k == 0) return false;
        out[k] = '\0';
        return true;
    }
    return false;
}

/* ============================================================================
 * §2. NVS blob 编解码
 * ========================================================================= */

/* 读 blob 到 buf；失败时 *out_len = 0、返回 ESP_ERR_NVS_NOT_FOUND 等。 */
static esp_err_t mb_load_blob(const char *app_id, uint8_t *buf, size_t *out_len)
{
    size_t cap = BLOB_MAX_LEN;
    esp_err_t err = persist_get_blob(NS_MAILBOX, app_id, buf, &cap);
    if (err != ESP_OK) {
        *out_len = 0;
        return err;
    }
    *out_len = cap;
    return ESP_OK;
}

static esp_err_t mb_save_blob(const char *app_id, const uint8_t *buf, size_t len)
{
    return persist_set_blob(NS_MAILBOX, app_id, buf, len);
}

static esp_err_t mb_erase(const char *app_id)
{
    /* persist 没有 erase_key 接口，用 set_blob 写 0 字节 = 等价于"删除/置空"
     * 但 NVS 不允许 0 长度 blob。改成写一个 1B "空" 标记（version=0 即视为空）。 */
    uint8_t marker = 0;
    return persist_set_blob(NS_MAILBOX, app_id, &marker, 1);
}

/* 把现有 blob 解析为 (count, entries[])，append 一条新 entry，超出容量丢最老。
 * 返回新 blob 的 buf + len（buf 由调用方提供）。 */
static size_t mb_append_entry(uint8_t *buf, size_t cur_len,
                              const uint8_t *new_payload, uint16_t new_len)
{
    /* 解析现有：跳过版本和 count；保留 entries 段 */
    uint8_t count = 0;
    size_t  entries_off = 0;   /* entries 在 buf 中起始偏移（旧 blob） */
    size_t  entries_len = 0;
    if (cur_len >= BLOB_HDR_LEN && buf[0] == BLOB_VERSION) {
        count = buf[1];
        entries_off = BLOB_HDR_LEN;
        entries_len = cur_len - BLOB_HDR_LEN;
    } else {
        /* 空 / marker / 版本不识别：当成全新 */
        count = 0;
        entries_off = 0;
        entries_len = 0;
    }

    /* 容量满：从前面砍掉若干条。简单做法：解析每条头部得到长度，丢第一条直到
     * count < MAX。 */
    while (count >= DYNAPP_MAILBOX_PER_APP_MAX && entries_len >= ENTRY_HDR_LEN) {
        uint16_t l = (uint16_t)buf[entries_off] |
                     ((uint16_t)buf[entries_off + 1] << 8);
        size_t consumed = ENTRY_HDR_LEN + l;
        if (consumed > entries_len) break;   /* 损坏：保护 */
        entries_off += consumed;
        entries_len -= consumed;
        count--;
    }

    /* 现在的 entries 在 buf[entries_off..entries_off+entries_len]
     * 把它平移到 buf[BLOB_HDR_LEN..]，然后在尾部 append 新 entry */
    if (entries_off != BLOB_HDR_LEN) {
        memmove(buf + BLOB_HDR_LEN, buf + entries_off, entries_len);
    }

    size_t write_off = BLOB_HDR_LEN + entries_len;
    /* 防止溢出 */
    if (write_off + ENTRY_HDR_LEN + new_len > BLOB_MAX_LEN) {
        ESP_LOGW(TAG, "blob would overflow even after evict, dropping new");
        return 0;
    }

    buf[write_off++] = (uint8_t)(new_len & 0xFF);
    buf[write_off++] = (uint8_t)(new_len >> 8);
    memcpy(buf + write_off, new_payload, new_len);
    write_off += new_len;

    /* 写头 */
    buf[0] = BLOB_VERSION;
    buf[1] = count + 1;

    return write_off;
}

/* 把 app 的 blob 全部 push 回 s_inbox，然后 erase。 */
static void mb_replay_to_inbox(const char *app_id)
{
    uint8_t *buf = (uint8_t *)heap_caps_malloc(BLOB_MAX_LEN, MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE(TAG, "replay: alloc %d failed", BLOB_MAX_LEN);
        return;
    }
    size_t blob_len = 0;
    esp_err_t err = mb_load_blob(app_id, buf, &blob_len);
    if (err != ESP_OK || blob_len < BLOB_HDR_LEN || buf[0] != BLOB_VERSION) {
        free(buf);
        return;   /* 没东西可放，正常 */
    }

    uint8_t count = buf[1];
    size_t  off = BLOB_HDR_LEN;
    int     replayed = 0;

    for (uint8_t i = 0; i < count && off + ENTRY_HDR_LEN <= blob_len; ++i) {
        uint16_t l = (uint16_t)buf[off] | ((uint16_t)buf[off + 1] << 8);
        off += ENTRY_HDR_LEN;
        if (l == 0 || l > DYNAPP_BRIDGE_MAX_PAYLOAD || off + l > blob_len) {
            ESP_LOGW(TAG, "replay: corrupt entry at %u, stop", (unsigned)off);
            break;
        }
        if (!dynapp_bridge_push_inbox(buf + off, l)) {
            ESP_LOGW(TAG, "replay: inbox full at %d/%u, drop rest",
                     replayed, count);
            break;
        }
        off += l;
        replayed++;
    }

    free(buf);
    mb_erase(app_id);
    if (replayed > 0) {
        ESP_LOGI(TAG, "replay %s: pushed %d msgs back to inbox", app_id, replayed);
    }
}

/* ============================================================================
 * §3. mailbox task：drain inbox → 归档 NVS
 * ========================================================================= */

static void handle_inbox_msg(const dynapp_bridge_msg_t *msg)
{
    char app_id[APP_ID_MAX];
    if (!peek_to_app_id(msg->data, msg->len, app_id, sizeof(app_id))) {
        /* 打印前 60B 帮调试：能看出是 to:? 还是别的格式 */
        char preview[64];
        size_t plen = msg->len < sizeof(preview) - 1 ? msg->len : sizeof(preview) - 1;
        memcpy(preview, msg->data, plen);
        preview[plen] = '\0';
        ESP_LOGW(TAG, "drop msg without parsable to:xxx (%uB) head=%s",
                 msg->len, preview);
        return;
    }

    uint8_t *buf = (uint8_t *)heap_caps_malloc(BLOB_MAX_LEN, MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE(TAG, "alloc %d failed", BLOB_MAX_LEN);
        return;
    }
    size_t cur_len = 0;
    mb_load_blob(app_id, buf, &cur_len);   /* 不存在也 OK，cur_len=0 */

    size_t new_len = mb_append_entry(buf, cur_len, msg->data, msg->len);
    if (new_len > 0) {
        esp_err_t err = mb_save_blob(app_id, buf, new_len);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "save %s err=0x%x (len=%u)",
                     app_id, err, (unsigned)new_len);
        } else {
            ESP_LOGI(TAG, "archived to %s (count=%u, blob=%uB)",
                     app_id, buf[1], (unsigned)new_len);
        }
    }
    free(buf);
}

static void mailbox_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "mailbox task started");

    while (1) {
        /* 最高优先级：处理 replay 请求 */
        replay_req_t req;
        if (xQueueReceive(s_replay_q, &req, 0) == pdTRUE) {
            mb_replay_to_inbox(req.app_id);
            if (req.done) xSemaphoreGive(req.done);
            continue;
        }

        /* JS 在跑就让位（让 script_task 自己 drain inbox） */
        if (atomic_load(&s_js_active)) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        /* JS 没起：自己 drain + 归档 */
        dynapp_bridge_msg_t msg;
        if (dynapp_bridge_pop_inbox(&msg)) {
            handle_inbox_msg(&msg);
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

/* ============================================================================
 * §4. 公共 API
 * ========================================================================= */

esp_err_t dynapp_mailbox_init(void)
{
    if (s_task) return ESP_OK;

    s_replay_q = xQueueCreate(2, sizeof(replay_req_t));
    if (!s_replay_q) {
        ESP_LOGE(TAG, "replay queue alloc failed");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t r = xTaskCreatePinnedToCore(
        mailbox_task, "dynapp_mb", 4096, NULL, 3, &s_task, 0);
    if (r != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "init done (per-app cap=%d, blob max=%dB)",
             DYNAPP_MAILBOX_PER_APP_MAX, BLOB_MAX_LEN);
    return ESP_OK;
}

void dynapp_mailbox_set_js_active(bool active)
{
    atomic_store(&s_js_active, active);
}

void dynapp_mailbox_replay(const char *app_id)
{
    if (!s_task || !s_replay_q || !app_id || !app_id[0]) return;

    replay_req_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.app_id, app_id, APP_ID_MAX - 1);
    req.done = xSemaphoreCreateBinary();
    if (!req.done) {
        ESP_LOGW(TAG, "replay sem alloc failed, skip");
        return;
    }

    if (xQueueSend(s_replay_q, &req, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "replay enqueue timeout for %s", app_id);
        vSemaphoreDelete(req.done);
        return;
    }

    if (xSemaphoreTake(req.done, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW(TAG, "replay wait timeout for %s (continuing)", app_id);
        /* 注意：sem 不能删除，因为 mailbox task 仍持有引用。漏掉一个 binary
         * sem 不致命；后续 replay 会重新申请。 */
        return;
    }
    vSemaphoreDelete(req.done);
}
