#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * dynapp_bridge_service —— 给 dynamic app 用的"通用 BLE 透传管道"
 *
 * 设计目标：
 *   - 一个固定的 GATT service + 两个 characteristic，开机就注册死，无需运行时增删
 *   - 完全把 payload 当透明字节流处理，不解析格式，应用层（JS + PC）自己用 JSON 等
 *   - 跨线程安全：BLE host task 入 inbox 队列、script task 出队；script task 入 outbox
 *     队列、host task 触发 notify
 *
 * GATT 表：
 *   Service: a3a30001-0000-4aef-b87e-4fa1e0c7e0f6
 *     ├─ char "rx": a3a30002-0000-...      WRITE         (PC → ESP)
 *     └─ char "tx": a3a30003-0000-...      READ + NOTIFY (ESP → PC)
 *
 * 容量约束：
 *   单条 payload ≤ DYNAPP_BRIDGE_MAX_PAYLOAD（200 byte），超长由应用层自己分包
 *   inbox/outbox 各 8 槽，满策略不一样：
 *     inbox 满 → 丢最老（PC 主动写无 backpressure，丢老的体验更好）
 *     outbox 满 → 当前 send 返 false（让 JS 自己决定重试）
 *
 * 线程模型：
 *   inbox 写：host task 在 access_cb 里调 push_from_isr_safe（实际不是 ISR，
 *             但 NimBLE 不允许长阻塞，用 0 timeout）
 *   inbox 读：script task 在 tick 里轮询 pop
 *   outbox 写：script task 在 sys.ble.send 里调
 *   outbox 读：host task 通过 host_task_drain_outbox（由 nimble 内部 timer 触发）
 *             —— 实际更简单的做法：script task 直接 ble_gatts_notify_custom
 *             也是线程安全的；为简化先这么做，无需独立 outbox 队列
 */

#define DYNAPP_BRIDGE_MAX_PAYLOAD 200
#define DYNAPP_BRIDGE_INBOX_LEN     8

/* ---- 生命周期 ---- */

/* 在 nimble_init 之后、nimble_start 之前调用 */
esp_err_t dynapp_bridge_service_init(void);

/* ---- script_task 一侧的 inbox 接口 ---- */

typedef struct {
    uint16_t len;                                       /* payload 实际字节数 */
    uint8_t  data[DYNAPP_BRIDGE_MAX_PAYLOAD];           /* payload 内容 */
} dynapp_bridge_msg_t;

/* 取一条来自 PC 的消息。无消息返回 false。
 * 仅在 script_task 调用。 */
bool dynapp_bridge_pop_inbox(dynapp_bridge_msg_t *out);

/* 清空 inbox（app 切换时用，避免上个 app 漏的消息发给下个 app） */
void dynapp_bridge_clear_inbox(void);

/* 主动 push 一条消息到 inbox（mailbox replay 用，把 NVS 归档的消息回灌）。
 * len 必须 ≤ DYNAPP_BRIDGE_MAX_PAYLOAD。返回 false 表示 inbox 满 / 参数错。
 * 不像 access_cb 那样满时丢老的——回灌是同步操作，调用方自己决定怎么处理失败。 */
bool dynapp_bridge_push_inbox(const uint8_t *data, uint16_t len);

/* ---- script_task 一侧的发送接口 ---- */

/* 把 payload 推给 PC。要求当前已有 central 连接 + PC 已订阅 tx char。
 * 调用线程：script_task。本函数内部调 ble_gatts_notify_custom，NimBLE 自带锁。
 *
 * 返回 false 的情况：
 *   - 未连接 / PC 未订阅 / payload 过长 / NimBLE mbuf 申请失败
 * JS 侧应根据返回值决定丢弃还是稍后重试。 */
bool dynapp_bridge_send(const uint8_t *data, size_t len);

/* ---- 状态查询 ---- */

/* 当前是否有 PC 连接（不区分是否已订阅 tx）。 */
bool dynapp_bridge_is_connected(void);

#ifdef __cplusplus
}
#endif
