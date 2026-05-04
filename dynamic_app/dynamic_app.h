#pragma once

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化 Dynamic App（MicroQuickJS）运行时框架（脚本任务 + UI 指令队列）。
 *
 * 你可以把它理解为一个"极简脚本插件系统"的雏形：
 * - Script Task：运行 JS VM、执行 setInterval 等定时回调、解析 sys.* 调用参数。
 * - UI Task：唯一允许调用 LVGL 的线程；它从队列里取出 UI 指令并真正更新控件。
 *
 * 为什么要这样拆？
 * - LVGL 不是线程安全的：脚本任务绝不能直接调用 `lv_label_set_text()` 之类的 UI API。
 * - 因此脚本侧只能"发消息"（enqueue），UI 侧在自己的循环里"收消息并执行"（drain）。
 *
 * 典型调用顺序：
 * 1) `dynamic_app_init()`：创建脚本任务、初始化 UI 队列（通常在系统启动时调用一次）
 * 2) 页面 create 时：创建 Dynamic App 的 root 容器，并调用 `dynamic_app_ui_set_root(root)`（只在 `PAGE_DYNAMIC_APP` 生命周期内有效）
 * 3) 页面显示后：`dynamic_app_start()` 启动脚本（脚本侧通过 `sys.ui.createLabel(id)` / `sys.ui.setText(id, text)` 等原语自建 UI）
 * 4) UI 循环中：周期性调用 `dynamic_app_ui_drain()` 让 UI 指令尽快生效
 * 5) 页面 destroy 时：`dynamic_app_stop()` 停止脚本并 `dynamic_app_ui_unregister_all()` 清理映射表
 *
 * 线程模型：
 * - Script Task（建议 Core0）：运行 JS VM + 事件循环
 * - UI Task（建议 Core1）：drain UI 队列并执行 LVGL 操作
 */
esp_err_t dynamic_app_init(void);

/**
 * 启动指定 app（通常在页面 create/init 末尾触发）。
 *
 * @param app_name 注册表中的 app 名（"alarm" / "calc" / ...）
 *
 * 说明：
 * - 异步触发：内部通过 FreeRTOS 通知脚本任务开始工作，不会阻塞 UI。
 * - 名称会被拷贝到内部，调用方不必保留字符串生命周期。
 * - 找不到对应脚本时，脚本任务在尝试 eval 时报错并自动回到 idle 状态。
 */
void dynamic_app_start(const char *app_name);

/**
 * 停止当前动态 App（通常在页面 destroy 时触发）。
 *
 * 说明：
 * - 同样是异步触发：通知脚本任务停止；
 * - 停止后脚本任务会释放 JS 上下文、堆内存、定时器回调引用等资源（见实现）。
 */
void dynamic_app_stop(void);

void dynamic_app_ui_set_root(lv_obj_t *root);

/**
 * UI 线程：注销所有已注册的控件映射。
 *
 * 典型用途：
 * - 页面 destroy 时调用，避免 registry 里残留已删除对象的指针。
 */
void dynamic_app_ui_unregister_all(void);

/**
 * UI 线程：drain 并执行最多 max_count 条 UI 指令（限制每帧工作量，避免卡顿）。
 *
 * 建议：
 * - 在 UI 主循环里调用，并尽量放在 `page_router_update()/lvgl_port_task_handler()` 之前，
 *   这样脚本更新能尽快在同一帧显示出来。
 */
void dynamic_app_ui_drain(int max_count);

#ifdef __cplusplus
}
#endif
