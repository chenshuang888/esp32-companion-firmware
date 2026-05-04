#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------
 * BLE 媒体服务（v2 协议：首字节 type 分发）
 *
 * 两个 char 复用 v3 规则（触发端与响应端同 service）：
 *   - WRITE  (8a5c0008): PC → ESP，按首字节 type 分发：
 *       0x01 NOWPLAYING       —— 当前播放快照（旧 media_payload_t，92B 子载荷）
 *       0x02 PLAYLIST_BEGIN   —— 歌单批次开始（total_count + version）
 *       0x03 PLAYLIST_ITEM    —— 歌单条目（index + title + artist）
 *       0x04 PLAYLIST_END     —— 歌单批次结束
 *
 *   - NOTIFY (8a5c000d): ESP → PC，按首字节 type 分发：
 *       0x01 BUTTON           —— 屏上媒体键事件（旧 media_button_event_t，4B）
 *       0x02 PLAY_TRACK       —— 屏上点歌（track_index + seq）
 *
 * 协议字节序：little-endian，packed。
 * ------------------------------------------------------------------ */

/* WRITE: type tag */
#define MEDIA_MSG_NOWPLAYING       0x01
#define MEDIA_MSG_PLAYLIST_BEGIN   0x02
#define MEDIA_MSG_PLAYLIST_ITEM    0x03
#define MEDIA_MSG_PLAYLIST_END     0x04

/* NOTIFY: type tag */
#define MEDIA_NOTIFY_BUTTON        0x01
#define MEDIA_NOTIFY_PLAY_TRACK    0x02

/* ============================================================================
 * NOWPLAYING（旧 media_payload_t）
 * ========================================================================= */

/* 与 PC 端 struct.pack("<BBhhHI48s32s", ...) 严格对齐（92 bytes 子载荷）*/
typedef struct {
    uint8_t  playing;                       // 0=paused, 1=playing
    uint8_t  _reserved;
    int16_t  position_sec;                  // 当前进度秒；-1 表示未知
    int16_t  duration_sec;                  // 总时长秒；-1 表示未知 / 直播
    uint16_t _pad;                          // 4 字节边界对齐
    uint32_t sample_ts;                     // PC 采样时刻 unix sec
    char     title[48];                     // UTF-8，末尾 \0
    char     artist[32];                    // UTF-8，末尾 \0
} __attribute__((packed)) media_payload_t;

#define MEDIA_TITLE_MAX  48
#define MEDIA_ARTIST_MAX 32

/* ============================================================================
 * BUTTON（旧 media_button_event_t）
 * ========================================================================= */

typedef struct {
    uint8_t  id;       // 0=prev, 1=play_pause, 2=next
    uint8_t  action;   // 0=press
    uint16_t seq;      // 单调递增；PC 端用于去重
} __attribute__((packed)) media_button_event_t;

#define MEDIA_BTN_PREV         0
#define MEDIA_BTN_PLAY_PAUSE   1
#define MEDIA_BTN_NEXT         2
#define MEDIA_BTN_ACTION_PRESS 0

/* ============================================================================
 * PLAYLIST 协议（PC → ESP）
 * ========================================================================= */

#define MEDIA_PLAYLIST_TITLE_MAX   40
#define MEDIA_PLAYLIST_ARTIST_MAX  24
#define MEDIA_PLAYLIST_MAX_ITEMS   50      // 歌单上限（手表端）

typedef struct {
    uint16_t total_count;       // 本批共 N 首
    uint16_t version;           // PC 自增版本号；ESP 用它分辨"新一批"
} __attribute__((packed)) media_playlist_begin_t;

typedef struct {
    uint16_t index;             // 0..total_count-1
    char     title[MEDIA_PLAYLIST_TITLE_MAX];
    char     artist[MEDIA_PLAYLIST_ARTIST_MAX];
} __attribute__((packed)) media_playlist_item_t;

/* ============================================================================
 * PLAY_TRACK（ESP → PC）
 * ========================================================================= */

typedef struct {
    uint16_t track_index;
    uint16_t seq;
} __attribute__((packed)) media_play_track_event_t;

/* ============================================================================
 * 服务接口
 * ========================================================================= */

/** 注册 GATT 表，必须在 nimble_port_freertos_init() 之前调用 */
esp_err_t media_service_init(void);

/** UI 线程：发送媒体键（prev/play_pause/next） */
esp_err_t media_service_send_button(uint8_t id);

/** UI 线程：发送"在歌单第 N 首点歌"事件 */
esp_err_t media_service_send_play_track(uint16_t track_index);

#ifdef __cplusplus
}
#endif
