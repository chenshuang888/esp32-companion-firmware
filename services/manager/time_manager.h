#pragma once

#include <sys/time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化时间管理器
 *
 * 创建内部消息队列。必须在任何 time_manager_request_set_time
 * 调用之前完成。
 *
 * @return
 *  - ESP_OK: 成功
 *  - ESP_ERR_NO_MEM: 队列创建失败
 */
esp_err_t time_manager_init(void);

/**
 * @brief 请求将系统时间设置为 tv（线程安全，非阻塞）
 *
 * 请求会被投递到内部队列，由 UI 线程在
 * time_manager_process_pending() 中串行执行实际的
 * settimeofday()。适用于 BLE 等不能阻塞的回调。
 *
 * @param tv 目标时间
 * @return
 *  - ESP_OK: 请求已入队
 *  - ESP_ERR_INVALID_STATE: 未初始化
 *  - ESP_ERR_INVALID_ARG: tv 为 NULL
 *  - ESP_ERR_NO_MEM: 队列满
 */
esp_err_t time_manager_request_set_time(const struct timeval *tv);

/**
 * @brief 处理队列中所有待处理的时间设置请求
 *
 * 仅由 UI 线程周期性调用。所有实际的 settimeofday()
 * 都在此函数内发生，因此系统时间的写入点是单线程的。
 */
void time_manager_process_pending(void);

#ifdef __cplusplus
}
#endif
