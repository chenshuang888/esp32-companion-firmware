#include "ble_driver.h"

#include <string.h>
#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "host/ble_store.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

/* 外部库函数声明 */
void ble_store_config_init(void);

static const char *TAG = "ble_driver";

#define BLE_DEVICE_NAME        "ESP32-S3-DEMO"
#define BLE_DRIVER_MAX_SUB_CBS 8

/* SUBSCRIBE 回调数组：service 自己在 init 阶段注册，GAP 事件触发时遍历分发。 */
static ble_driver_subscribe_cb_t s_sub_cbs[BLE_DRIVER_MAX_SUB_CBS];
static size_t                    s_sub_cb_count = 0;

/* 连接状态：BLE host 线程写、UI 线程读 —— 单写多读标量走 volatile 契约
 * （参考 spec/iot/guides/nimble-ui-thread-communication-contract.md）。 */
static volatile bool     s_is_connected = false;
static volatile uint16_t s_conn_handle  = 0;

/* 前向声明 */
static void ble_host_task(void *param);
static void on_stack_reset(int reason);
static void on_stack_sync(void);
static int  gap_event_handler(struct ble_gap_event *event, void *arg);

/* ============================================================================
 * BLE 协议栈回调函数
 * ============================================================================ */

static void on_stack_reset(int reason)
{
    ESP_LOGI(TAG, "BLE stack reset, reason: %d", reason);
}

static void on_stack_sync(void)
{
    int rc;

    ESP_LOGI(TAG, "BLE stack synced");

    rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to ensure address, error: %d", rc);
        return;
    }

    uint8_t addr[6] = {0};
    rc = ble_hs_id_infer_auto(0, &addr[0]);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to infer address, error: %d", rc);
        return;
    }

    ESP_LOGI(TAG, "Device address: %02x:%02x:%02x:%02x:%02x:%02x",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);

    ble_driver_start_advertising();
}

static int gap_event_handler(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "Connection %s; status=%d",
                 event->connect.status == 0 ? "established" : "failed",
                 event->connect.status);

        if (event->connect.status == 0) {
            /* 先写 handle 再置 connected，避免 reader 看到"已连接但 handle 无效"的中间态。 */
            s_conn_handle  = event->connect.conn_handle;
            s_is_connected = true;
        } else {
            ble_driver_start_advertising();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Disconnected; reason=%d", event->disconnect.reason);
        s_is_connected = false;
        s_conn_handle  = 0;

        ble_driver_start_advertising();
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "Advertising complete");
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "Subscribe event; conn_handle=%d attr_handle=%d cur_notify=%d",
                 event->subscribe.conn_handle,
                 event->subscribe.attr_handle,
                 event->subscribe.cur_notify);
        /* 广播分发：每个注册 cb 自判 attr_handle 是否属于自己，不匹配即返回。 */
        for (size_t i = 0; i < s_sub_cb_count; i++) {
            s_sub_cbs[i](event->subscribe.attr_handle,
                         event->subscribe.prev_notify,
                         event->subscribe.cur_notify);
        }
        break;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU update event; conn_handle=%d mtu=%d",
                 event->mtu.conn_handle,
                 event->mtu.value);
        break;

    default:
        break;
    }

    return 0;
}

/* ============================================================================
 * BLE 主机任务
 * ============================================================================ */

static void ble_host_task(void *param)
{
    ESP_LOGI(TAG, "BLE host task started");

    /* 运行 NimBLE 主机，阻塞直到 nimble_port_stop() */
    nimble_port_run();

    nimble_port_freertos_deinit();
}

/* ============================================================================
 * 公共接口函数
 * ============================================================================ */

esp_err_t ble_driver_nimble_init(void)
{
    int rc;

    ESP_LOGI(TAG, "Initializing NimBLE stack (GATT registration window open)");

    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NimBLE stack, error: %d", ret);
        return ESP_FAIL;
    }

    ble_svc_gap_init();
    ble_svc_gatt_init();

    rc = ble_svc_gap_device_name_set(BLE_DEVICE_NAME);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to set device name, error: %d", rc);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "NimBLE stack ready; register GATT services now");
    return ESP_OK;
}

esp_err_t ble_driver_nimble_start(void)
{
    ESP_LOGI(TAG, "Starting NimBLE host task");

    ble_hs_cfg.reset_cb          = on_stack_reset;
    ble_hs_cfg.sync_cb           = on_stack_sync;
    ble_hs_cfg.gatts_register_cb = NULL;
    ble_hs_cfg.store_status_cb   = ble_store_util_status_rr;

    ble_store_config_init();

    /* 启动 host task；sync 回调触发时会自动调用 start_advertising() */
    nimble_port_freertos_init(ble_host_task);

    ESP_LOGI(TAG, "NimBLE host task started");
    return ESP_OK;
}

esp_err_t ble_driver_start_advertising(void)
{
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields  fields;
    int                       rc;

    ESP_LOGI(TAG, "Starting BLE advertising");

    memset(&fields, 0, sizeof(fields));
    fields.flags            = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name             = (uint8_t *)BLE_DEVICE_NAME;
    fields.name_len         = strlen(BLE_DEVICE_NAME);
    fields.name_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to set advertising data, error: %d", rc);
        return ESP_FAIL;
    }

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.itvl_min  = 0x20; /* 20 ms */
    adv_params.itvl_max  = 0x40; /* 40 ms */

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                           &adv_params, gap_event_handler, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to start advertising, error: %d", rc);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "BLE advertising started");
    return ESP_OK;
}

esp_err_t ble_driver_stop_advertising(void)
{
    int rc;

    ESP_LOGI(TAG, "Stopping BLE advertising");

    rc = ble_gap_adv_stop();
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to stop advertising, error: %d", rc);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "BLE advertising stopped");
    return ESP_OK;
}

bool ble_driver_is_connected(void)
{
    return s_is_connected;
}

bool ble_driver_get_conn_handle(uint16_t *out)
{
    if (!s_is_connected) {
        return false;
    }
    if (out) {
        *out = s_conn_handle;
    }
    return true;
}

esp_err_t ble_driver_register_subscribe_cb(ble_driver_subscribe_cb_t cb)
{
    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_sub_cb_count >= BLE_DRIVER_MAX_SUB_CBS) {
        ESP_LOGE(TAG, "subscribe_cb array full (max=%d)", BLE_DRIVER_MAX_SUB_CBS);
        return ESP_ERR_NO_MEM;
    }
    s_sub_cbs[s_sub_cb_count++] = cb;
    return ESP_OK;
}
