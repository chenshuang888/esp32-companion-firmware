#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * dynapp_mailbox —— 动态 app 离线消息兜底。
 *
 * 接力模型：
 *   - 当某个动态 app 没有运行时，dynapp_bridge.s_inbox 来的 BLE 消息没人 drain。
 *     mailbox 后台 task 在这种情况下接管 drain，按 payload 里的 "to":"xxx"
 *     字段把消息归档到 NVS（每个 app 一个 blob，最多 N 条 FIFO）。
 *   - 当动态 app 启动（script_task 进入 NOTIFY_START 分支）时调用
 *     dynapp_mailbox_set_js_active(true)，mailbox task 立即让位停止 drain。
 *     script_task 紧跟着调 dynapp_mailbox_replay(app_id) 把这个 app 的 NVS
 *     blob 逐条回灌进 s_inbox，然后清空该 blob；JS 端 onRecv 一旦注册就会
 *     当作"实时消息"全部消化。
 *   - app 退出时 set_js_active(false)，mailbox task 重新接管。
 *
 * 关键约束：
 *   - 单写者：仅 mailbox task 调 NVS write；replay 也走 mailbox task（避免
 *     script_task 直接写 NVS）。replay 通过队列同步。
 *   - 透明 payload：mailbox 不解析 JSON，仅 strstr("\"to\":\"") 提取 app_id 当
 *     NVS key，对其余字节 0 修改。
 *   - 容量保护：每个 app blob 上限 30 条，超了丢最老。
 */

#define DYNAPP_MAILBOX_PER_APP_MAX 30

/* 启动 mailbox 后台 task。在 dynapp_bridge_service_init / persist_init / nimble_start
 * 之后调用一次。 */
esp_err_t dynapp_mailbox_init(void);

/* 翻 active flag。
 *   true  = JS app 已起，mailbox 让位（不再从 inbox 取消息）
 *   false = JS app 没在跑，mailbox 接管 drain + 归档 NVS
 *
 * 调用线程：script_task。线程安全（atomic）。 */
void dynapp_mailbox_set_js_active(bool active);

/* 回灌：把 NVS blob 里 app_id 的待投递消息**全部 push 回 s_inbox**，然后清空
 * 该 blob。同步阻塞，最多等 200ms（等 mailbox task 完成）。
 *
 * 调用线程：script_task，在 dynapp_bridge_clear_inbox 之后、JS eval 之前调。
 * 失败（超时 / NVS 错误 / blob 不存在）静默返回，不影响 app 启动。
 *
 * 注意：mailbox task 会先 set_js_active(true) 再走 replay 路径，避免 race。 */
void dynapp_mailbox_replay(const char *app_id);

#ifdef __cplusplus
}
#endif
