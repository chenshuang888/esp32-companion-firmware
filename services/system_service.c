#include "system_service.h"
#include "system_manager.h"
#include "ble_driver.h"

#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "system_svc";

/* Service UUID: 8a5c0009-0000-4aef-b87e-4fa1e0c7e0f6 (little-endian bytes) */
static const ble_uuid128_t s_system_svc_uuid = BLE_UUID128_INIT(
    0xf6, 0xe0, 0xc7, 0xe0, 0xa1, 0x4f, 0x7e, 0xb8,
    0xef, 0x4a, 0x00, 0x00, 0x09, 0x00, 0x5c, 0x8a
);

/* Write characteristic UUID: 8a5c000a-0000-4aef-b87e-4fa1e0c7e0f6 */
static const ble_uuid128_t s_system_chr_uuid = BLE_UUID128_INIT(
    0xf6, 0xe0, 0xc7, 0xe0, 0xa1, 0x4f, 0x7e, 0xb8,
    0xef, 0x4a, 0x00, 0x00, 0x0a, 0x00, 0x5c, 0x8a
);

/* Notify characteristic UUID: 8a5c000c-0000-4aef-b87e-4fa1e0c7e0f6 */
static const ble_uuid128_t s_system_req_uuid = BLE_UUID128_INIT(
    0xf6, 0xe0, 0xc7, 0xe0, 0xa1, 0x4f, 0x7e, 0xb8,
    0xef, 0x4a, 0x00, 0x00, 0x0c, 0x00, 0x5c, 0x8a
);

static uint16_t s_chr_val_handle;
static uint16_t s_req_val_handle;
static uint8_t  s_req_seq = 0;

/* 前向声明：init 末尾要注册给 ble_driver */
static void system_service_on_subscribe(uint16_t attr_handle,
                                        uint8_t prev_notify,
                                        uint8_t cur_notify);

static int system_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        ESP_LOGW(TAG, "Unsupported op: %d", ctxt->op);
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len != sizeof(system_payload_t)) {
        ESP_LOGE(TAG, "Invalid payload length: %d (expected %d)",
                 len, (int)sizeof(system_payload_t));
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    system_payload_t payload;
    int rc = ble_hs_mbuf_to_flat(ctxt->om, &payload, sizeof(payload), NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to flatten mbuf, error: %d", rc);
        return BLE_ATT_ERR_UNLIKELY;
    }

    /* 投递到队列，UI 线程消费（非阻塞） */
    esp_err_t err = system_manager_push(&payload);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "system_manager_push failed: %d", err);
        return BLE_ATT_ERR_UNLIKELY;
    }

    ESP_LOGD(TAG, "System payload received (%d bytes)", len);
    return 0;
}

/* READ 回调仅用于让客户端能"发现"request char，真正信号走 NOTIFY。
 * 返回最近一次 seq 作心跳占位。 */
static int system_req_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        ESP_LOGW(TAG, "Unsupported op: %d", ctxt->op);
        return BLE_ATT_ERR_UNLIKELY;
    }
    uint8_t snapshot = s_req_seq;
    int rc = os_mbuf_append(ctxt->om, &snapshot, sizeof(snapshot));
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static const struct ble_gatt_svc_def s_system_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_system_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &s_system_chr_uuid.u,
                .access_cb = system_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE,
                .val_handle = &s_chr_val_handle
            },
            {
                .uuid = &s_system_req_uuid.u,
                .access_cb = system_req_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_req_val_handle
            },
            {0}
        }
    },
    {0}
};

esp_err_t system_service_init(void)
{
    int rc;

    ESP_LOGI(TAG, "Initializing BLE System Service");

    rc = ble_gatts_count_cfg(s_system_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to count service config, error: %d", rc);
        return ESP_FAIL;
    }

    rc = ble_gatts_add_svcs(s_system_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to add services, error: %d", rc);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "BLE System Service initialized");
    ESP_LOGI(TAG, "Service UUID: 8a5c0009-0000-4aef-b87e-4fa1e0c7e0f6");

    esp_err_t err = ble_driver_register_subscribe_cb(system_service_on_subscribe);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register subscribe_cb failed: %d", err);
        return err;
    }

    return ESP_OK;
}

esp_err_t system_service_send_request(void)
{
    uint16_t conn_handle;
    if (!ble_driver_get_conn_handle(&conn_handle)) {
        ESP_LOGD(TAG, "not connected, drop system request");
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t payload = ++s_req_seq;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(&payload, sizeof(payload));
    if (!om) {
        ESP_LOGW(TAG, "mbuf alloc failed");
        return ESP_ERR_NO_MEM;
    }

    int rc = ble_gatts_notify_custom(conn_handle, s_req_val_handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "notify failed rc=%d (client may not subscribe)", rc);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "system request sent: seq=%u", payload);
    return ESP_OK;
}

static void system_service_on_subscribe(uint16_t attr_handle,
                                        uint8_t prev_notify,
                                        uint8_t cur_notify)
{
    if (attr_handle != s_req_val_handle) return;
    if (prev_notify || !cur_notify) return;

    ESP_LOGI(TAG, "client subscribed system req, triggering initial push");
    system_service_send_request();
}
