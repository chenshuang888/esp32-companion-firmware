#pragma once

/* ============================================================================
 * battery_sim —— 电池模拟（无真实电池硬件时的占位）
 *
 * 行为：开机 100%，每 5 分钟 -1%，到 0 后重新回到 100%
 * 无任务/定时器开销 —— 调用方查询时按当前 esp_timer 计算
 *
 * 接真实电池后：替换内部实现，对外接口不变
 * ========================================================================= */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BATTERY_OK,        /* > 30% */
    BATTERY_LOW,       /* 10% - 30% */
    BATTERY_CRITICAL,  /* < 10% */
} battery_state_t;

uint8_t         battery_sim_get_percent(void);
battery_state_t battery_sim_get_state(void);

#ifdef __cplusplus
}
#endif
