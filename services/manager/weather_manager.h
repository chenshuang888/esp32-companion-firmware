#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WEATHER_CITY_MAX 24
#define WEATHER_DESC_MAX 32

/* 天气编码（PC 端同步维护） */
typedef enum {
    WEATHER_CODE_UNKNOWN  = 0,
    WEATHER_CODE_CLEAR    = 1,
    WEATHER_CODE_CLOUDY   = 2,
    WEATHER_CODE_OVERCAST = 3,
    WEATHER_CODE_RAIN     = 4,
    WEATHER_CODE_SNOW     = 5,
    WEATHER_CODE_FOG      = 6,
    WEATHER_CODE_THUNDER  = 7,
} weather_code_t;

/* 与 PC 端 struct.pack("<hhhBBI24s32s", ...) 严格对齐 */
typedef struct {
    int16_t  temp_c_x10;                    // 当前温度 ×10
    int16_t  temp_min_x10;                  // 今日最低 ×10
    int16_t  temp_max_x10;                  // 今日最高 ×10
    uint8_t  humidity;                      // 湿度 %
    uint8_t  weather_code;                  // weather_code_t
    uint32_t updated_at;                    // unix timestamp
    char     city[WEATHER_CITY_MAX];        // UTF-8
    char     description[WEATHER_DESC_MAX]; // UTF-8
} __attribute__((packed)) weather_payload_t;

/**
 * @brief 初始化天气管理器
 * @return ESP_OK / ESP_ERR_NO_MEM
 */
esp_err_t weather_manager_init(void);

/**
 * @brief 请求更新天气数据（线程安全，非阻塞）
 *
 * 投递到队列，由 UI 线程在 process_pending 中消费。
 * 队列满时自动丢弃最旧的一条（天气数据越新越好）。
 */
esp_err_t weather_manager_push(const weather_payload_t *data);

/**
 * @brief 处理队列中的数据，更新内部快照（仅 UI 线程调用）
 */
void weather_manager_process_pending(void);

/**
 * @brief 读取最新快照（仅 UI 线程调用，无锁）
 * @return NULL 表示尚未收到任何数据
 */
const weather_payload_t *weather_manager_get_latest(void);

/**
 * @brief 是否已有至少一次天气数据
 */
bool weather_manager_has_data(void);

#ifdef __cplusplus
}
#endif
