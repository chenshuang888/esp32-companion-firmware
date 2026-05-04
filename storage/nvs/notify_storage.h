#pragma once

#include <stddef.h>
#include "esp_err.h"
#include "nvs.h"   /* 暴露 ESP_ERR_NVS_* 常量，调用方需要区分 NOT_FOUND 等语义 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 通知环形缓冲持久化存储门面。
 * 仅 notify_manager 使用，向上层隐藏 NVS 命名空间/键等存储细节。
 *
 * 业务层（notify_manager）负责定义 blob 内部结构和版本兼容，
 * 本层只做"收到一坨字节 → 存 NVS / 读出一坨字节"的中转。
 */

/**
 * @brief 加载通知 blob
 * @param buf 输出缓冲区
 * @param len in: 缓冲区容量；out: 实际读出字节数
 * @return ESP_OK / ESP_ERR_NVS_NOT_FOUND / ESP_ERR_NVS_INVALID_LENGTH / ...
 */
esp_err_t notify_storage_load(void *buf, size_t *len);

/** @brief 保存通知 blob（原子写入） */
esp_err_t notify_storage_save(const void *buf, size_t len);

#ifdef __cplusplus
}
#endif
