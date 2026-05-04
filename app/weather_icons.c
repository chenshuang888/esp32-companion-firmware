#include "weather_icons.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "weather_icons";

/* EMBED_FILES 生成的符号
 * 文件名 ic_clear.bin → _binary_ic_clear_bin_start / _end */
#define DECL_ICON(name)                                                                       \
    extern const uint8_t _binary_##name##_bin_start[] asm("_binary_" #name "_bin_start");     \
    extern const uint8_t _binary_##name##_bin_end[]   asm("_binary_" #name "_bin_end")

DECL_ICON(ic_clear);
DECL_ICON(ic_cloudy);
DECL_ICON(ic_overcast);
DECL_ICON(ic_rain);
DECL_ICON(ic_snow);
DECL_ICON(ic_fog);
DECL_ICON(ic_thunder);
DECL_ICON(ic_unknown);

/* 8 张图标的 dsc 槽位（init 时一次性填充） */
static lv_image_dsc_t s_dsc_clear;
static lv_image_dsc_t s_dsc_cloudy;
static lv_image_dsc_t s_dsc_overcast;
static lv_image_dsc_t s_dsc_rain;
static lv_image_dsc_t s_dsc_snow;
static lv_image_dsc_t s_dsc_fog;
static lv_image_dsc_t s_dsc_thunder;
static lv_image_dsc_t s_dsc_unknown;

static bool s_inited = false;

/* 把一个 .bin 文件 (header + pixel data) 解析成 lv_image_dsc_t */
static void parse_bin(lv_image_dsc_t *dst, const uint8_t *start, const uint8_t *end)
{
    /* 文件前 sizeof(lv_image_header_t) 字节就是 header；其余是 pixel data */
    memcpy(&dst->header, start, sizeof(lv_image_header_t));
    dst->data      = start + sizeof(lv_image_header_t);
    dst->data_size = (uint32_t)(end - start) - sizeof(lv_image_header_t);
    dst->reserved   = NULL;
    dst->reserved_2 = NULL;
}

#define INIT_ICON(field, name)                                                          \
    parse_bin(&s_dsc_##field, _binary_##name##_bin_start, _binary_##name##_bin_end)

void weather_icons_init(void)
{
    if (s_inited) return;
    INIT_ICON(clear,    ic_clear);
    INIT_ICON(cloudy,   ic_cloudy);
    INIT_ICON(overcast, ic_overcast);
    INIT_ICON(rain,     ic_rain);
    INIT_ICON(snow,     ic_snow);
    INIT_ICON(fog,      ic_fog);
    INIT_ICON(thunder,  ic_thunder);
    INIT_ICON(unknown,  ic_unknown);
    s_inited = true;
    ESP_LOGI(TAG, "loaded 8 weather icons (40x40 each)");
}

const lv_image_dsc_t *weather_icon_for(uint8_t code)
{
    if (!s_inited) weather_icons_init();
    switch (code) {
    case WEATHER_CODE_CLEAR:    return &s_dsc_clear;
    case WEATHER_CODE_CLOUDY:   return &s_dsc_cloudy;
    case WEATHER_CODE_OVERCAST: return &s_dsc_overcast;
    case WEATHER_CODE_RAIN:     return &s_dsc_rain;
    case WEATHER_CODE_SNOW:     return &s_dsc_snow;
    case WEATHER_CODE_FOG:      return &s_dsc_fog;
    case WEATHER_CODE_THUNDER:  return &s_dsc_thunder;
    default:                    return &s_dsc_unknown;
    }
}
