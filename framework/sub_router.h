#pragma once

#include "esp_err.h"
#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Sub Router —— App 内部的 page 路由（可多实例）
 *
 * 用法（settings_app 为例）：
 *   - app on_enter 创建 sub_router，注册 home/time/about 三个 page
 *   - sub_router_push 进子页时保留来源到栈里
 *   - sub_router_pop 回上一页；栈空时返回 ESP_ERR_INVALID_STATE，由 app 决定退出
 *
 * 关键差异（vs 老的 page_router）：
 *   - 多实例：每个 app 自己 create 一个，互不干扰
 *   - 字符串 id 改 int id（每个 app 自己维护 enum，更轻量）
 *   - 接管 lv_scr_load —— 当前 page screen 直接 load，所有 page 与外部共享 active screen
 *
 * page_callbacks_t 契约（与原 page_router 完全一致）：
 *   create / destroy / update —— 文件 framework/app_router.h 旁边的复刻；
 *   这里直接用本文件内的同结构，避免依赖即将删除的 page_router.h。
 *
 * 线程：所有 API 都在 UI 线程调用。
 * ========================================================================= */

/**
 * Page 回调（继承自老 page_router 的契约）：
 *   - create() 必须返回独立 screen（lv_obj_create(NULL)），不允许返回 lv_scr_act()
 *   - destroy() 自己负责 lv_obj_del(screen)；未提供时 sub_router 自动 lv_obj_del 兜底
 *   - update() 可选；UI 线程周期性调
 */
typedef struct {
    lv_obj_t *(*create)(void);
    void      (*destroy)(void);
    void      (*update)(void);
} page_callbacks_t;

typedef struct sub_router sub_router_t;

/**
 * 创建 sub_router。
 * @param page_capacity   注册 page 数上限
 * @param history_depth   pop 历史栈深度（典型 4 够用）
 */
sub_router_t *sub_router_create(uint8_t page_capacity, uint8_t history_depth);
void          sub_router_destroy(sub_router_t *r);

/** 注册 page。page_id 由调用方维护（每个 app 自己 enum）。重复注册返回 INVALID_STATE */
esp_err_t sub_router_register(sub_router_t *r, int page_id, const page_callbacks_t *cb);

/**
 * 切到目标 page，并把当前 page 推入历史栈（用于后续 pop）。
 * 第一次调用（current = -1）不入栈。
 */
esp_err_t sub_router_push(sub_router_t *r, int page_id);

/**
 * 弹回上一页。
 * 历史栈空时返回 ESP_ERR_INVALID_STATE（调用方据此决定 app 退出等动作）。
 */
esp_err_t sub_router_pop(sub_router_t *r);

/** 直接切到目标 page，不进栈（重置导航；少用） */
esp_err_t sub_router_replace(sub_router_t *r, int page_id);

/** 当前 page id；未进入任何 page 返回 -1 */
int sub_router_current(sub_router_t *r);

/** UI 线程每帧调；转发到当前 page update */
void sub_router_tick(sub_router_t *r);

#ifdef __cplusplus
}
#endif
