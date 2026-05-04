#include "weather_app.h"
#include "page_weather.h"
#include "app_fonts.h"

static lv_obj_t *on_enter(void) { return page_weather_get_callbacks()->create(); }
static void on_app_exit(void)       { const page_callbacks_t *cb = page_weather_get_callbacks(); if (cb->destroy) cb->destroy(); }
static void on_tick(void)       { const page_callbacks_t *cb = page_weather_get_callbacks(); if (cb->update) cb->update(); }

const app_descriptor_t WEATHER_APP = {
    .id              = "weather",
    .display_name    = "天气",
    .menu_icon       = ICON_WEATHER,
    .menu_icon_color = 0xF59E0B,
    .immersive       = false,          /* 挂 statusbar */
    .show_in_menu    = true,
    .on_enter        = on_enter,
    .on_exit         = on_app_exit,
    .on_tick         = on_tick,
};
