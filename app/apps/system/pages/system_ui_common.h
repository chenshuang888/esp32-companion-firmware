#pragma once

#include "lvgl.h"
#include "system_app.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * system app 双 page 共用 UI 组件
 *   - 圆盘 gauge_t（lv_arc + 中心数字 + 单位）
 *   - 信息卡 + KV 行
 *   - 顶部 tabbar（PC/Device）
 *   - 底部 30px hit zone（上滑退出 + 横向滑切 tab）
 *
 * 屏幕区域规划（240×320）：
 *   y=0~24    statusbar（外部 attach）
 *   y=24~52   tabbar（28px）
 *   y=52~290  内容区（238px）—— 子 page 自己排
 *   y=290~320 hit zone（30px）
 * ========================================================================= */

#define SYS_HIT_ZONE_H   30
#define SYS_TABBAR_H     28
#define SYS_CONTENT_TOP  (24 + SYS_TABBAR_H)
#define SYS_GAUGE_SIZE   64
#define SYS_GAUGE_ARC_W  6

/* 三个圆盘横向 x 偏移 */
extern const int SYS_GAUGE_X[3];

typedef struct {
    lv_obj_t *arc;
    lv_obj_t *num_lbl;
    lv_obj_t *unit_lbl;
} sys_gauge_t;

typedef struct {
    lv_obj_t *k;
    lv_obj_t *v;
} sys_kv_t;

/* 圆盘工厂 */
sys_gauge_t sys_make_gauge(lv_obj_t *parent, int x, int y,
                            lv_color_t arc_color, const char *unit_str);
lv_obj_t   *sys_make_gauge_label(lv_obj_t *parent, int x, int y, const char *txt);
void        sys_apply_gauge(sys_gauge_t *g, int value, lv_color_t color_normal,
                             lv_color_t color_warn, int warn_at);

/* 信息卡 */
lv_obj_t *sys_make_info_card(lv_obj_t *parent, int y_offset, int height);
sys_kv_t  sys_make_kv_row(lv_obj_t *card, int y, const char *key);

/* 顶部 tabbar：插到 screen 上（y=24，高 28），返回 bar 对象 */
lv_obj_t *sys_make_tabbar(lv_obj_t *screen);
/* tabbar 视觉跟随当前 page 自动刷新（在 page create 末尾调一次） */
void      sys_tabbar_update_active(lv_obj_t *bar);

/* 底部 30px hit zone：上滑退出回 launcher。
 * 同时绑定整屏左右滑监听以切 tab（在 screen 上加事件）。 */
void sys_attach_hit_and_swipe(lv_obj_t *screen);

#ifdef __cplusplus
}
#endif
