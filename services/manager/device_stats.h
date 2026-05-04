#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * device_stats —— ESP32 端运行状态采集
 *
 * UI 线程定时调 device_stats_tick() 触发采集（约 1Hz），结果存内部快照。
 * 之所以放在 UI 线程：
 *   - 避免独占一条 task / 多一组同步原语
 *   - 数据消费方就是 UI，写者读者同线程，无锁
 *
 * 唯一特殊：温度 sensor 必须先 device_stats_init() 装一次（main 启动时调）
 * ========================================================================= */

typedef struct {
    /* 内存 */
    uint32_t psram_free;        // 字节
    uint32_t psram_total;       // 字节
    uint32_t sram_free;         // 字节（INTERNAL 堆）
    uint32_t sram_total;        // 字节
    uint32_t min_free_ever;     // 历史最小空闲（泄漏检测）

    /* 温度 */
    int16_t  chip_temp_cx10;    // 芯片温度 ×10；-32768 = 不可用

    /* 运行 */
    uint32_t uptime_sec;        // esp_timer 推算
    uint16_t task_count;        // FreeRTOS 任务数
    bool     ble_connected;

    /* 存储 */
    uint32_t fs_used;           // littlefs used bytes
    uint32_t fs_total;          // littlefs total bytes

    /* 系统 */
    uint8_t  reset_reason;      // esp_reset_reason() raw value
    char     fw_version[24];    // esp_app_get_description()->version
} device_stats_t;

/* 装温度 sensor，只能调一次。失败时 chip_temp_cx10 永远是 -32768 */
esp_err_t device_stats_init(void);

/* UI 线程定期调（约 1Hz；过频次没意义，温度 sensor 也是几百 ms 级）*/
void device_stats_tick(void);

/* 读快照。返回内部地址，仅供 UI 线程读，不可缓存指针 */
const device_stats_t *device_stats_get(void);

/* 数据版本（每次 tick 后 +1）；UI 用它去重，避免每帧重渲染 label */
uint32_t device_stats_epoch(void);

#ifdef __cplusplus
}
#endif
