#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 电池/温度哨兵值 */
#define SYSTEM_BATTERY_ABSENT   255
#define SYSTEM_CHARGING_ABSENT  255
#define SYSTEM_CPU_TEMP_INVALID ((int16_t)-32768)

/* 与 PC 端 struct.pack("<BBBBBBhIHH", ...) 严格对齐（16 字节）
 * 顺序：CPU% / MEM% / DISK% / BAT% / charging / reserved
 *       / cpu_temp×10 / uptime_sec / net_down_kbps / net_up_kbps */
typedef struct {
    uint8_t  cpu_percent;       // 0-100
    uint8_t  mem_percent;       // 0-100
    uint8_t  disk_percent;      // 0-100
    uint8_t  battery_percent;   // 0-100；255 = 无电池
    uint8_t  battery_charging;  // 0=未充 / 1=充电中 / 255=无电池
    uint8_t  _reserved;         // 对齐
    int16_t  cpu_temp_cx10;     // CPU 温度 ×10；-32768 = 不可用
    uint32_t uptime_sec;        // PC 开机时长（秒）
    uint16_t net_down_kbps;     // 下行 KB/s
    uint16_t net_up_kbps;       // 上行 KB/s
} __attribute__((packed)) system_payload_t;

/**
 * @brief 初始化系统监控管理器（queue 深 2，越新越好）
 */
esp_err_t system_manager_init(void);

/**
 * @brief GATT 回调用：把收到的 payload 投递到队列（非阻塞）
 */
esp_err_t system_manager_push(const system_payload_t *data);

/**
 * @brief UI 线程用：消费队列，更新快照
 */
void system_manager_process_pending(void);

/**
 * @brief UI 线程用：读最新快照，NULL = 尚未有数据
 */
const system_payload_t *system_manager_get_latest(void);

/**
 * @brief 是否至少有一次数据到达
 */
bool system_manager_has_data(void);

/**
 * @brief 页面侧去重用：每次 push 成功后单调递增
 */
uint32_t system_manager_get_epoch(void);

#ifdef __cplusplus
}
#endif
