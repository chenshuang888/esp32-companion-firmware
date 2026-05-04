#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "media_service.h"   /* media_payload_t / MEDIA_TITLE_MAX / MEDIA_ARTIST_MAX */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化媒体管理器（创建队列）
 *        必须在 BLE 回调可能触发之前调用。
 */
esp_err_t media_manager_init(void);

/**
 * @brief BLE host 线程调用，投递一条新的媒体状态到 UI 线程。
 *
 * 队列满时丢弃最旧的一条（媒体数据越新越好，不保留历史）。
 */
esp_err_t media_manager_push(const media_payload_t *payload);

/**
 * @brief UI 线程每帧调用：消费队列、刷新内部 latest 快照、更新 version 和收到时刻。
 */
void media_manager_process_pending(void);

/** 读取最新快照（仅 UI 线程，无锁）；NULL 表示尚未收到任何数据 */
const media_payload_t *media_manager_get_latest(void);

/** 是否已至少收到一条数据 */
bool media_manager_has_data(void);

/** 基于 esp_timer 插值得到"此刻"的播放位置（秒）；-1 = 未知 */
int16_t media_manager_get_position_now(void);

/** 版本号：process_pending 消费到新数据时 +1 */
uint32_t media_manager_version(void);

#ifdef __cplusplus
}
#endif
