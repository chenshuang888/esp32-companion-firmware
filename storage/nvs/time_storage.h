#pragma once

#include <sys/time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化时间存储
 *
 * 记录 tick_save 起点时刻，避免启动瞬间就写 NVS。
 * 必须在 persist_init() 之后调用。
 */
esp_err_t time_storage_init(void);

/**
 * @brief 读取 NVS 中保存的上次系统时间
 *
 * 上层把结果传给 settimeofday()。若 NVS 没有记录返回 ESP_ERR_NVS_NOT_FOUND，
 * 上层回退到硬编码默认时间。
 */
esp_err_t time_storage_load_last(struct timeval *out);

/**
 * @brief 周期保存系统时间（UI 线程调用）
 *
 * 内部按 5 分钟间隔写一次，未到间隔直接返回。
 */
void time_storage_tick_save(void);

#ifdef __cplusplus
}
#endif
