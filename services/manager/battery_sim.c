#include "battery_sim.h"
#include "esp_timer.h"

/* 每 5 分钟 -1%，开机 100%，到 0 后重置回 100% 循环 */
#define BATTERY_DROP_INTERVAL_US   (5LL * 60 * 1000 * 1000)
#define BATTERY_FULL_PCT           100

uint8_t battery_sim_get_percent(void)
{
    int64_t now_us = esp_timer_get_time();
    int64_t drops  = now_us / BATTERY_DROP_INTERVAL_US;
    int     pct    = BATTERY_FULL_PCT - (int)(drops % (BATTERY_FULL_PCT + 1));
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return (uint8_t)pct;
}

battery_state_t battery_sim_get_state(void)
{
    uint8_t p = battery_sim_get_percent();
    if (p < 10) return BATTERY_CRITICAL;
    if (p < 30) return BATTERY_LOW;
    return BATTERY_OK;
}
