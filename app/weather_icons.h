#pragma once

/* ============================================================================
 * weather_icons.h —— 8 张天气图标资源
 *
 * 资源来源：dynamic_app/scripts/dash/assets/ic_*.bin（40×40 LVGL 9 binary）
 * 嵌入方式：app/CMakeLists.txt 用 EMBED_FILES 把 ic_*.bin 编进固件
 * 加载方式：app_fonts_init 风格——首次访问时把 binary 头解析成 lv_image_dsc_t
 *
 * 用法：
 *   #include "weather_icons.h"
 *   const lv_image_dsc_t *dsc = weather_icon_for(WEATHER_CODE_CLEAR);
 *   lv_image_set_src(img, dsc);
 * ========================================================================= */

#include "lvgl.h"
#include "weather_manager.h"   /* weather_code_t */

#ifdef __cplusplus
extern "C" {
#endif

/* 必须在使用前调用一次（建议放在 app_main 早期，跟 app_fonts_init 一起） */
void weather_icons_init(void);

/* 按 weather_code 返回对应图标 dsc；未知 code 返回 ic_unknown */
const lv_image_dsc_t *weather_icon_for(uint8_t code);

#ifdef __cplusplus
}
#endif
