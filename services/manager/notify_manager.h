#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NOTIFY_TITLE_MAX  32
#define NOTIFY_BODY_MAX   96
#define NOTIFY_STORE_MAX  10

/* 通知类别（PC 端同步维护） */
typedef enum {
    NOTIFY_CAT_GENERIC  = 0,
    NOTIFY_CAT_MESSAGE  = 1,
    NOTIFY_CAT_EMAIL    = 2,
    NOTIFY_CAT_CALL     = 3,
    NOTIFY_CAT_CALENDAR = 4,
    NOTIFY_CAT_SOCIAL   = 5,
    NOTIFY_CAT_NEWS     = 6,
    NOTIFY_CAT_ALERT    = 7,
} notify_category_t;

/* 与 PC 端 struct.pack("<IBB2x32s96s", ...) 严格对齐（136 字节） */
typedef struct {
    uint32_t timestamp;                 // unix ts
    uint8_t  category;                  // notify_category_t
    uint8_t  priority;                  // 0 低 / 1 普通 / 2 高
    uint8_t  _reserved[2];
    char     title[NOTIFY_TITLE_MAX];   // UTF-8, 末尾必须 \0
    char     body[NOTIFY_BODY_MAX];     // UTF-8, 末尾必须 \0
} __attribute__((packed)) notification_payload_t;

/**
 * @brief 初始化通知管理器
 * @return ESP_OK / ESP_ERR_NO_MEM
 */
esp_err_t notify_manager_init(void);

/**
 * @brief 请求写入一条通知（线程安全，非阻塞）
 *
 * 投递到队列，由 UI 线程在 process_pending 中消费进环形缓冲。
 * 队列满时丢弃最旧的一条（与 weather_manager 行为一致）。
 */
esp_err_t notify_manager_push(const notification_payload_t *n);

/**
 * @brief 处理队列中的所有待处理通知，写入环形缓冲（仅 UI 线程）
 */
void notify_manager_process_pending(void);

/**
 * @brief 周期性落盘（仅 UI 线程调用）
 *
 * 内部按 dirty + 防抖策略判断是否真正写 NVS；未满足条件时立即返回。
 * 建议在 UI 主循环中每帧调用。
 */
void notify_manager_tick_flush(void);

/**
 * @brief 当前存储的通知条数（0 ~ NOTIFY_STORE_MAX）
 */
size_t notify_manager_count(void);

/**
 * @brief 按时间倒序读取第 index 条通知（index=0 表示最新）
 * @return NULL 表示越界
 */
const notification_payload_t *notify_manager_get_at(size_t index);

/**
 * @brief 版本号：每次 process_pending 消费到新数据时 +1
 *
 * UI 页面可用于去重刷新：上次版本号与当前相同时跳过重建。
 */
uint32_t notify_manager_version(void);

/**
 * @brief 清空所有已存通知（UI 线程调用）
 */
void notify_manager_clear(void);

/**
 * @brief 删除第 index 条通知（与 get_at 同序：0=最新）
 *
 * 仅 UI 线程调用。删除后立即更新 version 与 dirty，2 秒后自动落盘。
 * 越界返回 ESP_ERR_INVALID_ARG。
 */
esp_err_t notify_manager_remove_at(size_t index);

#ifdef __cplusplus
}
#endif
