#include "media_service.h"
#include "media_manager.h"
#include "playlist_manager.h"
#include "ble_driver.h"

#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "services/gatt/ble_svc_gatt.h"
#include <string.h>

static const char *TAG = "media_svc";

/* Service UUID: 8a5c0007-...（不变） */
static const ble_uuid128_t s_media_svc_uuid = BLE_UUID128_INIT(
    0xf6, 0xe0, 0xc7, 0xe0, 0xa1, 0x4f, 0x7e, 0xb8,
    0xef, 0x4a, 0x00, 0x00, 0x07, 0x00, 0x5c, 0x8a
);

/* WRITE char UUID: 8a5c0008-...（type 分发） */
static const ble_uuid128_t s_media_chr_uuid = BLE_UUID128_INIT(
    0xf6, 0xe0, 0xc7, 0xe0, 0xa1, 0x4f, 0x7e, 0xb8,
    0xef, 0x4a, 0x00, 0x00, 0x08, 0x00, 0x5c, 0x8a
);

/* NOTIFY char UUID: 8a5c000d-...（type 分发） */
static const ble_uuid128_t s_media_btn_uuid = BLE_UUID128_INIT(
    0xf6, 0xe0, 0xc7, 0xe0, 0xa1, 0x4f, 0x7e, 0xb8,
    0xef, 0x4a, 0x00, 0x00, 0x0d, 0x00, 0x5c, 0x8a
);

static uint16_t s_chr_val_handle;
static uint16_t s_btn_val_handle;
static uint16_t s_btn_seq = 0;
static uint16_t s_play_track_seq = 0;

/* ------------------------------------------------------------------
 * WRITE：按首字节 type 分发
 * ------------------------------------------------------------------ */

/* 最大单包 = 1B type + max(payload, item) */
#define MEDIA_MAX_FRAME_SIZE  (1 + sizeof(media_payload_t))

static int media_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        ESP_LOGW(TAG, "Unsupported op: %d", ctxt->op);
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint16_t total_len = OS_MBUF_PKTLEN(ctxt->om);
    if (total_len < 1 || total_len > MEDIA_MAX_FRAME_SIZE) {
        ESP_LOGE(TAG, "bad len: %u", total_len);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    uint8_t buf[MEDIA_MAX_FRAME_SIZE];
    int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, total_len, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "mbuf flatten failed: %d", rc);
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint8_t   type     = buf[0];
    uint8_t  *body     = buf + 1;
    uint16_t  body_len = total_len - 1;

    switch (type) {
    case MEDIA_MSG_NOWPLAYING: {
        if (body_len != sizeof(media_payload_t)) {
            ESP_LOGE(TAG, "NOWPLAYING bad body_len: %u", body_len);
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        esp_err_t err = media_manager_push((const media_payload_t *)body);
        return (err == ESP_OK) ? 0 : BLE_ATT_ERR_UNLIKELY;
    }
    case MEDIA_MSG_PLAYLIST_BEGIN: {
        if (body_len != sizeof(media_playlist_begin_t)) {
            ESP_LOGE(TAG, "PLAYLIST_BEGIN bad body_len: %u", body_len);
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        const media_playlist_begin_t *b = (const media_playlist_begin_t *)body;
        esp_err_t err = playlist_manager_push_begin(b->total_count, b->version);
        return (err == ESP_OK) ? 0 : BLE_ATT_ERR_UNLIKELY;
    }
    case MEDIA_MSG_PLAYLIST_ITEM: {
        if (body_len != sizeof(media_playlist_item_t)) {
            ESP_LOGE(TAG, "PLAYLIST_ITEM bad body_len: %u", body_len);
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        esp_err_t err = playlist_manager_push_item((const media_playlist_item_t *)body);
        return (err == ESP_OK) ? 0 : BLE_ATT_ERR_UNLIKELY;
    }
    case MEDIA_MSG_PLAYLIST_END: {
        esp_err_t err = playlist_manager_push_end();
        return (err == ESP_OK) ? 0 : BLE_ATT_ERR_UNLIKELY;
    }
    default:
        ESP_LOGW(TAG, "unknown type=0x%02x len=%u", type, total_len);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
}

/* ------------------------------------------------------------------
 * NOTIFY descriptor read（兼容旧实现，返回最近一次 button seq 占位）
 * ------------------------------------------------------------------ */

static int media_btn_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    /* 读返回 [type=BUTTON, id=0, action=0, seq] 5 字节 */
    uint8_t buf[1 + sizeof(media_button_event_t)];
    buf[0] = MEDIA_NOTIFY_BUTTON;
    media_button_event_t snap = {0, 0, s_btn_seq};
    memcpy(buf + 1, &snap, sizeof(snap));
    int rc = os_mbuf_append(ctxt->om, buf, sizeof(buf));
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

/* ------------------------------------------------------------------
 * GATT 表
 * ------------------------------------------------------------------ */

static const struct ble_gatt_svc_def s_media_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_media_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &s_media_chr_uuid.u,
                .access_cb = media_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE,
                .val_handle = &s_chr_val_handle
            },
            {
                .uuid = &s_media_btn_uuid.u,
                .access_cb = media_btn_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_btn_val_handle
            },
            {0}
        }
    },
    {0}
};

esp_err_t media_service_init(void)
{
    int rc;
    ESP_LOGI(TAG, "Initializing BLE Media Service (v2 type-tagged)");
    rc = ble_gatts_count_cfg(s_media_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "count_cfg failed: %d", rc); return ESP_FAIL; }
    rc = ble_gatts_add_svcs(s_media_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "add_svcs failed: %d", rc); return ESP_FAIL; }
    ESP_LOGI(TAG, "Service UUID: 8a5c0007-...");
    return ESP_OK;
}

/* ------------------------------------------------------------------
 * NOTIFY 发送
 * ------------------------------------------------------------------ */

static esp_err_t notify_with_type(uint8_t type, const void *body, size_t body_len)
{
    uint16_t conn_handle;
    if (!ble_driver_get_conn_handle(&conn_handle)) {
        ESP_LOGD(TAG, "not connected, drop notify type=0x%02x", type);
        return ESP_ERR_INVALID_STATE;
    }
    /* 拼 [type | body] */
    uint8_t buf[1 + 16];
    if (1 + body_len > sizeof(buf)) return ESP_ERR_INVALID_SIZE;
    buf[0] = type;
    memcpy(buf + 1, body, body_len);

    struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, 1 + body_len);
    if (!om) {
        ESP_LOGW(TAG, "mbuf alloc failed");
        return ESP_ERR_NO_MEM;
    }
    int rc = ble_gatts_notify_custom(conn_handle, s_btn_val_handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "notify failed rc=%d (client may not subscribe)", rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t media_service_send_button(uint8_t id)
{
    media_button_event_t evt = {
        .id     = id,
        .action = MEDIA_BTN_ACTION_PRESS,
        .seq    = ++s_btn_seq,
    };
    esp_err_t r = notify_with_type(MEDIA_NOTIFY_BUTTON, &evt, sizeof(evt));
    if (r == ESP_OK) {
        ESP_LOGI(TAG, "button sent: id=%u seq=%u", id, evt.seq);
    }
    return r;
}

esp_err_t media_service_send_play_track(uint16_t track_index)
{
    media_play_track_event_t evt = {
        .track_index = track_index,
        .seq         = ++s_play_track_seq,
    };
    esp_err_t r = notify_with_type(MEDIA_NOTIFY_PLAY_TRACK, &evt, sizeof(evt));
    if (r == ESP_OK) {
        ESP_LOGI(TAG, "play_track sent: idx=%u seq=%u", track_index, evt.seq);
    }
    return r;
}
