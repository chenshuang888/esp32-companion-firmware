#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 BLE 通知推送服务
 *
 * 注册自定义 GATT 服务：
 *   Service UUID:        8a5c0003-0000-4aef-b87e-4fa1e0c7e0f6
 *   Characteristic UUID: 8a5c0004-0000-4aef-b87e-4fa1e0c7e0f6  (WRITE)
 *
 * 客户端（PC 端）写入 notification_payload_t 结构体，
 * 收到后会通过 notify_manager_push() 转发到 UI 线程。
 *
 * 必须在 nimble_port_freertos_init() 之前调用。
 */
esp_err_t notify_service_init(void);

#ifdef __cplusplus
}
#endif
