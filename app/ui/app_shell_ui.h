#pragma once

#include "lvgl.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * app_shell_ui —— app 层的 UI helper（依赖 app/ui 组件）
 *
 * 用法（非沉浸式 app on_enter）：
 *   lv_obj_t *screen = lv_obj_create(NULL);
 *   ui_screen_setup(screen);                            // 浅色 screen
 *   app_shell_attach_statusbar(screen, false);          // dark=false
 *   ... 其它内容 align 到 24px 以下
 *
 *   或深色页：
 *   lv_obj_t *screen = lv_obj_create(NULL);
 *   lv_obj_set_style_bg_color(screen, ...);             // 深色底
 *   app_shell_attach_statusbar(screen, true);           // dark=true 文字偏白
 * ========================================================================= */

/**
 * 在 screen 顶部挂统一 24px 状态栏。返回 statusbar obj。
 * @param screen 240 宽 screen / 容器
 * @param dark   true=深色页（文字偏白），false=浅色页（文字偏黑）
 */
lv_obj_t *app_shell_attach_statusbar(lv_obj_t *screen, bool dark);

#ifdef __cplusplus
}
#endif
