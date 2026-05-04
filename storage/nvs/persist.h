#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "nvs.h"   /* 暴露 ESP_ERR_NVS_* 常量，调用方需要区分 NOT_FOUND 等语义 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 NVS 子系统
 *
 * 必须在 app_main 最开始统一初始化一次（settings_store / notify_manager /
 * 未来的 wifi / bonding 都依赖 NVS）。
 *
 * 失败时按官方推荐流程擦除后重试。
 */
esp_err_t persist_init(void);

/* ------------------------------------------------------------------
 * KV：标量类型
 * 返回 ESP_OK、ESP_ERR_NVS_NOT_FOUND、或底层 NVS 错误码
 * ------------------------------------------------------------------ */

esp_err_t persist_get_u8 (const char *ns, const char *key, uint8_t  *out);
esp_err_t persist_set_u8 (const char *ns, const char *key, uint8_t   val);

esp_err_t persist_get_i64(const char *ns, const char *key, int64_t *out);
esp_err_t persist_set_i64(const char *ns, const char *key, int64_t   val);

/* ------------------------------------------------------------------
 * KV：blob
 *
 * get_blob: *len 传入缓冲区容量，返回时写入实际读出的字节数
 *           返回 ESP_ERR_NVS_INVALID_LENGTH 表示缓冲区太小
 * set_blob: 原子写入整块 blob
 * ------------------------------------------------------------------ */

esp_err_t persist_get_blob(const char *ns, const char *key,
                           void *buf, size_t *len);

esp_err_t persist_set_blob(const char *ns, const char *key,
                           const void *buf, size_t len);

/* ------------------------------------------------------------------
 * 调试 / 复位：擦除整个 namespace
 * ------------------------------------------------------------------ */

esp_err_t persist_erase_namespace(const char *ns);

#ifdef __cplusplus
}
#endif
