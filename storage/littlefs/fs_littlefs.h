#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * fs_littlefs —— 通用文件系统层（不知道"动态 app"是什么）
 *
 * 职责：
 *   - 在 storage 分区上挂载 LittleFS
 *   - 暴露根挂载点常量给上层用 fopen/fread/fwrite
 *   - 暴露 df / format / unmount 这类基础设施操作
 *
 * 不做：
 *   - 任何业务路径策略（"app 放哪儿"由 storage/littlefs/dynapp_script_store 决定）
 *   - 任何文件名约定
 *
 * 调用方式：
 *   ESP_ERROR_CHECK(fs_littlefs_init());
 *   FILE *f = fopen(FS_LITTLEFS_ROOT "/foo/bar.js", "rb");
 */

/* 挂载点。所有上层路径都应当 FS_LITTLEFS_ROOT "/..." 拼接。 */
#define FS_LITTLEFS_ROOT  "/littlefs"

/* 对应 partitions.csv 里 storage 分区的 Name 字段。 */
#define FS_LITTLEFS_PARTITION_LABEL  "storage"

/**
 * 挂载 LittleFS 到 FS_LITTLEFS_ROOT。
 *
 * 行为：
 *   - 分区无效 / 首次挂载失败时自动 format（format_if_mount_failed=true）
 *   - 重复调用是 no-op
 *
 * 失败时返回底层 esp_vfs_littlefs_register 的错误码。
 */
esp_err_t fs_littlefs_init(void);

/**
 * 查询 LittleFS 占用 / 总量（字节）。
 * 任一指针可为 NULL。
 */
esp_err_t fs_littlefs_info(size_t *total_bytes, size_t *used_bytes);

/**
 * 强制 format（清空所有文件）。仅在调试 / 出厂复位时调。
 */
esp_err_t fs_littlefs_format(void);

#ifdef __cplusplus
}
#endif
