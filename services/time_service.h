#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 BLE 时间服务
 *
 * 注册标准 Current Time Service (UUID: 0x1805, char 0x2A2B)：
 *   - WRITE  : PC → ESP, 写入 10B CTS payload 同步系统时间
 *   - NOTIFY : ESP → PC, 1B 递增 seq 哨兵，语义为"请 PC 推一次 CTS"
 *
 * @return
 *      - ESP_OK: 成功
 *      - ESP_FAIL: 失败
 */
esp_err_t time_service_init(void);

/**
 * @brief 发一次反向请求（ESP→PC），要求 PC 写入当前 CTS 时间
 *
 * 未连接 / 未订阅 / mbuf 分配失败时静默返回错误，只打 WARN，不崩溃。
 * body 为 1 字节递增 seq，PC 端据此去重。
 */
esp_err_t time_service_send_request(void);

#ifdef __cplusplus
}
#endif
