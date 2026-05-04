#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "media_service.h"   /* media_playlist_item_t / MEDIA_PLAYLIST_MAX_ITEMS */

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * playlist_manager —— PC 推过来的歌单（流式 BEGIN→ITEM*N→END）
 *
 * 线程模型：
 *   - BLE host 线程通过 push_begin / push_item / push_end 入队（小队列，丢旧策略）
 *   - UI 线程每帧 process_pending 消费队列，写 pending 缓冲
 *   - 收到 END → 把 pending 原子拷到 live，version++
 *   - UI 只读 get_count / get_track_at / version
 *
 * 单写者：只有 UI 线程写 live[] 与 pending[]，符合 single-writer 约定。
 * ========================================================================= */

esp_err_t playlist_manager_init(void);

/* --- BLE 线程入队 API（投递到内部队列，非阻塞） --- */
esp_err_t playlist_manager_push_begin(uint16_t total_count, uint16_t version);
esp_err_t playlist_manager_push_item(const media_playlist_item_t *item);
esp_err_t playlist_manager_push_end(void);

/* --- UI 线程：每帧消费队列、必要时切换 live --- */
void playlist_manager_process_pending(void);

/* --- UI 线程读：直接读 live 缓冲 --- */
size_t playlist_manager_get_count(void);
const media_playlist_item_t *playlist_manager_get_track_at(size_t index);

/* live 版本号；UI 用它决定是否要重建列表 */
uint32_t playlist_manager_version(void);

/* 是否曾经收到过完整一批（END 已落地） */
bool playlist_manager_has_data(void);

#ifdef __cplusplus
}
#endif
