#include "notify_service.h"
#include "notify_manager.h"

#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "notify_svc";

/* Service UUID: 8a5c0003-0000-4aef-b87e-4fa1e0c7e0f6 (little-endian bytes) */
static const ble_uuid128_t s_notify_svc_uuid = BLE_UUID128_INIT(
    0xf6, 0xe0, 0xc7, 0xe0, 0xa1, 0x4f, 0x7e, 0xb8,
    0xef, 0x4a, 0x00, 0x00, 0x03, 0x00, 0x5c, 0x8a
);

/* Characteristic UUID: 8a5c0004-0000-4aef-b87e-4fa1e0c7e0f6 */
static const ble_uuid128_t s_notify_chr_uuid = BLE_UUID128_INIT(
    0xf6, 0xe0, 0xc7, 0xe0, 0xa1, 0x4f, 0x7e, 0xb8,
    0xef, 0x4a, 0x00, 0x00, 0x04, 0x00, 0x5c, 0x8a
);

static uint16_t s_chr_val_handle;

static int notify_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        ESP_LOGW(TAG, "Unsupported op: %d", ctxt->op);
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len != sizeof(notification_payload_t)) {
        ESP_LOGE(TAG, "Invalid payload length: %d (expected %d)",
                 len, (int)sizeof(notification_payload_t));
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    notification_payload_t payload;
    int rc = ble_hs_mbuf_to_flat(ctxt->om, &payload, sizeof(payload), NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to flatten mbuf, error: %d", rc);
        return BLE_ATT_ERR_UNLIKELY;
    }

    esp_err_t err = notify_manager_push(&payload);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "notify_manager_push failed: %d", err);
        return BLE_ATT_ERR_UNLIKELY;
    }

    ESP_LOGI(TAG, "Notification payload received (%d bytes)", len);
    return 0;
}

static const struct ble_gatt_svc_def s_notify_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_notify_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &s_notify_chr_uuid.u,
                .access_cb = notify_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE,
                .val_handle = &s_chr_val_handle
            },
            {0}  /* characteristic end */
        }
    },
    {0}  /* service end */
};

esp_err_t notify_service_init(void)
{
    int rc;

    ESP_LOGI(TAG, "Initializing BLE Notify Service");

    rc = ble_gatts_count_cfg(s_notify_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to count service config, error: %d", rc);
        return ESP_FAIL;
    }

    rc = ble_gatts_add_svcs(s_notify_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to add services, error: %d", rc);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "BLE Notify Service initialized");
    ESP_LOGI(TAG, "Service UUID: 8a5c0003-0000-4aef-b87e-4fa1e0c7e0f6");

    return ESP_OK;
}
