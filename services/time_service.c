#include "time_service.h"
#include "time_manager.h"
#include "ble_driver.h"

#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "services/gatt/ble_svc_gatt.h"
#include <time.h>
#include <sys/time.h>
#include <string.h>

static const char *TAG = "time_service";

/* Current Time Service UUID: 0x1805 */
/* Current Time Characteristic UUID: 0x2A2B */

/* BLE Current Time 数据结构（符合 BLE CTS 标准） */
typedef struct {
    uint16_t year;        // 年份（1582-9999）
    uint8_t month;        // 月份（1-12）
    uint8_t day;          // 日期（1-31）
    uint8_t hour;         // 小时（0-23）
    uint8_t minute;       // 分钟（0-59）
    uint8_t second;       // 秒（0-59）
    uint8_t day_of_week;  // 星期（1=周一, 7=周日, 0=未知）
    uint8_t fractions256; // 1/256 秒（0-255）
    uint8_t adjust_reason;// 调整原因（位掩码）
} __attribute__((packed)) ble_cts_current_time_t;

/* 特征值句柄 */
static uint16_t current_time_val_handle;

/* 反向请求 seq，单调递增（PC 端去重） */
static uint8_t s_req_seq = 0;

/* 前向声明 */
static int current_time_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt *ctxt, void *arg);
static void time_service_on_subscribe(uint16_t attr_handle,
                                      uint8_t prev_notify,
                                      uint8_t cur_notify);

/* ============================================================================
 * 时间格式转换函数
 * ============================================================================ */

static void system_time_to_cts(ble_cts_current_time_t *cts_time)
{
    struct timeval tv;
    struct tm timeinfo;

    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &timeinfo);

    cts_time->year = timeinfo.tm_year + 1900;
    cts_time->month = timeinfo.tm_mon + 1;
    cts_time->day = timeinfo.tm_mday;
    cts_time->hour = timeinfo.tm_hour;
    cts_time->minute = timeinfo.tm_min;
    cts_time->second = timeinfo.tm_sec;
    cts_time->day_of_week = (timeinfo.tm_wday == 0) ? 7 : timeinfo.tm_wday;
    cts_time->fractions256 = (tv.tv_usec * 256) / 1000000;
    cts_time->adjust_reason = 0;
}

static esp_err_t cts_to_system_time(const ble_cts_current_time_t *cts_time)
{
    struct tm timeinfo = {
        .tm_year = cts_time->year - 1900,
        .tm_mon = cts_time->month - 1,
        .tm_mday = cts_time->day,
        .tm_hour = cts_time->hour,
        .tm_min = cts_time->minute,
        .tm_sec = cts_time->second
    };

    time_t t = mktime(&timeinfo);
    if (t == -1) {
        ESP_LOGE(TAG, "Invalid time data");
        return ESP_FAIL;
    }

    struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
    esp_err_t ret = time_manager_request_set_time(&tv);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to request time update: %d", ret);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "System time update requested: %04d-%02d-%02d %02d:%02d:%02d",
             cts_time->year, cts_time->month, cts_time->day,
             cts_time->hour, cts_time->minute, cts_time->second);

    return ESP_OK;
}

/* ============================================================================
 * GATT 特征值访问回调
 * ============================================================================ */

static int current_time_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    int rc;
    ble_cts_current_time_t cts_time;

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR:
        ESP_LOGI(TAG, "Current Time read request");

        /* 读取系统时间并转换为 CTS 格式 */
        system_time_to_cts(&cts_time);

        /* 将数据追加到响应缓冲区 */
        rc = os_mbuf_append(ctxt->om, &cts_time, sizeof(cts_time));
        if (rc != 0) {
            ESP_LOGE(TAG, "Failed to append data, error: %d", rc);
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }

        ESP_LOGI(TAG, "Current Time sent: %04d-%02d-%02d %02d:%02d:%02d",
                 cts_time.year, cts_time.month, cts_time.day,
                 cts_time.hour, cts_time.minute, cts_time.second);
        return 0;

    case BLE_GATT_ACCESS_OP_WRITE_CHR:
        ESP_LOGI(TAG, "Current Time write request");

        /* 检查数据长度 */
        if (OS_MBUF_PKTLEN(ctxt->om) != sizeof(cts_time)) {
            ESP_LOGE(TAG, "Invalid data length: %d (expected %d)",
                     OS_MBUF_PKTLEN(ctxt->om), sizeof(cts_time));
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }

        /* 从缓冲区读取数据 */
        rc = ble_hs_mbuf_to_flat(ctxt->om, &cts_time, sizeof(cts_time), NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "Failed to read data, error: %d", rc);
            return BLE_ATT_ERR_UNLIKELY;
        }

        /* 更新系统时间 */
        if (cts_to_system_time(&cts_time) != ESP_OK) {
            return BLE_ATT_ERR_UNLIKELY;
        }

        return 0;

    default:
        ESP_LOGW(TAG, "Unsupported operation: %d", ctxt->op);
        return BLE_ATT_ERR_UNLIKELY;
    }
}

/* ============================================================================
 * GATT 服务定义
 * ============================================================================ */

static const struct ble_gatt_svc_def time_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x1805),  /* Current Time Service */
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = BLE_UUID16_DECLARE(0x2A2B),  /* Current Time */
                .access_cb = current_time_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &current_time_val_handle
            },
            {0}  /* 特征值结束标记 */
        }
    },
    {0}  /* 服务结束标记 */
};

/* ============================================================================
 * 公共接口函数
 * ============================================================================ */

esp_err_t time_service_init(void)
{
    int rc;

    ESP_LOGI(TAG, "Initializing BLE Time Service");

    /* 计算服务配置 */
    rc = ble_gatts_count_cfg(time_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to count service config, error: %d", rc);
        return ESP_FAIL;
    }

    /* 添加服务到 GATT 服务器 */
    rc = ble_gatts_add_svcs(time_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to add services, error: %d", rc);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "BLE Time Service initialized successfully");
    ESP_LOGI(TAG, "Service UUID: 0x1805, Characteristic UUID: 0x2A2B");

    /* 订阅 SUBSCRIBE 事件：central 开 notify 时自动触发一次 send_request */
    esp_err_t err = ble_driver_register_subscribe_cb(time_service_on_subscribe);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register subscribe_cb failed: %d", err);
        return err;
    }

    return ESP_OK;
}

esp_err_t time_service_send_request(void)
{
    uint16_t conn_handle;
    if (!ble_driver_get_conn_handle(&conn_handle)) {
        ESP_LOGD(TAG, "not connected, drop time request");
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t payload = ++s_req_seq;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(&payload, sizeof(payload));
    if (!om) {
        ESP_LOGW(TAG, "mbuf alloc failed");
        return ESP_ERR_NO_MEM;
    }

    int rc = ble_gatts_notify_custom(conn_handle, current_time_val_handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "notify failed rc=%d (client may not subscribe)", rc);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "time request sent: seq=%u", payload);
    return ESP_OK;
}

static void time_service_on_subscribe(uint16_t attr_handle,
                                      uint8_t prev_notify,
                                      uint8_t cur_notify)
{
    if (attr_handle != current_time_val_handle) return;
    if (prev_notify || !cur_notify) return;

    ESP_LOGI(TAG, "client subscribed CTS, requesting time sync");
    time_service_send_request();
}
