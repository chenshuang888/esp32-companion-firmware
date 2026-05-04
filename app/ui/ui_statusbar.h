#pragma once

/* ============================================================================
 * ui_statusbar —— 系统级顶部状态栏（高 24px）
 *
 * 布局：
 *   [ 14:32 ]                    [ 蓝牙图标 ] [ 电池图标 87% ]
 *      左                                          右
 *
 * 数据：
 *   时间    : time(NULL) + localtime_r           (1Hz 自更新)
 *   蓝牙    : ble_driver_is_connected()           (1Hz 自更新)
 *   电池    : battery_sim_get_percent/state       (1Hz 自更新)
 *
 * 用法：
 *   lv_obj_t *bar = ui_statusbar_create(parent, false);  // 浅色页（白底）
 *   lv_obj_t *bar = ui_statusbar_create(parent, true);   // 深色页（深紫底）
 *   // 容器透明，文字色自适配；自动占满 parent 顶部 240×24，不需要 align
 *
 * 销毁：parent 删除时一起销毁；内部 timer 自动停（绑定到 obj 的 delete cb）
 * ========================================================================= */

#include "lvgl.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @param parent   240 宽容器（screen）
 * @param dark     true = 深色背景页（文字偏白），false = 浅色背景页（文字偏黑）
 *
 * 容器本身透明，由所在 screen 提供底色。
 */
lv_obj_t *ui_statusbar_create(lv_obj_t *parent, bool dark);

#ifdef __cplusplus
}
#endif
