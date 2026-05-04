/* ============================================================================
 * fs_littlefs.c —— LittleFS 通用挂载层
 *
 * 上层用法：fopen(FS_LITTLEFS_ROOT "/...") 即可，本文件不暴露文件 API。
 *
 * 设计要点：
 *   - 第一次启动 / 分区被擦过时 mount 必然失败，format_if_mount_failed=true
 *     让 esp_littlefs 自动 format 并重挂，避免上层判 NOT_FOUND 写一堆兜底逻辑
 *   - 同一进程内重复 init 直接返回 OK（idempotent）
 *   - 不开启 dont_mount=true 这种细粒度选项；项目只有一个 LittleFS 分区
 * ========================================================================= */

#include "fs_littlefs.h"

#include "esp_log.h"
#include "esp_littlefs.h"

static const char *TAG = "fs_littlefs";

static bool s_mounted = false;

esp_err_t fs_littlefs_init(void)
{
    if (s_mounted) return ESP_OK;

    esp_vfs_littlefs_conf_t conf = {
        .base_path = FS_LITTLEFS_ROOT,
        .partition_label = FS_LITTLEFS_PARTITION_LABEL,
        .format_if_mount_failed = true,
        .dont_mount = false,
    };

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t total = 0, used = 0;
    if (esp_littlefs_info(FS_LITTLEFS_PARTITION_LABEL, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "mounted at %s (used %u / %u bytes)",
                 FS_LITTLEFS_ROOT, (unsigned)used, (unsigned)total);
    } else {
        ESP_LOGI(TAG, "mounted at %s (info unavailable)", FS_LITTLEFS_ROOT);
    }

    s_mounted = true;
    return ESP_OK;
}

esp_err_t fs_littlefs_info(size_t *total_bytes, size_t *used_bytes)
{
    return esp_littlefs_info(FS_LITTLEFS_PARTITION_LABEL, total_bytes, used_bytes);
}

esp_err_t fs_littlefs_format(void)
{
    return esp_littlefs_format(FS_LITTLEFS_PARTITION_LABEL);
}
