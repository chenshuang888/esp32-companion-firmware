#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 BLE 系统监控服务
 *
 * 注册自定义 GATT 服务：
 *   Service UUID:          8a5c0009-0000-4aef-b87e-4fa1e0c7e0f6
 *   Write characteristic:  8a5c000a-0000-4aef-b87e-4fa1e0c7e0f6  (WRITE)
 *     PC → ESP 写入 system_payload_t（16 字节），ESP 转发到 UI 线程
 *   Notify characteristic: 8a5c000c-0000-4aef-b87e-4fa1e0c7e0f6  (NOTIFY)
 *     ESP → PC 发 1B 递增 seq，语义为"请 PC 立刻推一帧系统数据"
 *
 * 必须在 nimble_port_freertos_init() 之前调用。
 */
esp_err_t system_service_init(void);

/**
 * @brief 发一次反向请求（ESP→PC），要求 PC 立刻推一帧系统数据
 *
 * 未连接 / 未订阅 / mbuf 分配失败时静默返回错误，只打 WARN，不崩溃。
 * body 为 1 字节递增 seq，PC 端可用于去重。
 */
esp_err_t system_service_send_request(void);

#ifdef __cplusplus
}
#endif
