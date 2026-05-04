#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "dynapp_script_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * dynapp_fs_worker —— LittleFS 后台写入串行化器
 *
 * 角色：
 *   独占一个 task + queue，把所有 dynapp 文件 IO 串行化，
 *   让任何线程（BLE host task / JS script_task / 未来 WiFi 等）都能
 *   "非阻塞地丢一个写盘任务"，避免：
 *     - JS UI 任务被 fwrite 阻塞
 *     - 多线程同时碰 LittleFS（LittleFS 不是线程安全的）
 *
 * 与 dynapp_script_store 的分工：
 *   script_store 是"做什么"的库（路径策略、原子写、manifest 解析）
 *   fs_worker    是"什么时候做"的调度器（队列 + 串行 task）
 *
 * 职责边界：
 *   不知道任何 BLE 协议字段（op/seq/result）；这些由 callback 在调用方处理。
 *   也不知道 JS；natives 直接 submit 即可。
 *
 * 线程模型：
 *   submit_*  —— 任意线程可调；非阻塞 (xQueueSend timeout=0)
 *   done cb   —— 在 fs_worker task 上执行；cb 实现里禁阻塞 / 禁碰 LVGL
 */

/* ============================================================================
 * 生命周期
 * ========================================================================= */

esp_err_t dynapp_fs_worker_init(void);

/* ============================================================================
 * fire-and-forget：失败只在 worker 内部打 log，调用方不感知
 * ========================================================================= */

/* JS sys.fs.write —— 写入当前 app 沙箱 (apps/<id>/data/<rel>) */
bool dynapp_fs_worker_submit_user_write(const char *app_id, const char *relpath,
                                         const uint8_t *data, size_t len);

/* JS sys.fs.write 大块路径：超过 FS_CHUNK_MAX_BYTES 时走这条。
 * 内部把 data 拷一份到 PSRAM 再入队，调用方可立即释放原 buf。
 * 上限 DYNAPP_USER_DATA_MAX_BYTES（256KB）。失败已释放拷贝、返回 false。 */
bool dynapp_fs_worker_submit_user_write_large(const char *app_id,
                                               const char *relpath,
                                               const uint8_t *data, size_t len);

bool dynapp_fs_worker_submit_user_remove(const char *app_id, const char *relpath);

/* BLE 上传：开 writer / 追加 chunk / 取消。
 *
 * open 后有一个隐式 "active writer" 状态：append/abort/commit 都作用于它。
 * 跨任务 submit 不会乱：所有 submit 都进同一个队列，worker 串行执行。
 *
 * open 失败（FS 满 / 名字非法）会让 active writer 变 NULL，后续 append/commit
 * 都会被静默丢掉直到下一次 open。调用方可以靠 commit 的 done cb 拿到错误。 */
bool dynapp_fs_worker_submit_writer_open(const char *app_id, const char *filename);
bool dynapp_fs_worker_submit_writer_append(const uint8_t *data, size_t len);
bool dynapp_fs_worker_submit_writer_abort(void);

/* ============================================================================
 * 带 done callback：worker task 上回调
 * ========================================================================= */

/* commit 结果回调。result 直接是 dynapp_script_writer_commit 的 esp_err_t。 */
typedef void (*dynapp_fs_writer_done_cb_t)(esp_err_t result, void *cb_arg);
bool dynapp_fs_worker_submit_writer_commit(dynapp_fs_writer_done_cb_t cb,
                                            void *cb_arg);

/* delete app 结果回调。result：ESP_OK / ESP_ERR_NOT_FOUND / ESP_FAIL。 */
typedef void (*dynapp_fs_delete_done_cb_t)(esp_err_t result, void *cb_arg);
bool dynapp_fs_worker_submit_app_delete(const char *app_id,
                                         dynapp_fs_delete_done_cb_t cb,
                                         void *cb_arg);

/* list 结果回调。
 *   names 是 worker task 栈上的数组指针，cb 必须同步消费完（不能保留指针）。
 *   count = 实际条数。
 */
typedef void (*dynapp_fs_list_done_cb_t)(
    const char names[][DYNAPP_SCRIPT_STORE_MAX_NAME + 1],
    int count, void *cb_arg);
bool dynapp_fs_worker_submit_list_apps(dynapp_fs_list_done_cb_t cb,
                                        void *cb_arg);

/* ============================================================================
 * "running check" hook —— delete 前调一次，返回 true 则拒删（result=ESP_ERR_INVALID_STATE）
 *
 * 由上层（dynamic_app）注册：worker 不知道 dynamic_app 是什么，避免循环依赖。
 * ========================================================================= */

typedef bool (*dynapp_fs_app_running_cb_t)(const char *app_id);
void dynapp_fs_worker_set_running_check(dynapp_fs_app_running_cb_t cb);

#ifdef __cplusplus
}
#endif
