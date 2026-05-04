#pragma once

#include "sub_router.h"
#include "app_router.h"

/* ============================================================================
 * Dynamic App 宿主 —— 动态 JS app 的承载容器
 *
 * 用户视角：每个 JS app（calc / dash / alarm ...）是独立 app；
 * 实现层面：它们都共享这个宿主 app（一时一 app，复用 LVGL screen）。
 *
 * launcher 启动 JS app 的标准路径：
 *   dynapp_host_prepare_and_enter("calc");
 * 它会：
 *   1) 后台 off-screen 起脚本 build 完整 UI
 *   2) ready 后调 app_router_commit_prepared("dynapp_host", screen) 瞬切
 *   3) 800ms 超时兜底强切
 *
 * 进入其它 app 前 launcher 必须调 dynapp_host_cancel_prepare_if_any() 兜底。
 * ========================================================================= */

const page_callbacks_t *page_dynapp_host_get_callbacks(void);

/* 后台准备 + 瞬切（推荐入口） */
void dynapp_host_prepare_and_enter(const char *app_name);

/* 取消进行中的 prepare（其它 app 入口前调）*/
void dynapp_host_cancel_prepare_if_any(void);

/* 设置默认 pending（无 prepare 路径用） */
void dynapp_host_set_pending(const char *app_name);
