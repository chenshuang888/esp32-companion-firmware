#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 从 NVS 加载背光值到内存缓存
 *
 * 必须在 persist_init() 之后调用。若 NVS 中无记录使用默认值。
 */
esp_err_t backlight_storage_init(void);

/** 读取当前背光缓存值（0-255） */
uint8_t backlight_storage_get(void);

/**
 * @brief 更新背光缓存并立即落盘 NVS；无变化时跳过写入
 *
 * 仅负责持久化 + 缓存；硬件生效由调用方（lcd_panel_set_backlight）自行处理，
 * 避免 storage -> drivers 的反向依赖。
 */
esp_err_t backlight_storage_set(uint8_t duty);

#ifdef __cplusplus
}
#endif
