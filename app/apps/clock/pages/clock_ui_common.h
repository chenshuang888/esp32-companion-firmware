#pragma once
#include "lvgl.h"
#include "clock_app.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * clock app 4 页共用组件
 *   - 顶部 32px tab 栏（4 等分：闹钟 / 世界 / 秒表 / 计时）
 *   - 底部 30px hit zone（上滑退出 app）
 *   - 整屏左右滑切 tab
 *
 * 屏幕规划（240×320）：
 *   y=0~24    statusbar
 *   y=24~56   tab bar（32px）
 *   y=56~290  内容（234px，子 page 自己排）
 *   y=290~320 hit zone（30px）
 * ========================================================================= */

#define CLK_TABBAR_H     32
#define CLK_HIT_ZONE_H   30
#define CLK_CONTENT_TOP  (24 + CLK_TABBAR_H)
#define CLK_CONTENT_H    (320 - CLK_CONTENT_TOP - CLK_HIT_ZONE_H)

/* tab bar：插到 screen 上，返回 bar 对象。当前 page 自动高亮 */
lv_obj_t *clk_make_tabbar(lv_obj_t *screen);
void      clk_tabbar_update_active(lv_obj_t *bar);

/* 30px 底部 hit zone（上滑退出 app）+ 整屏左右滑监听切 tab；
 * 页面 children 自动加 EVENT_BUBBLE，保证事件能冒到 screen */
void clk_attach_hit_and_swipe(lv_obj_t *screen);

#ifdef __cplusplus
}
#endif
