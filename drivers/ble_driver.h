#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * SUBSCRIBE 事件回调类型。
 * 当 central 订阅/取消订阅任一 char 时，所有已注册的 cb 都会被调用；
 * cb 内部按 attr_handle 匹配自己关心的 char（其他 service 的事件应忽略）。
 */
typedef void (*ble_driver_subscribe_cb_t)(uint16_t attr_handle,
                                          uint8_t prev_notify,
                                          uint8_t cur_notify);

/**
 * @brief 初始化 NimBLE 协议栈基础设施
 *
 * 执行：nimble_port_init + ble_svc_gap_init + ble_svc_gatt_init + 设备名设置。
 * 调用完成后即可通过 ble_gatts_add_svcs() 注册 GATT 表（此时 host task 还没启动）。
 *
 * @return ESP_OK / ESP_FAIL
 */
esp_err_t ble_driver_nimble_init(void);

/**
 * @brief 启动 NimBLE host task（并随之触发 sync → advertising）
 *
 * 执行：ble_hs_cfg 配置 + ble_store_config_init + nimble_port_freertos_init。
 * 必须在所有 service 的 GATT 表注册完成后调用。
 *
 * @return ESP_OK / ESP_FAIL
 */
esp_err_t ble_driver_nimble_start(void);

/**
 * @brief 启动 BLE 广播
 */
esp_err_t ble_driver_start_advertising(void);

/**
 * @brief 停止 BLE 广播
 */
esp_err_t ble_driver_stop_advertising(void);

/**
 * @brief 查询当前是否有 central 连接
 */
bool ble_driver_is_connected(void);

/**
 * @brief 获取当前 conn_handle（供 service 发 notify）
 *
 * @param out  若已连接，写入当前 conn_handle；未连接时 out 不修改
 * @return true = 已连接；false = 未连接（调用方应放弃发送）
 */
bool ble_driver_get_conn_handle(uint16_t *out);

/**
 * @brief 注册 SUBSCRIBE 事件回调
 *
 * 仅在 ble_driver_nimble_init 与 ble_driver_nimble_start 之间的 service_init 阶段调用。
 * 运行期之后不应再注册（内部数组无锁保护）。
 *
 * @return ESP_OK / ESP_ERR_NO_MEM（数组已满）/ ESP_ERR_INVALID_ARG（cb 为 NULL）
 */
esp_err_t ble_driver_register_subscribe_cb(ble_driver_subscribe_cb_t cb);

#ifdef __cplusplus
}
#endif
