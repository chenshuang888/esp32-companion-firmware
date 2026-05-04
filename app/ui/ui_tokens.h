#pragma once

/* ============================================================================
 * UI Design Tokens —— 全项目唯一颜色 / 字号 / 间距 / 圆角源
 *
 * 守则：
 *  - 任何页面/组件不再写裸的 lv_color_hex(...)，统一用这里的 UI_C_*
 *  - 字号只用 UI_F_*；间距只用 UI_SP_*；圆角只用 UI_R_*
 *  - 配色参考 Linear / Raycast 深色系：高对比度 + 克制的强调色
 * ========================================================================= */

#include "lvgl.h"
#include "app_fonts.h"

/* ---- 颜色 —— iOS 原生浅色主题 ----
 * 命名按"角色"，不按"色相"。换皮肤时只改这里。 */

/* 背景层级（从最底到最浮上） */
#define UI_C_BG          lv_color_hex(0xF2F2F7)   /* 屏幕底（iOS systemGroupedBackground） */
#define UI_C_PANEL       lv_color_hex(0xFFFFFF)   /* 卡片（纯白） */
#define UI_C_PANEL_HI    lv_color_hex(0xE5E5EA)   /* 卡片悬浮 / hover */
#define UI_C_BORDER      lv_color_hex(0xC6C6C8)   /* 分隔线 / 描边（iOS separator） */

/* 文本层级 */
#define UI_C_TEXT        lv_color_hex(0x000000)   /* 主文字（纯黑） */
#define UI_C_TEXT_DIM    lv_color_hex(0x3C3C43)   /* 次要（iOS label.secondary） */
#define UI_C_TEXT_MUTED  lv_color_hex(0x6E6E73)   /* 弱（iOS label.tertiary） */

/* 强调色（iOS system colors） */
#define UI_C_ACCENT      lv_color_hex(0x007AFF)   /* iOS 蓝 —— 主操作 / 链接 */
#define UI_C_ACCENT_2    lv_color_hex(0xAF52DE)   /* iOS 紫 —— 次操作 / 装饰 */

/* 状态色（iOS system colors） */
#define UI_C_OK          lv_color_hex(0x34C759)   /* iOS 绿 */
#define UI_C_WARN        lv_color_hex(0xFF9500)   /* iOS 橙 */
#define UI_C_ERR         lv_color_hex(0xFF3B30)   /* iOS 红 */
#define UI_C_INFO        lv_color_hex(0x5AC8FA)   /* iOS 浅蓝 */

/* ---- 字号语义别名（复用现有 app_fonts） ----
 * 现有：g_app_font_text(14) / title(16) / huge(48) + montserrat fallback
 * 这里只起语义名，不引入新字体。需要新字号时再扩 app_fonts。 */

#define UI_F_BODY        APP_FONT_TEXT      /* 正文 14px CJK */
#define UI_F_LABEL       APP_FONT_TEXT      /* 14px 标签 */
#define UI_F_TITLE       APP_FONT_TITLE     /* 卡片标题 16px CJK */
#define UI_F_HUGE        APP_FONT_HUGE      /* 数字巨号 48px */
#define UI_F_NUM_M       APP_FONT_LARGE     /* 中号数字 24px Montserrat */
#define UI_F_ICON        (&lv_font_montserrat_24)  /* 图标（FontAwesome via fallback） */
#define UI_F_ICON_S      (&lv_font_montserrat_14)

/* ---- 间距 token (8 倍数体系) ----
 * 任何 padding / margin / gap 只能用这些。 */

#define UI_SP_XS    4
#define UI_SP_SM    8
#define UI_SP_MD    12
#define UI_SP_LG    16
#define UI_SP_XL    24
#define UI_SP_2XL   32

/* ---- 圆角 ---- */

#define UI_R_SM     6
#define UI_R_MD     10
#define UI_R_LG     14
#define UI_R_PILL   1000   /* 胶囊（lv_obj_set_style_radius 用，会自动 clamp） */

/* ---- 通用动画时长（ms） ---- */

#define UI_DUR_FAST    150
#define UI_DUR_NORM    250
#define UI_DUR_SLOW    400
