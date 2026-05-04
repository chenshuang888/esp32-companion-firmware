/* ============================================================================
 * dynamic_app_runtime.c —— JS 侧"引擎层"
 *
 * 职责：
 *   1. 申请 PSRAM 上的 1MB JS 堆
 *   2. 复制并扩展 esp-mquickjs 的 stdlib c-function 表
 *      （在尾部追加我们自己的 native fn 槽位）
 *   3. 创建 / 销毁 JSContext
 *   4. 在 JS 全局对象上挂载 sys.* / setInterval / clearInterval
 *      （具体挂哪些函数和常量由 natives.c 提供）
 *   5. eval 嵌入到固件里的 app.js
 *
 * 不做的事：
 *   - 不实现 native fn 自身（在 natives.c）
 *   - 不跑 tick 循环（在 dynamic_app.c 的 script_task）
 *
 * 文件目录：
 *   §1. 嵌入资源符号
 *   §2. JS 写日志钩子 / 中断钩子
 *   §3. setup：分配堆 + 注册 cfunc 表 + 创建 ctx
 *   §4. teardown：释放 ctx + 释放堆 + 释放 cfunc 表
 *   §5. bind：挂 sys.* 到 JS 全局
 *   §6. eval：跑 app.js
 * ========================================================================= */

#include "dynamic_app_internal.h"
#include "dynamic_app_registry.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mqjs.h"

static const char *TAG = "dynamic_app_rt";

/* ============================================================================
 * §1. 嵌入资源符号
 *
 *   各 app 的脚本由 registry 统一管理，runtime 不再直接持有符号。
 *   见 dynamic_app_registry.c 的 g_apps[] 表。
 * ========================================================================= */

/* ============================================================================
 * §2. JS 写日志 / 中断钩子
 * ========================================================================= */

/* JS_SetLogFunc 的回调：把 JS 引擎自己 print 的内容输出到串口 */
static void js_write_func(void *opaque, const void *buf, size_t buf_len)
{
    (void)opaque;
    fwrite(buf, 1, buf_len, stdout);
}

/* JS_SetInterruptHandler 的回调：定期 yield 给 FreeRTOS，避免脚本霸占 CPU */
static int js_interrupt_handler(JSContext *ctx, void *opaque)
{
    (void)ctx;
    (void)opaque;
    static int64_t last_yield = 0;
    int64_t cur = dynamic_app_now_ms();
    if (cur - last_yield > 100) {
        vTaskDelay(1);
        last_yield = cur;
    }
    return 0;
}

/* ============================================================================
 * §3. setup —— 分配堆 + 注册 cfunc 表 + 创建 ctx
 *
 *   关键技巧：复制并扩展 esp-mquickjs 的 stdlib c-function 表
 *     - 上游 stdlib 的 c_function_table 是只读的固定表
 *     - 我们要在尾部追加自己的 native fn（sys.log / sys.ui.* / ...）
 *     - 所以分配一份可写拷贝，base 部分 memcpy，尾部由 natives.c 填充
 *
 *   填充流程：
 *     a) 分配大表（base + extra 项）
 *     b) memcpy 上游表的 base 部分
 *     c) 调 dynamic_app_natives_register() 让 natives.c 填充自己的索引
 *     d) 用扩展后的表创建 JSContext
 * ========================================================================= */

esp_err_t dynamic_app_runtime_setup(void)
{
    const JSSTDLibraryDef *base = esp_mqjs_get_stdlib_def();
    if (!base) return ESP_FAIL;

    size_t base_count = esp_mqjs_get_stdlib_c_function_count();
    if (base_count == 0) return ESP_FAIL;

    /*
     * 我们追加的 native 数量声明在 dynamic_app_internal.h:
     *   DYNAMIC_APP_EXTRA_NATIVE_COUNT
     * 加新 native 时改那里和 natives.c::dynamic_app_natives_register 即可，
     * 这里读宏自动跟上，不会再踩 cfunc_table 越界的坑。
     */
    const int extra = DYNAMIC_APP_EXTRA_NATIVE_COUNT;
    size_t total = base_count + (size_t)extra;

    s_rt.cfunc_table = heap_caps_malloc(total * sizeof(JSCFunctionDef),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rt.cfunc_table) return ESP_ERR_NO_MEM;

    memcpy(s_rt.cfunc_table, base->c_function_table,
           base_count * sizeof(JSCFunctionDef));
    s_rt.cfunc_table_count = total;

    /* 让 natives 模块自己分配槽位的索引并填 JSCFunctionDef */
    dynamic_app_natives_register(&s_rt, base_count);

    /* 用扩展后的表覆盖原 stdlib_def */
    s_rt.stdlib_def = *base;
    s_rt.stdlib_def.c_function_table = s_rt.cfunc_table;

    /* 申请 1MB PSRAM 当 JS 堆 */
    s_rt.js_mem_size = JS_HEAP_SIZE_BYTES;
    s_rt.js_mem = heap_caps_malloc(s_rt.js_mem_size,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rt.js_mem) return ESP_ERR_NO_MEM;

    /* 创建 JSContext */
    s_rt.ctx = JS_NewContext(s_rt.js_mem, s_rt.js_mem_size, &s_rt.stdlib_def);
    if (!s_rt.ctx) return ESP_FAIL;

    JS_SetLogFunc(s_rt.ctx, js_write_func);
    JS_SetInterruptHandler(s_rt.ctx, js_interrupt_handler);

    return ESP_OK;
}

/* ============================================================================
 * §4. teardown —— 反向释放
 *
 *   顺序：先释放回调注册表（GCRef 必须在 ctx 还活着时释放），
 *   再 JS_FreeContext，最后释放堆。
 * ========================================================================= */

void dynamic_app_runtime_teardown(void)
{
    if (s_rt.ctx) {
        dynamic_app_intervals_reset(s_rt.ctx);
        dynamic_app_ble_reset(s_rt.ctx);
        if (s_rt.dispatcher_allocated) {
            JS_DeleteGCRef(s_rt.ctx, &s_rt.dispatcher);
            s_rt.dispatcher_allocated = false;
        }
        JS_FreeContext(s_rt.ctx);
        s_rt.ctx = NULL;
    }

    if (s_rt.js_mem) {
        heap_caps_free(s_rt.js_mem);
        s_rt.js_mem = NULL;
        s_rt.js_mem_size = 0;
    }

    if (s_rt.cfunc_table) {
        heap_caps_free(s_rt.cfunc_table);
        s_rt.cfunc_table = NULL;
        s_rt.cfunc_table_count = 0;
    }
}

/* ============================================================================
 * §5. bind —— 把 sys.* 挂到 JS 全局
 *
 *   只是个薄壳，真正干活的是 natives.c 的 dynamic_app_natives_bind()。
 *   这里保留壳的原因：保持 runtime 这一层"完整封装 JS 引擎"的语义对称。
 * ========================================================================= */

esp_err_t dynamic_app_runtime_bind_globals(JSContext *ctx)
{
    if (!ctx) return ESP_ERR_INVALID_ARG;
    return dynamic_app_natives_bind(ctx);
}

/* ============================================================================
 * §6. eval —— 跑嵌入的 app.js
 * ========================================================================= */

esp_err_t dynamic_app_runtime_eval_app(JSContext *ctx)
{
    const char *name = dynamic_app_registry_current();
    if (!name || name[0] == '\0') {
        ESP_LOGE(TAG, "no current app set");
        return ESP_FAIL;
    }

    /* Step 1: 注入标准库 prelude（VDOM / makeBle / setDispatcher）。
     * 业务脚本因此能直接使用 VDOM / h / makeBle 等全局符号。 */
    const uint8_t *pre_buf = NULL;
    size_t pre_len = 0;
    dynamic_app_registry_get_prelude(&pre_buf, &pre_len);
    if (pre_buf && pre_len > 0) {
        JSValue pre = JS_Eval(ctx, (const char *)pre_buf, pre_len,
                              "prelude.js", 0);
        if (JS_IsException(pre)) {
            ESP_LOGE(TAG, "eval prelude failed");
            dynamic_app_dump_exception(ctx);
            return ESP_FAIL;
        }
    } else {
        ESP_LOGW(TAG, "prelude empty; business app may miss VDOM/makeBle");
    }

    /* Step 2: eval 业务脚本。 */
    const uint8_t *buf = NULL;
    size_t buf_len = 0;
    if (!dynamic_app_registry_get(name, &buf, &buf_len) ||
        !buf || buf_len == 0)
    {
        ESP_LOGE(TAG, "app '%s' not found in registry", name);
        return ESP_FAIL;
    }

    /* JS 异常文件名用 app 名，便于定位 */
    char filename[24];
    snprintf(filename, sizeof(filename), "%s.js", name);

    JSValue val = JS_Eval(ctx, (const char *)buf, buf_len, filename, 0);
    /* eval 完即可释放（JS 已经 parse 进自己的字节码）；
     * 内嵌 buf 是 rodata，release 是 no-op；FS buf 是 heap，必须 free。 */
    dynamic_app_registry_release(buf);
    if (JS_IsException(val)) {
        dynamic_app_dump_exception(ctx);
        return ESP_FAIL;
    }
    return ESP_OK;
}
