/* ============================================================================
 * dynamic_app.c —— JS 侧"控制层"
 *
 * 职责：
 *   这是整个模块的总入口。只负责：
 *     1. 定义全局 runtime 实例 s_rt
 *     2. 对外公共 API：dynamic_app_init / start / stop
 *     3. Script Task 主循环：等 start → 跑脚本 → tick 循环 → 等 stop
 *
 * 分层关系：
 *
 *     dynamic_app.h (对外 API)
 *           ↓
 *   ┌───────────────────┐
 *   │  dynamic_app.c    │ 本文件：start/stop 入口 + script_task 主循环
 *   └────────┬──────────┘
 *            ↓
 *   ┌───────────────────┐
 *   │ runtime.c         │ 引擎层：创建/销毁 JSContext、绑全局、eval app.js
 *   └────────┬──────────┘
 *            ↓
 *   ┌───────────────────┐
 *   │ natives.c         │ API 层：所有 sys.* 函数实现 + cfunc 注册 + tick 服务
 *   └───────────────────┘
 *
 * 文件目录：
 *   §1. 全局状态
 *   §2. 跨文件 helper（now_ms / dump_exception）
 *   §3. script_task 主循环
 *   §4. 公共 API：init / start / stop
 * ========================================================================= */

#include "dynamic_app.h"
#include "dynamic_app_internal.h"
#include "dynamic_app_registry.h"
#include "dynamic_app_ui.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "dynapp_bridge_service.h"
#include "dynapp_mailbox.h"

static const char *TAG = "dynamic_app";

/* ============================================================================
 * §1. 全局状态
 *
 *   整个模块只有一个 runtime 实例。所有 .c 文件通过 extern 共享。
 * ========================================================================= */

dynamic_app_runtime_t s_rt = {0};

/* ============================================================================
 * §2. 跨文件 helper
 * ========================================================================= */

int64_t dynamic_app_now_ms(void)
{
    return (int64_t)(esp_timer_get_time() / 1000);
}

void dynamic_app_dump_exception(JSContext *ctx)
{
    JSValue ex = JS_GetException(ctx);
    JS_PrintValueF(ctx, ex, JS_DUMP_LONG);
    fwrite("\n", 1, 1, stdout);
}

/* ============================================================================
 * §3. script_task 主循环
 *
 *   简化状态机：
 *     [等通知] ── NOTIFY_START ──→ [启动脚本]
 *                                      │
 *                                      ↓
 *                                 [tick 循环]：每轮做三件事
 *                                    1. drain_ui_events_once  消化点击事件
 *                                    2. run_intervals_once    跑到期的定时器
 *                                    3. vTaskDelay(<=50ms)    省电并保持 stop 响应
 *                                      │
 *                                      ↓
 *                                 [收到 NOTIFY_STOP 或 JS 异常]
 *                                      │
 *                                      ↓
 *                                 [释放上下文] → 回到 [等通知]
 * ========================================================================= */

static void script_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "script task started");

    while (1) {
        /* ——— 外层：阻塞等 start 通知 ——— */
        uint32_t notify_val = 0;
        xTaskNotifyWait(0, UINT32_MAX, &notify_val, portMAX_DELAY);

        /* start 和 stop 同时到达视为取消启动 */
        if ((notify_val & NOTIFY_START) && (notify_val & NOTIFY_STOP)) {
            continue;
        }
        if (!(notify_val & NOTIFY_START)) continue;
        if (s_rt.app_running) continue;

        ESP_LOGI(TAG, "starting dynamic app: %s",
                 dynamic_app_registry_current());

        /* 每次启动都彻底清零 interval 表（避免上次 stop 残留） */
        memset(s_rt.intervals, 0, sizeof(s_rt.intervals));
        /* 上一个 app 没消费完的 PC 消息也清掉，避免串台 */
        dynapp_bridge_clear_inbox();
        /* 通知 mailbox 让位（停止从 inbox 抢消息），然后把这个 app 离线期间
         * 攒在 NVS 的消息回灌进 inbox。replay 完 inbox 里就有"待 JS 注册 onRecv
         * 后第一轮 drain 消化"的历史消息。 */
        dynapp_mailbox_set_js_active(true);
        dynapp_mailbox_replay(dynamic_app_registry_current());
        s_rt.app_running = true;

        /* ——— 三步走：setup → bind → eval ——— */
        if (dynamic_app_runtime_setup() != ESP_OK) {
            ESP_LOGE(TAG, "runtime setup failed");
            dynamic_app_runtime_teardown();
            s_rt.app_running = false;
            dynapp_mailbox_set_js_active(false);
            continue;
        }
        if (dynamic_app_runtime_bind_globals(s_rt.ctx) != ESP_OK) {
            ESP_LOGE(TAG, "bind globals failed");
            dynamic_app_runtime_teardown();
            s_rt.app_running = false;
            dynapp_mailbox_set_js_active(false);
            continue;
        }
        if (dynamic_app_runtime_eval_app(s_rt.ctx) != ESP_OK) {
            ESP_LOGE(TAG, "eval app failed: %s", dynamic_app_registry_current());
            dynamic_app_runtime_teardown();
            s_rt.app_running = false;
            dynapp_mailbox_set_js_active(false);
            continue;
        }

        /* ——— 内层：tick 循环 ——— */
        while (s_rt.app_running) {
            uint32_t ev = 0;
            if (xTaskNotifyWait(0, UINT32_MAX, &ev, 0) == pdTRUE) {
                if (ev & NOTIFY_STOP) break;
            }

            int64_t cur = dynamic_app_now_ms();

            /* UI → Script 反向事件：先跑 onClick 回调 */
            dynamic_app_drain_ui_events_once(s_rt.ctx);

            /* BLE → Script 反向事件：把 PC 推来的消息送给 sys.ble.onRecv */
            dynamic_app_drain_ble_inbox_once(s_rt.ctx);

            /* 定时器 */
            if (!dynamic_app_run_intervals_once(s_rt.ctx, cur)) {
                ESP_LOGE(TAG, "JS exception, stopping app");
                break;
            }

            /* sleep：到下一个 deadline 或最多 50ms，保证 stop 响应及时 */
            int64_t deadline = dynamic_app_next_interval_deadline_ms(cur);
            int64_t sleep_ms = deadline - cur;
            if (sleep_ms < 1) sleep_ms = 1;
            if (sleep_ms > 50) sleep_ms = 50;
            vTaskDelay(pdMS_TO_TICKS((uint32_t)sleep_ms));
        }

        ESP_LOGI(TAG, "stopping dynamic app");
        dynamic_app_runtime_teardown();
        s_rt.app_running = false;
        /* 重新让 mailbox 接管 inbox：app 退出后 PC 推来的消息会被归档到 NVS */
        dynapp_mailbox_set_js_active(false);
    }
}

/* ============================================================================
 * §4. 公共 API
 * ========================================================================= */

esp_err_t dynamic_app_init(void)
{
    /* 先初始化 UI 桥接层（命令队列 / 事件队列 / registry） */
    ESP_RETURN_ON_ERROR(dynamic_app_ui_init(), TAG, "ui init failed");

    if (s_rt.task) return ESP_OK;

    /* 脚本任务绑 Core 0（UI 任务通常在 Core 1），减少互相抢占 */
    BaseType_t ret = xTaskCreatePinnedToCore(script_task, "script_task",
                                             16384, NULL, 4, &s_rt.task, 0);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create script task");
        return ESP_FAIL;
    }
    return ESP_OK;
}

void dynamic_app_start(const char *app_name)
{
    if (!s_rt.task) return;
    /* 通过 registry 的 current 字段把 app 名带到 script_task。
     * NotifyValue 是 32 位 bit mask，没法直接传字符串；
     * 在 notify 之前写入是安全的：start 是 idempotent 的 ack，
     * script_task 一旦看到 NOTIFY_START 就会读 current 并 eval。 */
    dynamic_app_registry_set_current(app_name ? app_name : "");
    xTaskNotify(s_rt.task, NOTIFY_START, eSetBits);
}

void dynamic_app_stop(void)
{
    if (!s_rt.task) return;
    xTaskNotify(s_rt.task, NOTIFY_STOP, eSetBits);
}
