#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Tiny TTF 运行时从嵌入的子集 TTF 创建字体对象；指针在 app_fonts_init 赋值 */
extern lv_font_t *g_app_font_text;
extern lv_font_t *g_app_font_title;
extern lv_font_t *g_app_font_huge;
extern lv_font_t *g_app_font_icons_24;   /* Material Symbols Rounded, 24px */
extern lv_font_t *g_app_font_icons_36;   /* Material Symbols Rounded, 36px (九宫格) */

#define APP_FONT_TEXT     (g_app_font_text)               /* 正文 14px */
#define APP_FONT_TITLE    (g_app_font_title)              /* 标题 16px */
#define APP_FONT_LARGE    (&lv_font_montserrat_24)        /* 大号数字 / ASCII */
#define APP_FONT_HUGE     (g_app_font_huge)               /* 48px 巨号 */
#define APP_FONT_ICONS_24 (g_app_font_icons_24)
#define APP_FONT_ICONS_36 (g_app_font_icons_36)

/* ---- Material Symbols Rounded codepoint（UTF-8 字面量）----
 * 在 lv_label_set_text 里用："ICON_BLUETOOTH" 等。
 * 注意 label 的 text_font 必须是 g_app_font_icons_*，否则字体里无此字符。
 */
#define ICON_BLUETOOTH      "\xEE\x86\xA7"   /* U+E1A7 */
#define ICON_BT_DISABLED    "\xEE\x86\xA8"   /* U+E1A8 */
#define ICON_BATT_FULL      "\xEE\x86\xA4"   /* U+E1A4 */
#define ICON_BATT_5BAR      "\xEE\xAF\x9D"   /* U+EBDD */
#define ICON_BATT_3BAR      "\xEE\xAF\xA0"   /* U+EBE0 */
#define ICON_BATT_1BAR      "\xEE\xAF\x9C"   /* U+EBDC */

#define ICON_SCHEDULE       "\xEE\xA2\xB5"   /* 时钟 */
#define ICON_WEATHER        "\xEF\x85\xB2"   /* 多云 */
#define ICON_NOTIFICATIONS  "\xEE\x9F\xB4"   /* 铃铛 */
#define ICON_MUSIC          "\xEE\x90\x85"   /* 音符 */
#define ICON_TUNE           "\xEE\x90\xA9"   /* 滑块（系统）*/
#define ICON_SETTINGS       "\xEE\xA2\xB8"   /* 齿轮（设置 app）*/
#define ICON_BRIGHTNESS     "\xEE\x86\xA9"   /* 太阳（亮度）*/
#define ICON_INFO           "\xEE\xA2\x8E"   /* ⓘ 关于 */
#define ICON_EDIT_CALENDAR  "\xEE\x95\x96"   /* 时间设置 */
#define ICON_APPS           "\xEE\x97\x83"   /* 通用 app fallback */

#define ICON_CHEVRON_LEFT   "\xEE\x97\x8B"
#define ICON_CHEVRON_RIGHT  "\xEE\x97\x8C"
#define ICON_DOT            "\xEE\xBD\x8A"   /* 大圆点 */
#define ICON_DOT_SMALL      "\xEE\x81\xA1"   /* 小圆点 */

/* ---- 业务 app 图标（动态 app launcher manifest 用，sys.icons 也暴露）---- */
#define ICON_ALARM          "\xEE\xA1\x95"   /* U+E855 闹钟 */
#define ICON_TIMER          "\xEE\x90\xA5"   /* U+E425 计时器（秒表共用）*/
#define ICON_HABIT          "\xEE\xA1\xAC"   /* U+E86C check_circle */
#define ICON_NOTE           "\xEE\xA1\xB3"   /* U+E873 description */
#define ICON_GAME           "\xEE\x8C\xB8"   /* U+E338 videogame_asset */
#define ICON_CALCULATOR     "\xEE\xA9\x9F"   /* U+EA5F calculate */
#define ICON_IMAGE_PIC      "\xEE\x8F\xB4"   /* U+E3F4 image */
#define ICON_MEMORY         "\xEE\x8C\xA2"   /* U+E322 memory 芯片 */
#define ICON_DASHBOARD      "\xEE\xA1\xB1"   /* U+E871 仪表盘 */
#define ICON_PUZZLE         "\xEE\xA1\xBB"   /* U+E87B extension */
#define ICON_TARGET         "\xEE\x86\xB3"   /* U+E1B3 gps_fixed */
#define ICON_PETS           "\xEE\xA4\x9D"   /* U+E91D 宠物（鱼缸借用）*/
#define ICON_CAMPAIGN       "\xEE\xBD\x89"   /* U+EF49 喇叭 */

void app_fonts_init(void);

#ifdef __cplusplus
}
#endif
