#pragma once

#include "esp_err.h"
#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * App Router —— 顶层 App 生命周期管理
 *
 * 与 sub_router 的关系：
 *   - app_router 管 "用户感知的应用"（settings / weather / music ...）
 *   - 每个 app 自己内部如有多 page，自己持一个 sub_router_t 实例
 *   - app 之间不能互跳 page，只能 enter / exit
 *
 * 与原 page_router 的区别：
 *   - id 改字符串（"settings"），方便日志阅读 + 解耦枚举膨胀
 *   - 新增 immersive / show_in_menu 等 app 级元数据
 *   - 新增 on_enter_with_arg 给 dynapp_host 传 JS app 名
 *
 * 线程约束：
 *   - 所有回调都在 UI 线程执行；
 *   - app_router_current_id() 是纯读，可任意线程调（fs_worker hook 用）。
 * ========================================================================= */

/**
 * App 描述符 —— 由各 app 模块定义为 const static，注册到 router 里。
 */
typedef struct {
    const char *id;                 /* "settings"，全局唯一 */
    const char *display_name;       /* "设置"，launcher 显示用；可为 NULL */
    const char *menu_icon;          /* ICON_* UTF-8 字面量；NULL = 不在 launcher 显示 */
    uint32_t    menu_icon_color;    /* 0xRRGGBB；纯展示用 */
    bool        immersive;          /* true = app 自管整个 320px，不挂 statusbar */
    bool        show_in_menu;       /* true = launcher 列举此 app */

    /* ---- 生命周期回调 ---- */

    /**
     * 进入 app —— 创建并返回 LVGL screen（lv_obj_create(NULL)）。
     * 必须非 NULL；返回 NULL 视作失败。
     * 二选一：on_enter / on_enter_with_arg（带参的优先用，没传 arg 时回退 on_enter）
     */
    lv_obj_t *(*on_enter)(void);
    lv_obj_t *(*on_enter_with_arg)(const char *arg);

    /**
     * 离开 app —— 释放 app 私有资源（含 lv_obj_del(screen)）。
     * 可选；如果不实现，router 会 lv_obj_del(current_screen) 兜底。
     */
    void (*on_exit)(void);

    /**
     * UI 线程每帧 tick —— 可选。
     * 多 page app 在此转发到 sub_router_tick。
     */
    void (*on_tick)(void);
} app_descriptor_t;

/* ============================================================================
 * Router API
 * ========================================================================= */

esp_err_t app_router_init(void);

/**
 * 注册 app（描述符须长寿命，通常是 const static）。
 */
esp_err_t app_router_register(const app_descriptor_t *app);

/**
 * 进入指定 app。如果当前已在该 app 上则 no-op。
 */
esp_err_t app_router_enter(const char *app_id);

/**
 * 进入指定 app 并带一个字符串参数（dynapp_host 传 JS app 名）。
 * 调用 on_enter_with_arg；若 app 未实现，回退 on_enter（arg 被忽略）。
 */
esp_err_t app_router_enter_with_arg(const char *app_id, const char *arg);

/**
 * 用外部 prepare 好的 screen 接管 app —— 跳过 on_enter，直接 lv_scr_load。
 * 仅 dynapp_host 这种"后台准备 + 瞬切"模式用。
 *
 * 失败（id 非法 / prepared_screen NULL / 已在该 app）时，调用方自己释放 prepared_screen。
 */
esp_err_t app_router_commit_prepared(const char *app_id, lv_obj_t *prepared_screen);

/**
 * 当前 app id；未进入任何 app 返回 NULL。线程安全（纯读）。
 */
const char *app_router_current_id(void);

/**
 * 当前 app 描述符；未进入任何 app 返回 NULL。
 */
const app_descriptor_t *app_router_current(void);

/**
 * UI 线程每帧调；转发到当前 app on_tick。
 */
void app_router_tick(void);

/* ---- 枚举（launcher 用）----
 * 收集所有 show_in_menu=true 的 app 描述符指针到 out 数组。
 * 返回实际填入个数（不超过 cap）。 */
uint8_t app_router_get_visible_apps(const app_descriptor_t **out, uint8_t cap);

/* ---- 便捷退出 ---- */
esp_err_t app_router_exit_to_launcher(void);
esp_err_t app_router_exit_to_lockscreen(void);

#ifdef __cplusplus
}
#endif
