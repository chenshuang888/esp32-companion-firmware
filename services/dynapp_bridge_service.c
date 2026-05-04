/* ============================================================================
 * dynapp_bridge_service.c —— Dynamic App 的通用 BLE 透传管道
 *
 * 设计原则：
 *   - 完全透明：C 不解析 payload，只搬字节
 *   - 跨线程安全：用 FreeRTOS queue 做 host task ↔ script task 隔离
 *   - 无需运行时增删 GATT：开机注册一次，所有 dynamic app 共用
 *
 * 详见 dynapp_bridge_service.h 顶部说明。
 * ========================================================================= */

#include "dynapp_bridge_service.h"
#include "ble_driver.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "dynapp_bridge";

/* Service UUID:  a3a30001-0000-4aef-b87e-4fa1e0c7e0f6
 * 选 a3a3 段是为了和 8a5c 段（已有 5 个原生 service）做视觉区分 */
static const ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(
    0xf6, 0xe0, 0xc7, 0xe0, 0xa1, 0x4f, 0x7e, 0xb8,
    0xef, 0x4a, 0x00, 0x00, 0x01, 0x00, 0xa3, 0xa3
);

/* RX char (PC → ESP, WRITE):  a3a30002-... */
static const ble_uuid128_t s_rx_uuid = BLE_UUID128_INIT(
    0xf6, 0xe0, 0xc7, 0xe0, 0xa1, 0x4f, 0x7e, 0xb8,
    0xef, 0x4a, 0x00, 0x00, 0x02, 0x00, 0xa3, 0xa3
);

/* TX char (ESP → PC, READ + NOTIFY):  a3a30003-... */
static const ble_uuid128_t s_tx_uuid = BLE_UUID128_INIT(
    0xf6, 0xe0, 0xc7, 0xe0, 0xa1, 0x4f, 0x7e, 0xb8,
    0xef, 0x4a, 0x00, 0x00, 0x03, 0x00, 0xa3, 0xa3
);

static uint16_t s_rx_val_handle;
static uint16_t s_tx_val_handle;

/* tx 是否被 PC 订阅。on_subscribe 维护，send 时检查。 */
static volatile bool s_tx_subscribed = false;

/* inbox：host task 写、script task 读 */
static QueueHandle_t s_inbox = NULL;

/* ============================================================================
 * §1. GATT access_cb
 * ========================================================================= */

/* PC 写 rx char 时被调（NimBLE host task 上下文）。
 * 这里不能阻塞 —— 拷贝出来塞进 inbox 立刻返回。 */
static int rx_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                        struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        ESP_LOGW(TAG, "rx: unsupported op %d", ctxt->op);
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len == 0 || len > DYNAPP_BRIDGE_MAX_PAYLOAD) {
        ESP_LOGW(TAG, "rx: bad len=%u (max %d)", len, DYNAPP_BRIDGE_MAX_PAYLOAD);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    dynapp_bridge_msg_t msg;
    msg.len = len;
    int rc = ble_hs_mbuf_to_flat(ctxt->om, msg.data, sizeof(msg.data), NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "rx: mbuf flatten failed rc=%d", rc);
        return BLE_ATT_ERR_UNLIKELY;
    }

    /* inbox 满策略：丢最老的，让新消息进。
     *
     * 为什么不让新消息丢：BLE 写命令是 PC 主动行为（write-without-response），
     * NimBLE 不会反压；如果丢新消息，PC 完全无感继续灌，inbox 永远卡老消息。
     * 丢老消息至少能保证 app 收到的是"近期的"。 */
    if (xQueueSend(s_inbox, &msg, 0) != pdTRUE) {
        dynapp_bridge_msg_t drop;
        if (xQueueReceive(s_inbox, &drop, 0) == pdTRUE) {
            ESP_LOGW(TAG, "rx: inbox full, dropped 1 old msg");
        }
        if (xQueueSend(s_inbox, &msg, 0) != pdTRUE) {
            /* 极端情况：刚 pop 又被另一线程填满。直接丢这条新的。 */
            ESP_LOGW(TAG, "rx: still full after evict, dropping new");
        }
    }
    return 0;
}

/* PC 读 tx char 占位实现（实际数据走 NOTIFY；READ 主要是为了让 PC 端能
 * 通过标准方式发现这个 char）。返回 1 byte 心跳。 */
static int tx_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                        struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    uint8_t z = 0;
    int rc = os_mbuf_append(ctxt->om, &z, 1);
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

/* ============================================================================
 * §2. GATT 表
 * ========================================================================= */

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid       = &s_rx_uuid.u,
                .access_cb  = rx_access_cb,
                .flags      = BLE_GATT_CHR_F_WRITE,
                .val_handle = &s_rx_val_handle,
            },
            {
                .uuid       = &s_tx_uuid.u,
                .access_cb  = tx_access_cb,
                .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_tx_val_handle,
            },
            { 0 }
        }
    },
    { 0 }
};

/* ============================================================================
 * §3. 订阅状态跟踪
 * ========================================================================= */

static void on_subscribe(uint16_t attr_handle, uint8_t prev_notify, uint8_t cur_notify)
{
    if (attr_handle != s_tx_val_handle) return;
    s_tx_subscribed = (cur_notify != 0);
    ESP_LOGI(TAG, "tx subscribe state -> %d", (int)s_tx_subscribed);
}

/* ============================================================================
 * §4. 公共 API
 * ========================================================================= */

esp_err_t dynapp_bridge_service_init(void)
{
    ESP_LOGI(TAG, "Initializing BLE Dynamic App Bridge");

    if (!s_inbox) {
        s_inbox = xQueueCreate(DYNAPP_BRIDGE_INBOX_LEN, sizeof(dynapp_bridge_msg_t));
        if (!s_inbox) {
            ESP_LOGE(TAG, "inbox alloc failed");
            return ESP_ERR_NO_MEM;
        }
    }

    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "count_cfg failed rc=%d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "add_svcs failed rc=%d", rc);
        return ESP_FAIL;
    }

    esp_err_t err = ble_driver_register_subscribe_cb(on_subscribe);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register subscribe_cb failed: %d", err);
        return err;
    }

    ESP_LOGI(TAG, "Service UUID: a3a30001-0000-4aef-b87e-4fa1e0c7e0f6 (rx + tx)");
    return ESP_OK;
}

bool dynapp_bridge_pop_inbox(dynapp_bridge_msg_t *out)
{
    if (!s_inbox || !out) return false;
    return xQueueReceive(s_inbox, out, 0) == pdTRUE;
}

void dynapp_bridge_clear_inbox(void)
{
    if (!s_inbox) return;
    xQueueReset(s_inbox);
}

bool dynapp_bridge_push_inbox(const uint8_t *data, uint16_t len)
{
    if (!s_inbox || !data || len == 0 || len > DYNAPP_BRIDGE_MAX_PAYLOAD) {
        return false;
    }
    dynapp_bridge_msg_t msg;
    msg.len = len;
    memcpy(msg.data, data, len);
    return xQueueSend(s_inbox, &msg, 0) == pdTRUE;
}

bool dynapp_bridge_send(const uint8_t *data, size_t len)
{
    if (!data || len == 0 || len > DYNAPP_BRIDGE_MAX_PAYLOAD) return false;
    if (!s_tx_subscribed) return false;

    uint16_t conn_handle;
    if (!ble_driver_get_conn_handle(&conn_handle)) return false;

    /* mbuf_from_flat 内部会拷贝 data，调用方栈上的 buffer 安全 */
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (!om) {
        ESP_LOGW(TAG, "send: mbuf alloc failed");
        return false;
    }

    /* ble_gatts_notify_custom 是线程安全的（NimBLE 内部锁） */
    int rc = ble_gatts_notify_custom(conn_handle, s_tx_val_handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "send: notify failed rc=%d", rc);
        return false;
    }
    return true;
}

bool dynapp_bridge_is_connected(void)
{
    uint16_t h;
    return ble_driver_get_conn_handle(&h);
}
