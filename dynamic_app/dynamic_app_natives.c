/* ============================================================================
 * dynamic_app_natives.c —— JS 侧"API 层"
 *
 * 职责：
 *   把 C 系统能力翻译成 JS 函数。所有 JS 脚本能调到的 sys.xxx 都在这里实现。
 *
 * 文件目录：
 *   §1. 通用 helper（参数解析、id/parent 提取、interval 表 reset）
 *   §2. JS Native：sys.log
 *   §3. JS Native：sys.ui.*  （setText/createLabel/createPanel/createButton/
 *                              setStyle/attachRootListener/__setDispatcher）
 *   §4. JS Native：sys.time.* （uptimeMs/uptimeStr）
 *   §5. JS Native：setInterval / clearInterval
 *   §6. tick 循环服务（run_intervals_once / drain_ui_events_once / next_deadline）
 *   §7. cfunc 表注册：register（runtime.c 调）
 *   §8. JS 全局对象绑定：bind（runtime.c 调）
 *
 * 单个 native fn 的标准三段式（参考 §3 第一个例子）：
 *   1. argc 校验，必要时 ThrowTypeError
 *   2. JS_ToCStringLen / JS_ToInt32 把 JS 参数取出来
 *   3. 调下层 enqueue_* / 系统 API
 * ========================================================================= */

#include "dynamic_app_internal.h"
#include "dynamic_app_registry.h"
#include "dynamic_app_ui.h"
#include "dynapp_bridge_service.h"
#include "dynapp_fs_worker.h"
#include "dynapp_script_store.h"
#include "persist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_mqjs.h"
#include "lvgl.h"   /* 只为拿 LV_SYMBOL_* 字面量挂到 sys.symbols */

static const char *TAG = "dynamic_app_natives";

/* ============================================================================
 * §1. 通用 helper
 * ========================================================================= */

/* 从 argv[i] 取一个 string|null|undefined 当作 parent_id：
 *   null/undefined → 表示"无 parent"（pid=NULL, plen=0），让下层回落到 root
 *   string         → 把字符串指针写出
 *   其它类型       → ThrowTypeError 并返回 false
 *
 * holder 持有 JSCStringBuf，调用方栈上分配；valid==true 时 pid 才有效。
 */
typedef struct {
    JSCStringBuf buf;
    bool valid;
} parent_str_t;

static bool extract_parent_id(JSContext *ctx, JSValue v,
                              const char **out_pid, size_t *out_len,
                              parent_str_t *holder)
{
    holder->valid = false;
    *out_pid = NULL;
    *out_len = 0;

    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        return true;   /* 落到 root */
    }
    if (!JS_IsString(ctx, v)) {
        JS_ThrowTypeError(ctx, "parent must be string|null|undefined");
        return false;
    }
    const char *s = JS_ToCStringLen(ctx, out_len, v, &holder->buf);
    if (!s) return false;
    holder->valid = true;
    *out_pid = s;
    return true;
}

/* setInterval 注册的 GCRef 在 teardown 时统一释放。 */
void dynamic_app_intervals_reset(JSContext *ctx)
{
    for (int i = 0; i < MAX_INTERVALS; i++) {
        if (s_rt.intervals[i].allocated) {
            JS_DeleteGCRef(ctx, &s_rt.intervals[i].func);
            s_rt.intervals[i].allocated = false;
        }
    }
}

/* ============================================================================
 * §2. JS Native：sys.log
 * ========================================================================= */

static JSValue js_sys_log(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;

    JSCStringBuf buf;
    size_t len = 0;
    const char *s = JS_ToCStringLen(ctx, &len, argv[0], &buf);
    if (!s) return JS_EXCEPTION;

    ESP_LOGI("dynapp", "%.*s", (int)len, s);
    return JS_UNDEFINED;
}

/* ============================================================================
 * §3. JS Native：sys.ui.*
 *
 *   全部走"取参数 → 入队 → 返回 bool"模式。
 *   绝不直接调 LVGL，所有动作都丢给 UI 线程的 dynamic_app_ui_drain。
 * ========================================================================= */

/* sys.ui.setText(id, text) */
static JSValue js_sys_ui_set_text(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)this_val;
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "sys.ui.setText(id, text) args missing");
    }

    JSCStringBuf id_buf, text_buf;
    size_t id_len = 0, text_len = 0;

    const char *id   = JS_ToCStringLen(ctx, &id_len,   argv[0], &id_buf);
    if (!id) return JS_EXCEPTION;
    const char *text = JS_ToCStringLen(ctx, &text_len, argv[1], &text_buf);
    if (!text) return JS_EXCEPTION;

    (void)dynamic_app_ui_enqueue_set_text(id, id_len, text, text_len);
    return JS_UNDEFINED;
}

/* createLabel/Panel/Button 的公共骨架。typed enqueue 函数指针由调用方传入。 */
typedef bool (*enqueue_create_fn_t)(const char *, size_t, const char *, size_t);

static JSValue js_create_widget_common(JSContext *ctx, int argc, JSValue *argv,
                                       const char *fn_name,
                                       enqueue_create_fn_t enq)
{
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, fn_name);
    }

    JSCStringBuf id_buf;
    size_t id_len = 0;
    const char *id = JS_ToCStringLen(ctx, &id_len, argv[0], &id_buf);
    if (!id) return JS_EXCEPTION;

    parent_str_t ph;
    const char *pid = NULL;
    size_t plen = 0;
    if (argc >= 2 && !extract_parent_id(ctx, argv[1], &pid, &plen, &ph)) {
        return JS_EXCEPTION;
    }

    bool ok = enq(id, id_len, pid, plen);
    return JS_NewBool(ok ? 1 : 0);
}

/* sys.ui.createLabel(id, parent?) */
static JSValue js_sys_ui_create_label(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)this_val;
    return js_create_widget_common(ctx, argc, argv,
        "sys.ui.createLabel(id, parent?) args missing",
        dynamic_app_ui_enqueue_create_label);
}

/* sys.ui.createPanel(id, parent?) */
static JSValue js_sys_ui_create_panel(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)this_val;
    return js_create_widget_common(ctx, argc, argv,
        "sys.ui.createPanel(id, parent?) args missing",
        dynamic_app_ui_enqueue_create_panel);
}

/* sys.ui.createButton(id, parent?) */
static JSValue js_sys_ui_create_button(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)this_val;
    return js_create_widget_common(ctx, argc, argv,
        "sys.ui.createButton(id, parent?) args missing",
        dynamic_app_ui_enqueue_create_button);
}

/* sys.ui.createImage(id, parent, src?)
 *   id     : 节点 id
 *   parent : string|null|undefined（同 createPanel）
 *   src    : 资源相对名（相对当前 app 的 assets/，如 "fish.bin"）；可省略
 */
static JSValue js_sys_ui_create_image(JSContext *ctx, JSValue *this_val,
                                      int argc, JSValue *argv)
{
    (void)this_val;
    if (argc < 1) {
        return JS_ThrowTypeError(ctx,
            "sys.ui.createImage(id, parent?, src?) args missing");
    }

    JSCStringBuf id_buf;
    size_t id_len = 0;
    const char *id = JS_ToCStringLen(ctx, &id_len, argv[0], &id_buf);
    if (!id) return JS_EXCEPTION;

    parent_str_t ph;
    const char *pid = NULL;
    size_t plen = 0;
    if (argc >= 2 && !extract_parent_id(ctx, argv[1], &pid, &plen, &ph)) {
        return JS_EXCEPTION;
    }

    JSCStringBuf src_buf;
    const char *src = NULL;
    size_t slen = 0;
    bool src_valid = false;
    if (argc >= 3 && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2])) {
        if (!JS_IsString(ctx, argv[2])) {
            return JS_ThrowTypeError(ctx, "sys.ui.createImage: src must be string|null");
        }
        src = JS_ToCStringLen(ctx, &slen, argv[2], &src_buf);
        if (!src) return JS_EXCEPTION;
        src_valid = true;
    }

    bool ok = dynamic_app_ui_enqueue_create_image(id, id_len,
                                                  pid, plen,
                                                  src_valid ? src : NULL,
                                                  src_valid ? slen : 0);
    return JS_NewBool(ok ? 1 : 0);
}

/* sys.ui.setImageSrc(id, src)  src=null/"" → 清空 */
static JSValue js_sys_ui_set_image_src(JSContext *ctx, JSValue *this_val,
                                       int argc, JSValue *argv)
{
    (void)this_val;
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "sys.ui.setImageSrc(id, src) args missing");
    }

    JSCStringBuf id_buf;
    size_t id_len = 0;
    const char *id = JS_ToCStringLen(ctx, &id_len, argv[0], &id_buf);
    if (!id) return JS_EXCEPTION;

    JSCStringBuf src_buf;
    const char *src = NULL;
    size_t slen = 0;
    if (!JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        if (!JS_IsString(ctx, argv[1])) {
            return JS_ThrowTypeError(ctx, "sys.ui.setImageSrc: src must be string|null");
        }
        src = JS_ToCStringLen(ctx, &slen, argv[1], &src_buf);
        if (!src) return JS_EXCEPTION;
    }

    (void)dynamic_app_ui_enqueue_set_image_src(id, id_len, src, slen);
    return JS_UNDEFINED;
}

/* sys.ui.setStyle(id, key, a, b?, c?, d?)
 *   注：JS_ToInt32 在 esp-mquickjs 是 (ctx, int*, val)，
 *       int32_t* 不兼容（xtensa 上 int32_t = long int），必须用 int 接。
 */
static JSValue js_sys_ui_set_style(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)this_val;
    if (argc < 3) {
        return JS_ThrowTypeError(ctx, "sys.ui.setStyle(id, key, a, b?, c?, d?) args missing");
    }

    JSCStringBuf id_buf;
    size_t id_len = 0;
    const char *id = JS_ToCStringLen(ctx, &id_len, argv[0], &id_buf);
    if (!id) return JS_EXCEPTION;

    int key = 0;
    if (JS_ToInt32(ctx, &key, argv[1])) return JS_EXCEPTION;

    int a = 0, b = 0, c = 0, d = 0;
    if (JS_ToInt32(ctx, &a, argv[2])) return JS_EXCEPTION;
    if (argc >= 4 && JS_ToInt32(ctx, &b, argv[3])) return JS_EXCEPTION;
    if (argc >= 5 && JS_ToInt32(ctx, &c, argv[4])) return JS_EXCEPTION;
    if (argc >= 6 && JS_ToInt32(ctx, &d, argv[5])) return JS_EXCEPTION;

    bool ok = dynamic_app_ui_enqueue_set_style(id, id_len,
        (dynamic_app_style_key_t)key,
        (int32_t)a, (int32_t)b, (int32_t)c, (int32_t)d);
    return JS_NewBool(ok ? 1 : 0);
}

/* sys.ui.attachRootListener(id)
 *
 *   事件委托入口：在指定 id 对象上挂一个 LVGL cb。
 *   之后所有冒泡到该对象的指针事件，都会被 on_lv_root_event 捕获，
 *   然后以"被点中真子对象的 id"为 payload 入队，由 sys.__setDispatcher
 *   注册的 JS 函数派发。
 *   不持有 JS 函数引用 —— dispatcher 是脚本侧的全局函数。
 */
static JSValue js_sys_ui_attach_root_listener(JSContext *ctx, JSValue *this_val,
                                              int argc, JSValue *argv)
{
    (void)this_val;
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "sys.ui.attachRootListener(id) args missing");
    }
    JSCStringBuf id_buf;
    size_t id_len = 0;
    const char *id = JS_ToCStringLen(ctx, &id_len, argv[0], &id_buf);
    if (!id) return JS_EXCEPTION;

    bool ok = dynamic_app_ui_enqueue_attach_root_listener(id, id_len);
    return JS_NewBool(ok ? 1 : 0);
}

/* sys.ui.destroy(id)
 *
 *   销毁单个对象。约定 JS 侧（VDOM.destroy）自底向上递归调用，
 *   保证子节点的 registry slot 先被释放，再销毁父节点。
 *   C 侧只负责"删一个 LVGL obj + 释放一个 slot"，不做递归判断。
 */
static JSValue js_sys_ui_destroy(JSContext *ctx, JSValue *this_val,
                                 int argc, JSValue *argv)
{
    (void)this_val;
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "sys.ui.destroy(id) args missing");
    }
    JSCStringBuf id_buf;
    size_t id_len = 0;
    const char *id = JS_ToCStringLen(ctx, &id_len, argv[0], &id_buf);
    if (!id) return JS_EXCEPTION;

    bool ok = dynamic_app_ui_enqueue_destroy(id, id_len);
    return JS_NewBool(ok ? 1 : 0);
}

/* sys.__setDispatcher(fn)
 *
 *   JS 侧注册一个全局 dispatcher 函数，C 侧用 GCRef 持有。
 *   所有 root delegation 路径的点击都通过这个 fn 派发，C 侧不需要
 *   反查 globalThis.__dynapp_dispatch（esp-mquickjs 顶层 this 为 undefined，
 *   那条路走不通）。teardown 时 GCRef 一起释放，不泄漏。
 *   重复调用时，先释放旧的再记录新的。
 */
static JSValue js_sys_set_dispatcher(JSContext *ctx, JSValue *this_val,
                                     int argc, JSValue *argv)
{
    (void)this_val;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "sys.__setDispatcher(fn): not a function");
    }
    if (s_rt.dispatcher_allocated) {
        JS_DeleteGCRef(ctx, &s_rt.dispatcher);
        s_rt.dispatcher_allocated = false;
    }
    JSValue *p = JS_AddGCRef(ctx, &s_rt.dispatcher);
    *p = argv[0];
    s_rt.dispatcher_allocated = true;
    return JS_UNDEFINED;
}

/* ============================================================================
 * §4. JS Native：sys.time.*
 * ========================================================================= */

static JSValue js_sys_time_uptime_ms(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return JS_NewInt64(ctx, dynamic_app_now_ms());
}

static JSValue js_sys_time_uptime_str(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)this_val; (void)argc; (void)argv;

    int64_t ms = dynamic_app_now_ms();
    int64_t sec_total = ms / 1000;
    int hours   = (int)(sec_total / 3600);
    int minutes = (int)((sec_total / 60) % 60);
    int seconds = (int)(sec_total % 60);

    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", hours % 100, minutes, seconds);
    return JS_NewString(ctx, buf);
}

/* sys.time.now() -> int  当前 unix 秒（OS 系统时间，由 NTP 或 PC 同步获得）
 *  esp-mquickjs 没有 JS_NewInt64，而 unix 秒到 2038 之前用 int32 够；
 *  超过 2038 后再考虑加双精度路径。 */
static JSValue js_sys_time_now(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)this_val; (void)argc; (void)argv;
    time_t now = time(NULL);
    return JS_NewInt32(ctx, (int32_t)now);
}

/* sys.time.parts(unix_ts) -> { y, mo, d, h, mi, s, wday, yday }
 *   wday : 0=周日 .. 6=周六（与 struct tm 一致） */
static JSValue js_sys_time_parts(JSContext *ctx, JSValue *this_val,
                                 int argc, JSValue *argv)
{
    (void)this_val;
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "sys.time.parts(unix_ts) args missing");
    }
    int ts32 = 0;
    if (JS_ToInt32(ctx, &ts32, argv[0])) return JS_EXCEPTION;

    time_t t = (time_t)(int32_t)ts32;
    struct tm tm;
    localtime_r(&t, &tm);

    JSValue obj = JS_NewObject(ctx);
    if (JS_IsException(obj)) return obj;
    (void)JS_SetPropertyStr(ctx, obj, "y",    JS_NewInt32(ctx, tm.tm_year + 1900));
    (void)JS_SetPropertyStr(ctx, obj, "mo",   JS_NewInt32(ctx, tm.tm_mon + 1));
    (void)JS_SetPropertyStr(ctx, obj, "d",    JS_NewInt32(ctx, tm.tm_mday));
    (void)JS_SetPropertyStr(ctx, obj, "h",    JS_NewInt32(ctx, tm.tm_hour));
    (void)JS_SetPropertyStr(ctx, obj, "mi",   JS_NewInt32(ctx, tm.tm_min));
    (void)JS_SetPropertyStr(ctx, obj, "s",    JS_NewInt32(ctx, tm.tm_sec));
    (void)JS_SetPropertyStr(ctx, obj, "wday", JS_NewInt32(ctx, tm.tm_wday));
    (void)JS_SetPropertyStr(ctx, obj, "yday", JS_NewInt32(ctx, tm.tm_yday));
    return obj;
}

/* sys.time.format(unix_ts, fmt) -> string
 *   fmt 直接传给 strftime，例如 "%Y-%m-%d %H:%M" / "%H:%M" */
static JSValue js_sys_time_format(JSContext *ctx, JSValue *this_val,
                                  int argc, JSValue *argv)
{
    (void)this_val;
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "sys.time.format(ts, fmt) args missing");
    }
    int ts32 = 0;
    if (JS_ToInt32(ctx, &ts32, argv[0])) return JS_EXCEPTION;

    JSCStringBuf fbuf;
    size_t flen = 0;
    const char *fmt = JS_ToCStringLen(ctx, &flen, argv[1], &fbuf);
    if (!fmt) return JS_EXCEPTION;

    time_t t = (time_t)(int32_t)ts32;
    struct tm tm;
    localtime_r(&t, &tm);

    char out[64];
    size_t n = strftime(out, sizeof(out), fmt, &tm);
    if (n == 0) out[0] = '\0';
    return JS_NewString(ctx, out);
}

/* sys.ui.modal({ id, title?, body?, action0?, action1? })
 *   id : 数字 modal_id（>0），由 JS 侧分配。按钮按下时通过 EV_MODAL 事件
 *        回传，dx = 按钮索引（0/1），点遮罩/下滑 dx = -1。
 *   action0/action1 : 字符串；缺省/空表示无该按钮。 */
static JSValue js_sys_ui_modal(JSContext *ctx, JSValue *this_val,
                               int argc, JSValue *argv)
{
    (void)this_val;
    /* esp-mquickjs 没有 JS_IsObject；用 JS_IsPtr 排除原始类型，对象在 mqjs 中是 ptr。
     * String/Function 也是 ptr 但取属性会得到 undefined，下面有兜底。 */
    if (argc < 1 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0]) || !JS_IsPtr(argv[0])) {
        return JS_ThrowTypeError(ctx, "sys.ui.modal(opts): need object");
    }
    JSValue opts = argv[0];

    /* 取 id（必填） */
    JSValue v_id = JS_GetPropertyStr(ctx, opts, "id");
    int id_int = 0;
    if (JS_ToInt32(ctx, &id_int, v_id)) {
        return JS_ThrowTypeError(ctx, "sys.ui.modal: opts.id must be int");
    }
    if (id_int <= 0) {
        return JS_ThrowTypeError(ctx, "sys.ui.modal: opts.id must be > 0");
    }

    /* 取 title/body/action0/action1（可选 string） */
    const char *title = NULL, *body = NULL, *a0 = NULL, *a1 = NULL;
    size_t tlen = 0, blen = 0, a0len = 0, a1len = 0;
    JSCStringBuf tbuf, bbuf, a0buf, a1buf;

    JSValue v_t = JS_GetPropertyStr(ctx, opts, "title");
    if (JS_IsString(ctx, v_t)) title = JS_ToCStringLen(ctx, &tlen, v_t, &tbuf);

    JSValue v_b = JS_GetPropertyStr(ctx, opts, "body");
    if (JS_IsString(ctx, v_b)) body = JS_ToCStringLen(ctx, &blen, v_b, &bbuf);

    JSValue v_a0 = JS_GetPropertyStr(ctx, opts, "action0");
    if (JS_IsString(ctx, v_a0)) a0 = JS_ToCStringLen(ctx, &a0len, v_a0, &a0buf);

    JSValue v_a1 = JS_GetPropertyStr(ctx, opts, "action1");
    if (JS_IsString(ctx, v_a1)) a1 = JS_ToCStringLen(ctx, &a1len, v_a1, &a1buf);

    bool ok = dynamic_app_ui_enqueue_show_modal((uint32_t)id_int,
                                                title, tlen,
                                                body,  blen,
                                                a0, a0len,
                                                a1, a1len);
    return JS_NewBool(ok ? 1 : 0);
}

/* sys.ui.toast(text, dur_ms?)  dur_ms 默认 1500 */
static JSValue js_sys_ui_toast(JSContext *ctx, JSValue *this_val,
                               int argc, JSValue *argv)
{
    (void)this_val;
    if (argc < 1 || !JS_IsString(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "sys.ui.toast(text, dur?): need string");
    }
    JSCStringBuf tbuf;
    size_t tlen = 0;
    const char *t = JS_ToCStringLen(ctx, &tlen, argv[0], &tbuf);
    if (!t) return JS_EXCEPTION;

    int dur = 0;
    if (argc >= 2 && JS_ToInt32(ctx, &dur, argv[1])) return JS_EXCEPTION;
    if (dur < 0) dur = 0;
    if (dur > 10000) dur = 10000;

    bool ok = dynamic_app_ui_enqueue_toast(t, tlen, (uint16_t)dur);
    return JS_NewBool(ok ? 1 : 0);
}

/* sys.ui.fadeIn(id, delay_ms?) */
static JSValue js_sys_ui_fade_in(JSContext *ctx, JSValue *this_val,
                                 int argc, JSValue *argv)
{
    (void)this_val;
    if (argc < 1 || !JS_IsString(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "sys.ui.fadeIn(id, delay?): need string id");
    }
    JSCStringBuf ibuf;
    size_t ilen = 0;
    const char *id = JS_ToCStringLen(ctx, &ilen, argv[0], &ibuf);
    if (!id) return JS_EXCEPTION;

    int delay = 0;
    if (argc >= 2 && JS_ToInt32(ctx, &delay, argv[1])) return JS_EXCEPTION;
    if (delay < 0) delay = 0;
    if (delay > 5000) delay = 5000;

    bool ok = dynamic_app_ui_enqueue_fade_in(id, ilen, (uint16_t)delay);
    return JS_NewBool(ok ? 1 : 0);
}

/* ============================================================================
 * §5. JS Native：setInterval / clearInterval
 * ========================================================================= */

static JSValue js_set_interval(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)this_val;
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "setInterval(fn, ms) args missing");
    }
    if (!JS_IsFunction(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "setInterval: not a function");
    }

    int delay_ms = 0;
    if (JS_ToInt32(ctx, &delay_ms, argv[1])) return JS_EXCEPTION;
    if (delay_ms < 1) delay_ms = 1;

    for (int i = 0; i < MAX_INTERVALS; i++) {
        js_interval_t *t = &s_rt.intervals[i];
        if (!t->allocated) {
            JSValue *pfunc = JS_AddGCRef(ctx, &t->func);
            *pfunc = argv[0];
            t->interval_ms = delay_ms;
            t->next_ms = dynamic_app_now_ms() + delay_ms;
            t->allocated = true;
            return JS_NewInt32(ctx, i);
        }
    }
    return JS_ThrowInternalError(ctx, "too many intervals");
}

static JSValue js_clear_interval(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;

    int id = -1;
    if (JS_ToInt32(ctx, &id, argv[0])) return JS_EXCEPTION;
    if (id < 0 || id >= MAX_INTERVALS) return JS_UNDEFINED;

    js_interval_t *t = &s_rt.intervals[id];
    if (t->allocated) {
        JS_DeleteGCRef(ctx, &t->func);
        t->allocated = false;
    }
    return JS_UNDEFINED;
}

/* ============================================================================
 * §5b. JS Native：sys.app.* （持久化）
 *
 *   设计：
 *     - JS 侧自己 JSON.stringify/parse；C 侧只是个 string ↔ NVS blob 的搬运工
 *     - 同步调用：返回值生效就是真落盘/真读出。线程上下文是 Script Task，
 *       NVS API 自带锁，无需队列
 *     - namespace 固定 "dynapp"，key = 当前 app 名（"alarm" / "calc" / ...）
 *       不同 app 自动隔离；同一 app 多页面共享一个 blob
 *     - 4KB 上限：NVS blob 上限 ~4000B，闹钟 state JSON ~几百字节足够
 * ========================================================================= */

#define DYNAPP_NS         "dynapp"
#define DYNAPP_STATE_MAX  4000   /* 留出 NVS 元信息空间 */

/* 取当前 app 的 NVS key。空字符串 → 拒绝读写（避免误存到错误位置） */
static const char *current_app_key(void)
{
    const char *n = dynamic_app_registry_current();
    return (n && n[0]) ? n : NULL;
}

/* sys.app.saveState(jsonString) -> bool
 *   传入 JSON 字符串，写入 NVS。成功返回 true。
 */
static JSValue js_sys_app_save_state(JSContext *ctx, JSValue *this_val,
                                     int argc, JSValue *argv)
{
    (void)this_val;
    if (argc < 1 || !JS_IsString(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "sys.app.saveState(jsonString): need string");
    }

    JSCStringBuf buf;
    size_t len = 0;
    const char *s = JS_ToCStringLen(ctx, &len, argv[0], &buf);
    if (!s) return JS_EXCEPTION;

    const char *key = current_app_key();
    if (!key) {
        ESP_LOGW(TAG, "saveState: no current app, dropped");
        return JS_NewBool(0);
    }
    if (len > DYNAPP_STATE_MAX) {
        ESP_LOGW(TAG, "saveState: payload %u B exceeds %u, dropped",
                 (unsigned)len, (unsigned)DYNAPP_STATE_MAX);
        return JS_NewBool(0);
    }

    esp_err_t err = persist_set_blob(DYNAPP_NS, key, s, len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "saveState NVS err=0x%x (key=%s)", err, key);
        return JS_NewBool(0);
    }
    return JS_NewBool(1);
}

/* sys.app.loadState() -> string | null
 *   读出 JSON 字符串。无数据返回 null（首次启动）。
 */
static JSValue js_sys_app_load_state(JSContext *ctx, JSValue *this_val,
                                     int argc, JSValue *argv)
{
    (void)this_val; (void)argc; (void)argv;

    const char *key = current_app_key();
    if (!key) return JS_NULL;

    /* 第一次调 nvs_get_blob 用 NULL 探测长度 */
    size_t len = 0;
    esp_err_t err = persist_get_blob(DYNAPP_NS, key, NULL, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND || len == 0) {
        return JS_NULL;
    }
    if (err != ESP_OK && err != ESP_ERR_NVS_INVALID_LENGTH) {
        ESP_LOGW(TAG, "loadState probe err=0x%x (key=%s)", err, key);
        return JS_NULL;
    }
    if (len > DYNAPP_STATE_MAX) {
        ESP_LOGW(TAG, "loadState: blob %u B too large, dropped", (unsigned)len);
        return JS_NULL;
    }

    char *tmp = (char *)malloc(len + 1);
    if (!tmp) {
        ESP_LOGW(TAG, "loadState malloc %u failed", (unsigned)len);
        return JS_NULL;
    }
    err = persist_get_blob(DYNAPP_NS, key, tmp, &len);
    if (err != ESP_OK) {
        free(tmp);
        ESP_LOGW(TAG, "loadState read err=0x%x (key=%s)", err, key);
        return JS_NULL;
    }
    tmp[len] = '\0';

    JSValue v = JS_NewStringLen(ctx, tmp, len);
    free(tmp);
    return v;
}

/* sys.app.eraseState() -> bool
 *   抹掉当前 app 的持久化 blob（不影响其它 app）。
 */
static JSValue js_sys_app_erase_state(JSContext *ctx, JSValue *this_val,
                                      int argc, JSValue *argv)
{
    (void)this_val; (void)argc; (void)argv;
    const char *key = current_app_key();
    if (!key) return JS_NewBool(0);
    /* 用 set_blob 写 0 长度等价"清空"——更精确、不影响别人 */
    esp_err_t err = persist_set_blob(DYNAPP_NS, key, "", 0);
    return JS_NewBool(err == ESP_OK ? 1 : 0);
}

/* ============================================================================
 * §5c. JS Native：sys.ble.* （BLE 透传）
 *
 *   设计：
 *     - 完全透明：JS 传字符串 → C 当字节流通过 dynapp_bridge_service 发出去
 *     - 接收：JS 注册一个 onRecv(payloadStr) 回调，每 tick C 侧 drain inbox 后逐条调
 *     - 不暴露 GATT/UUID 概念，对 JS 来说就是 send/recv 一对管道
 *     - 单一 onRecv：覆盖式注册（再调一次会替换，传 null 可清空）
 *   线程：所有 JS 调到这里都在 script_task；安全调 dynapp_bridge_send（NimBLE 自带锁）
 * ========================================================================= */

/* sys.ble.send(payloadStr) -> bool
 *   把字符串当 utf8 字节发给 PC。返回 true=NimBLE 成功 enqueue notify。
 */
static JSValue js_sys_ble_send(JSContext *ctx, JSValue *this_val,
                               int argc, JSValue *argv)
{
    (void)this_val;
    if (argc < 1 || !JS_IsString(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "sys.ble.send(payloadString): need string");
    }
    JSCStringBuf buf;
    size_t len = 0;
    const char *s = JS_ToCStringLen(ctx, &len, argv[0], &buf);
    if (!s) return JS_EXCEPTION;
    if (len > DYNAPP_BRIDGE_MAX_PAYLOAD) {
        ESP_LOGW(TAG, "ble.send: payload %u B exceeds %d, dropped",
                 (unsigned)len, DYNAPP_BRIDGE_MAX_PAYLOAD);
        return JS_NewBool(0);
    }
    bool ok = dynapp_bridge_send((const uint8_t *)s, len);
    return JS_NewBool(ok ? 1 : 0);
}

/* sys.ble.onRecv(fn|null) -> undefined
 *   注册 PC 推数据回调。fn 签名：function(payloadStr) {...}
 *   传 null/undefined 取消注册。再次注册会替换上一次。
 */
static JSValue js_sys_ble_on_recv(JSContext *ctx, JSValue *this_val,
                                  int argc, JSValue *argv)
{
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;

    /* 先释放旧 ref（如果有） */
    if (s_rt.ble_recv_cb_allocated) {
        JS_DeleteGCRef(ctx, &s_rt.ble_recv_cb);
        s_rt.ble_recv_cb_allocated = false;
    }

    if (JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) {
        return JS_UNDEFINED;   /* 取消注册 */
    }
    if (!JS_IsFunction(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "sys.ble.onRecv(fn|null): not a function");
    }

    JSValue *p = JS_AddGCRef(ctx, &s_rt.ble_recv_cb);
    if (!p) {
        ESP_LOGE(TAG, "ble.onRecv: AddGCRef failed");
        return JS_ThrowInternalError(ctx, "out of GC slots");
    }
    *p = argv[0];
    s_rt.ble_recv_cb_allocated = true;
    return JS_UNDEFINED;
}

/* sys.ble.isConnected() -> bool */
static JSValue js_sys_ble_is_connected(JSContext *ctx, JSValue *this_val,
                                       int argc, JSValue *argv)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return JS_NewBool(dynapp_bridge_is_connected() ? 1 : 0);
}

/* ============================================================================
 * §5d. JS Native：sys.fs.* （app 沙箱文件 IO）
 *
 *   设计：
 *     - 路径强制相对当前 app：JS 写 "save.json"，C 拼成
 *       /littlefs/apps/<current_app>/data/save.json
 *     - read/exists/list 同步直接走 FS（小文件 < 100ms 可接受）
 *     - write/remove 走 dynapp_upload_manager 队列异步落盘
 *       JS 调入立即返回 true（"入队成功"），失败只在 manager 内部打 log
 *     - 大小上限沿用 64KB；单 path 长度 ≤ 31
 *   线程：所有 JS 调用都在 script_task；FS API 自带锁。
 * ========================================================================= */

/* 拒绝在没有 current_app 时使用 sys.fs（避免误写到错误位置） */
static const char *current_app_id_or_null(void)
{
    const char *id = dynamic_app_registry_current();
    return (id && id[0]) ? id : NULL;
}

/* sys.fs.read(path) -> string | null */
static JSValue js_sys_fs_read(JSContext *ctx, JSValue *this_val,
                              int argc, JSValue *argv)
{
    (void)this_val;
    if (argc < 1 || !JS_IsString(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "sys.fs.read(path): need string");
    }
    const char *app = current_app_id_or_null();
    if (!app) return JS_NULL;

    JSCStringBuf pbuf;
    size_t plen = 0;
    const char *p = JS_ToCStringLen(ctx, &plen, argv[0], &pbuf);
    if (!p) return JS_EXCEPTION;

    uint8_t *data = NULL;
    size_t   dlen = 0;
    esp_err_t e = dynapp_user_data_read(app, p, &data, &dlen);
    if (e != ESP_OK) {
        return JS_NULL;
    }
    JSValue v = JS_NewStringLen(ctx, (const char *)data, dlen);
    dynapp_script_store_release(data);
    return v;
}

/* sys.fs.write(path, content) -> bool   入队即 true，失败只 log
 *   ≤ 196B 走单帧老路径；> 196B 走 large 路径（worker 内部拷贝 PSRAM）。
 *   上限 DYNAPP_USER_DATA_MAX_BYTES (256KB)。 */
static JSValue js_sys_fs_write(JSContext *ctx, JSValue *this_val,
                               int argc, JSValue *argv)
{
    (void)this_val;
    if (argc < 2 || !JS_IsString(ctx, argv[0]) || !JS_IsString(ctx, argv[1])) {
        return JS_ThrowTypeError(ctx, "sys.fs.write(path, content): need (string, string)");
    }
    const char *app = current_app_id_or_null();
    if (!app) return JS_NewBool(0);

    JSCStringBuf pbuf, cbuf;
    size_t plen = 0, clen = 0;
    const char *p = JS_ToCStringLen(ctx, &plen, argv[0], &pbuf);
    if (!p) return JS_EXCEPTION;
    const char *c = JS_ToCStringLen(ctx, &clen, argv[1], &cbuf);
    if (!c) return JS_EXCEPTION;

    bool ok;
    if (clen <= 196) {
        ok = dynapp_fs_worker_submit_user_write(app, p, (const uint8_t *)c, clen);
    } else {
        ok = dynapp_fs_worker_submit_user_write_large(app, p,
                                                       (const uint8_t *)c, clen);
    }
    if (!ok) ESP_LOGW(TAG, "fs.write: queue refused or oversize (%u B)", (unsigned)clen);
    return JS_NewBool(ok ? 1 : 0);
}

/* sys.fs.exists(path) -> bool */
static JSValue js_sys_fs_exists(JSContext *ctx, JSValue *this_val,
                                int argc, JSValue *argv)
{
    (void)this_val;
    if (argc < 1 || !JS_IsString(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "sys.fs.exists(path): need string");
    }
    const char *app = current_app_id_or_null();
    if (!app) return JS_NewBool(0);

    JSCStringBuf pbuf;
    size_t plen = 0;
    const char *p = JS_ToCStringLen(ctx, &plen, argv[0], &pbuf);
    if (!p) return JS_EXCEPTION;
    return JS_NewBool(dynapp_user_data_exists(app, p) ? 1 : 0);
}

/* sys.fs.remove(path) -> bool   入队即 true */
static JSValue js_sys_fs_remove(JSContext *ctx, JSValue *this_val,
                                int argc, JSValue *argv)
{
    (void)this_val;
    if (argc < 1 || !JS_IsString(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "sys.fs.remove(path): need string");
    }
    const char *app = current_app_id_or_null();
    if (!app) return JS_NewBool(0);

    JSCStringBuf pbuf;
    size_t plen = 0;
    const char *p = JS_ToCStringLen(ctx, &plen, argv[0], &pbuf);
    if (!p) return JS_EXCEPTION;
    bool ok = dynapp_fs_worker_submit_user_remove(app, p);
    return JS_NewBool(ok ? 1 : 0);
}

/* sys.fs.list() -> [string, ...]   列出 data/ 下所有文件名 */
static JSValue js_sys_fs_list(JSContext *ctx, JSValue *this_val,
                              int argc, JSValue *argv)
{
    (void)this_val; (void)argc; (void)argv;
    const char *app = current_app_id_or_null();
    int n = 0;
    char names[16][DYNAPP_USER_DATA_MAX_PATH + 1];
    if (app) n = dynapp_user_data_list(app, names, 16);

    JSValue arr = JS_NewArray(ctx, n);
    if (JS_IsException(arr)) return arr;
    for (int i = 0; i < n; i++) {
        JSValue s = JS_NewString(ctx, names[i]);
        (void)JS_SetPropertyUint32(ctx, arr, (uint32_t)i, s);
    }
    return arr;
}

/* ============================================================================
 * §5e. JS Native：sys.canvas.* （像素级绘图 + 文件互转）
 *
 *   - create(id, parent, w?, h?)：默认 240×320 RGB565 in PSRAM
 *   - fill / setPixel / line：纯 buffer 操作 + lv_obj_invalidate
 *   - saveTo(id, relpath)：把 buffer dump 到 data/<rel>，走 fs_worker
 *     大块路径（worker 内拷贝 PSRAM 一份再串行落盘，调用即返）
 *   - loadFrom(id, relpath)：同步 read_file 到 buffer + invalidate
 *
 * 参数都从 JS 拿 int / string，错误返回 false（透明降级）。
 * ========================================================================= */

static JSValue js_sys_canvas_create(JSContext *ctx, JSValue *this_val,
                                    int argc, JSValue *argv)
{
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx,
        "sys.canvas.create(id, parent?, w?, h?): id missing");

    JSCStringBuf id_buf;
    size_t id_len = 0;
    const char *id = JS_ToCStringLen(ctx, &id_len, argv[0], &id_buf);
    if (!id) return JS_EXCEPTION;

    parent_str_t ph;
    const char *pid = NULL;
    size_t plen = 0;
    if (argc >= 2 && !extract_parent_id(ctx, argv[1], &pid, &plen, &ph)) {
        return JS_EXCEPTION;
    }

    int w = 0, h = 0;
    if (argc >= 3 && JS_ToInt32(ctx, &w, argv[2])) return JS_EXCEPTION;
    if (argc >= 4 && JS_ToInt32(ctx, &h, argv[3])) return JS_EXCEPTION;
    if (w < 0) w = 0;
    if (h < 0) h = 0;

    bool ok = dynamic_app_ui_enqueue_create_canvas(id, id_len, pid, plen,
                                                     (uint16_t)w, (uint16_t)h);
    return JS_NewBool(ok ? 1 : 0);
}

static JSValue js_sys_canvas_fill(JSContext *ctx, JSValue *this_val,
                                   int argc, JSValue *argv)
{
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx,
        "sys.canvas.fill(id, color): args missing");
    JSCStringBuf id_buf;
    size_t id_len = 0;
    const char *id = JS_ToCStringLen(ctx, &id_len, argv[0], &id_buf);
    if (!id) return JS_EXCEPTION;
    int color = 0;
    if (JS_ToInt32(ctx, &color, argv[1])) return JS_EXCEPTION;
    bool ok = dynamic_app_ui_enqueue_canvas_fill(id, id_len, (uint32_t)color);
    return JS_NewBool(ok ? 1 : 0);
}

static JSValue js_sys_canvas_pixel(JSContext *ctx, JSValue *this_val,
                                    int argc, JSValue *argv)
{
    (void)this_val;
    if (argc < 4) return JS_ThrowTypeError(ctx,
        "sys.canvas.setPixel(id, x, y, color): args missing");
    JSCStringBuf id_buf;
    size_t id_len = 0;
    const char *id = JS_ToCStringLen(ctx, &id_len, argv[0], &id_buf);
    if (!id) return JS_EXCEPTION;
    int x = 0, y = 0, color = 0;
    if (JS_ToInt32(ctx, &x,     argv[1])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &y,     argv[2])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &color, argv[3])) return JS_EXCEPTION;
    bool ok = dynamic_app_ui_enqueue_canvas_pixel(id, id_len,
                                                    (int16_t)x, (int16_t)y,
                                                    (uint32_t)color);
    return JS_NewBool(ok ? 1 : 0);
}

static JSValue js_sys_canvas_line(JSContext *ctx, JSValue *this_val,
                                   int argc, JSValue *argv)
{
    (void)this_val;
    if (argc < 6) return JS_ThrowTypeError(ctx,
        "sys.canvas.line(id, x0, y0, x1, y1, color, thickness?): args missing");
    JSCStringBuf id_buf;
    size_t id_len = 0;
    const char *id = JS_ToCStringLen(ctx, &id_len, argv[0], &id_buf);
    if (!id) return JS_EXCEPTION;
    int x0=0, y0=0, x1=0, y1=0, color=0, thickness=1;
    if (JS_ToInt32(ctx, &x0,    argv[1])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &y0,    argv[2])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &x1,    argv[3])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &y1,    argv[4])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &color, argv[5])) return JS_EXCEPTION;
    if (argc >= 7 && JS_ToInt32(ctx, &thickness, argv[6])) return JS_EXCEPTION;
    bool ok = dynamic_app_ui_enqueue_canvas_line(id, id_len,
                                                   (int16_t)x0, (int16_t)y0,
                                                   (int16_t)x1, (int16_t)y1,
                                                   (uint32_t)color,
                                                   (uint8_t)thickness);
    return JS_NewBool(ok ? 1 : 0);
}

static JSValue js_sys_canvas_save_to(JSContext *ctx, JSValue *this_val,
                                       int argc, JSValue *argv)
{
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx,
        "sys.canvas.saveTo(id, relpath): args missing");
    JSCStringBuf id_buf, rp_buf;
    size_t id_len = 0, rp_len = 0;
    const char *id = JS_ToCStringLen(ctx, &id_len, argv[0], &id_buf);
    if (!id) return JS_EXCEPTION;
    const char *rp = JS_ToCStringLen(ctx, &rp_len, argv[1], &rp_buf);
    if (!rp) return JS_EXCEPTION;
    bool ok = dynamic_app_ui_enqueue_canvas_save(id, id_len, rp, rp_len);
    return JS_NewBool(ok ? 1 : 0);
}

static JSValue js_sys_canvas_load_from(JSContext *ctx, JSValue *this_val,
                                         int argc, JSValue *argv)
{
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx,
        "sys.canvas.loadFrom(id, relpath): args missing");
    JSCStringBuf id_buf, rp_buf;
    size_t id_len = 0, rp_len = 0;
    const char *id = JS_ToCStringLen(ctx, &id_len, argv[0], &id_buf);
    if (!id) return JS_EXCEPTION;
    const char *rp = JS_ToCStringLen(ctx, &rp_len, argv[1], &rp_buf);
    if (!rp) return JS_EXCEPTION;
    bool ok = dynamic_app_ui_enqueue_canvas_load(id, id_len, rp, rp_len);
    return JS_NewBool(ok ? 1 : 0);
}

/* ============================================================================
 * §6. tick 循环服务
 *
 *   被 dynamic_app.c 的 script_task 主循环周期性调用。
 *
 *   - run_intervals_once：跑所有到期的 setInterval
 *   - drain_ui_events_once：消化 UI→Script 事件队列里的点击事件
 *   - next_interval_deadline_ms：算下一个 deadline 决定 sleep 多久
 * ========================================================================= */

bool dynamic_app_run_intervals_once(JSContext *ctx, int64_t cur_ms)
{
    for (int i = 0; i < MAX_INTERVALS; i++) {
        js_interval_t *t = &s_rt.intervals[i];
        if (!t->allocated) continue;
        if (t->next_ms > cur_ms) continue;

        if (JS_StackCheck(ctx, 2)) {
            dynamic_app_dump_exception(ctx);
            return false;
        }

        JS_PushArg(ctx, t->func.val);
        JS_PushArg(ctx, JS_NULL);
        JSValue ret = JS_Call(ctx, 0);
        if (JS_IsException(ret)) {
            dynamic_app_dump_exception(ctx);
            return false;
        }

        int64_t next = t->next_ms + t->interval_ms;
        if (next <= cur_ms) {
            next = cur_ms + t->interval_ms;
        }
        t->next_ms = next;
    }
    return true;
}

int64_t dynamic_app_next_interval_deadline_ms(int64_t cur_ms)
{
    int64_t next = cur_ms + 1000;
    bool found = false;
    for (int i = 0; i < MAX_INTERVALS; i++) {
        js_interval_t *t = &s_rt.intervals[i];
        if (!t->allocated) continue;
        if (!found || t->next_ms < next) {
            next = t->next_ms;
            found = true;
        }
    }
    if (!found) return cur_ms + 200;
    return next;
}

void dynamic_app_drain_ui_events_once(JSContext *ctx)
{
    /* 每 tick 最多消 8 个事件，避免大量回调阻塞主循环。
     *
     * root delegation 路径：
     *   on_lv_root_event 把 { type, dx, dy, node_id } 入队，
     *   这里调用 sys.__setDispatcher 注册的 JS 函数：
     *     dispatcher(node_id, type, dx, dy)
     *
     *   esp-mquickjs 栈约定：push 顺序是 argN ... arg1, fn, this
     *   （从栈底到栈顶），栈顶是 this。
     */
    int budget = 8;
    dynamic_app_ui_event_t ev;
    while (budget-- > 0 && dynamic_app_ui_pop_event(&ev)) {
        if (ev.node_id[0] == '\0') continue;
        if (!s_rt.dispatcher_allocated) continue;

        if (JS_StackCheck(ctx, 6)) {
            dynamic_app_dump_exception(ctx);
            return;
        }
        JSValue arg_id   = JS_NewString(ctx, ev.node_id);
        JSValue arg_type = JS_NewInt32(ctx, (int)ev.type);
        JSValue arg_dx   = JS_NewInt32(ctx, (int)ev.dx);
        JSValue arg_dy   = JS_NewInt32(ctx, (int)ev.dy);

        JS_PushArg(ctx, arg_dy);                 /* arg4 最深 */
        JS_PushArg(ctx, arg_dx);                 /* arg3 */
        JS_PushArg(ctx, arg_type);               /* arg2 */
        JS_PushArg(ctx, arg_id);                 /* arg1 */
        JS_PushArg(ctx, s_rt.dispatcher.val);    /* fn */
        JS_PushArg(ctx, JS_NULL);                /* this 栈顶 */
        JSValue ret = JS_Call(ctx, 4);
        if (JS_IsException(ret)) {
            dynamic_app_dump_exception(ctx);
        }
    }
}

/* 消化 BLE inbox 队列：每 tick 最多 4 条。
 *   消息内容当 utf8 字符串传给 onRecv（脚本一般 JSON.parse 处理）。
 *   若 onRecv 抛 JS 异常：打印 + 继续下一条（不能让一个坏包让 app 退出）。 */
void dynamic_app_drain_ble_inbox_once(JSContext *ctx)
{
    if (!s_rt.ble_recv_cb_allocated) {
        /* JS 没注册回调时，直接清空 inbox 防积压 */
        dynapp_bridge_msg_t drop;
        while (dynapp_bridge_pop_inbox(&drop)) { /* drop */ }
        return;
    }

    int budget = 4;
    dynapp_bridge_msg_t msg;
    while (budget-- > 0 && dynapp_bridge_pop_inbox(&msg)) {
        if (JS_StackCheck(ctx, 3)) {
            dynamic_app_dump_exception(ctx);
            return;
        }
        JSValue arg = JS_NewStringLen(ctx, (const char *)msg.data, msg.len);
        JS_PushArg(ctx, arg);                       /* arg1 */
        JS_PushArg(ctx, s_rt.ble_recv_cb.val);      /* fn */
        JS_PushArg(ctx, JS_NULL);                   /* this */
        JSValue ret = JS_Call(ctx, 1);
        if (JS_IsException(ret)) {
            dynamic_app_dump_exception(ctx);
            /* 不 return；坏包不应拖死 app */
        }
    }
}

void dynamic_app_ble_reset(JSContext *ctx)
{
    if (s_rt.ble_recv_cb_allocated) {
        JS_DeleteGCRef(ctx, &s_rt.ble_recv_cb);
        s_rt.ble_recv_cb_allocated = false;
    }
    dynapp_bridge_clear_inbox();
}

/* ============================================================================
 * §7. cfunc 表注册
 *
 *   被 runtime.c 的 setup 调用：
 *     1. 给每个自定义 native 分配一个表索引
 *     2. 填 JSCFunctionDef（指向 C 函数 + 参数个数）
 *
 *   如果以后要加新 native，只在这个函数里改：
 *     - 分配 idx
 *     - 填 JSCFunctionDef
 *     - 在 §8 的 bind 里把 idx 转成 JSValue 并挂到 sys.* 上
 * ========================================================================= */

#define DEF_CFN(idx_field, fn, argn) \
    s_rt.cfunc_table[s_rt.idx_field] = (JSCFunctionDef){ \
        .func.generic = (fn), \
        .name = JS_UNDEFINED, \
        .def_type = JS_CFUNC_generic, \
        .arg_count = (argn), \
        .magic = 0, \
    }

void dynamic_app_natives_register(dynamic_app_runtime_t *rt, size_t base_count)
{
    /* 索引分配 */
    rt->func_idx_sys_log                     = (int)base_count + 0;
    rt->func_idx_sys_ui_set_text             = (int)base_count + 1;
    rt->func_idx_sys_ui_create_label         = (int)base_count + 2;
    rt->func_idx_sys_ui_create_panel         = (int)base_count + 3;
    rt->func_idx_sys_ui_create_button        = (int)base_count + 4;
    rt->func_idx_sys_ui_set_style            = (int)base_count + 5;
    rt->func_idx_sys_ui_attach_root_listener = (int)base_count + 6;
    rt->func_idx_sys_ui_destroy              = (int)base_count + 7;
    rt->func_idx_sys_set_dispatcher          = (int)base_count + 8;
    rt->func_idx_sys_time_uptime_ms          = (int)base_count + 9;
    rt->func_idx_sys_time_uptime_str         = (int)base_count + 10;
    rt->func_idx_set_interval                = (int)base_count + 11;
    rt->func_idx_clear_interval              = (int)base_count + 12;
    rt->func_idx_sys_app_save_state          = (int)base_count + 13;
    rt->func_idx_sys_app_load_state          = (int)base_count + 14;
    rt->func_idx_sys_app_erase_state         = (int)base_count + 15;
    rt->func_idx_sys_ble_send                = (int)base_count + 16;
    rt->func_idx_sys_ble_on_recv             = (int)base_count + 17;
    rt->func_idx_sys_ble_is_connected        = (int)base_count + 18;
    rt->func_idx_sys_fs_read                 = (int)base_count + 19;
    rt->func_idx_sys_fs_write                = (int)base_count + 20;
    rt->func_idx_sys_fs_exists               = (int)base_count + 21;
    rt->func_idx_sys_fs_remove               = (int)base_count + 22;
    rt->func_idx_sys_fs_list                 = (int)base_count + 23;
    rt->func_idx_sys_ui_create_image         = (int)base_count + 24;
    rt->func_idx_sys_ui_set_image_src        = (int)base_count + 25;
    rt->func_idx_sys_time_now                = (int)base_count + 26;
    rt->func_idx_sys_time_parts              = (int)base_count + 27;
    rt->func_idx_sys_time_format             = (int)base_count + 28;
    rt->func_idx_sys_ui_modal                = (int)base_count + 29;
    rt->func_idx_sys_ui_toast                = (int)base_count + 30;
    rt->func_idx_sys_ui_fade_in              = (int)base_count + 31;
    rt->func_idx_sys_canvas_create           = (int)base_count + 32;
    rt->func_idx_sys_canvas_fill             = (int)base_count + 33;
    rt->func_idx_sys_canvas_pixel            = (int)base_count + 34;
    rt->func_idx_sys_canvas_line             = (int)base_count + 35;
    rt->func_idx_sys_canvas_save_to          = (int)base_count + 36;
    rt->func_idx_sys_canvas_load_from        = (int)base_count + 37;

    /* 函数定义填充 */
    DEF_CFN(func_idx_sys_log,                     js_sys_log,                     1);
    DEF_CFN(func_idx_sys_ui_set_text,             js_sys_ui_set_text,             2);
    DEF_CFN(func_idx_sys_ui_create_label,         js_sys_ui_create_label,         2);
    DEF_CFN(func_idx_sys_ui_create_panel,         js_sys_ui_create_panel,         2);
    DEF_CFN(func_idx_sys_ui_create_button,        js_sys_ui_create_button,        2);
    DEF_CFN(func_idx_sys_ui_set_style,            js_sys_ui_set_style,            6);
    DEF_CFN(func_idx_sys_ui_attach_root_listener, js_sys_ui_attach_root_listener, 1);
    DEF_CFN(func_idx_sys_ui_destroy,              js_sys_ui_destroy,              1);
    DEF_CFN(func_idx_sys_set_dispatcher,          js_sys_set_dispatcher,          1);
    DEF_CFN(func_idx_sys_time_uptime_ms,          js_sys_time_uptime_ms,          0);
    DEF_CFN(func_idx_sys_time_uptime_str,         js_sys_time_uptime_str,         0);
    DEF_CFN(func_idx_set_interval,                js_set_interval,                2);
    DEF_CFN(func_idx_clear_interval,              js_clear_interval,              1);
    DEF_CFN(func_idx_sys_app_save_state,          js_sys_app_save_state,          1);
    DEF_CFN(func_idx_sys_app_load_state,          js_sys_app_load_state,          0);
    DEF_CFN(func_idx_sys_app_erase_state,         js_sys_app_erase_state,         0);
    DEF_CFN(func_idx_sys_ble_send,                js_sys_ble_send,                1);
    DEF_CFN(func_idx_sys_ble_on_recv,             js_sys_ble_on_recv,             1);
    DEF_CFN(func_idx_sys_ble_is_connected,        js_sys_ble_is_connected,        0);
    DEF_CFN(func_idx_sys_fs_read,                 js_sys_fs_read,                 1);
    DEF_CFN(func_idx_sys_fs_write,                js_sys_fs_write,                2);
    DEF_CFN(func_idx_sys_fs_exists,               js_sys_fs_exists,               1);
    DEF_CFN(func_idx_sys_fs_remove,               js_sys_fs_remove,               1);
    DEF_CFN(func_idx_sys_fs_list,                 js_sys_fs_list,                 0);
    DEF_CFN(func_idx_sys_ui_create_image,         js_sys_ui_create_image,         3);
    DEF_CFN(func_idx_sys_ui_set_image_src,        js_sys_ui_set_image_src,        2);
    DEF_CFN(func_idx_sys_time_now,                js_sys_time_now,                0);
    DEF_CFN(func_idx_sys_time_parts,              js_sys_time_parts,              1);
    DEF_CFN(func_idx_sys_time_format,             js_sys_time_format,             2);
    DEF_CFN(func_idx_sys_ui_modal,                js_sys_ui_modal,                1);
    DEF_CFN(func_idx_sys_ui_toast,                js_sys_ui_toast,                2);
    DEF_CFN(func_idx_sys_ui_fade_in,              js_sys_ui_fade_in,              2);
    DEF_CFN(func_idx_sys_canvas_create,           js_sys_canvas_create,           4);
    DEF_CFN(func_idx_sys_canvas_fill,             js_sys_canvas_fill,             2);
    DEF_CFN(func_idx_sys_canvas_pixel,            js_sys_canvas_pixel,            4);
    DEF_CFN(func_idx_sys_canvas_line,             js_sys_canvas_line,             7);
    DEF_CFN(func_idx_sys_canvas_save_to,          js_sys_canvas_save_to,          2);
    DEF_CFN(func_idx_sys_canvas_load_from,        js_sys_canvas_load_from,        2);
}

#undef DEF_CFN

/* ============================================================================
 * §8. JS 全局对象绑定
 *
 *   把 native fn 挂到 sys.ui / sys.time 等子对象，
 *   再挂上枚举常量 sys.symbols / sys.style / sys.align / sys.font。
 *
 *   JS 侧 enum 与 C 侧 enum 必须对齐：
 *     - sys.style.* 与 dynamic_app_style_key_t 数值相同
 *     - sys.align.* 与 styles.c 的 k_align_map[] 索引相同
 *     - sys.font.*  与 styles.c 的 resolve_font() switch 相同
 * ========================================================================= */

#define BIND_FN(parent, name, idx) do { \
        JSValue _fn = JS_NewCFunctionParams(ctx, s_rt.idx, JS_UNDEFINED); \
        if (JS_IsException(_fn)) return ESP_FAIL; \
        (void)JS_SetPropertyStr(ctx, parent, name, _fn); \
    } while (0)

esp_err_t dynamic_app_natives_bind(JSContext *ctx)
{
    JSValue global  = JS_GetGlobalObject(ctx);
    JSValue sys     = JS_NewObject(ctx);
    JSValue ui      = JS_NewObject(ctx);
    JSValue time    = JS_NewObject(ctx);
    JSValue app     = JS_NewObject(ctx);
    JSValue ble     = JS_NewObject(ctx);
    JSValue fs      = JS_NewObject(ctx);
    JSValue canvas  = JS_NewObject(ctx);
    JSValue symbols = JS_NewObject(ctx);
    JSValue style   = JS_NewObject(ctx);
    JSValue align   = JS_NewObject(ctx);
    JSValue font    = JS_NewObject(ctx);

    /* sys.ui.* */
    BIND_FN(ui, "setText",            func_idx_sys_ui_set_text);
    BIND_FN(ui, "createLabel",        func_idx_sys_ui_create_label);
    BIND_FN(ui, "createPanel",        func_idx_sys_ui_create_panel);
    BIND_FN(ui, "createButton",       func_idx_sys_ui_create_button);
    BIND_FN(ui, "createImage",        func_idx_sys_ui_create_image);
    BIND_FN(ui, "setImageSrc",        func_idx_sys_ui_set_image_src);
    BIND_FN(ui, "setStyle",           func_idx_sys_ui_set_style);
    BIND_FN(ui, "attachRootListener", func_idx_sys_ui_attach_root_listener);
    BIND_FN(ui, "destroy",            func_idx_sys_ui_destroy);
    BIND_FN(ui, "modal",              func_idx_sys_ui_modal);
    BIND_FN(ui, "toast",              func_idx_sys_ui_toast);
    BIND_FN(ui, "fadeIn",             func_idx_sys_ui_fade_in);

    /* sys.time.* */
    BIND_FN(time, "uptimeMs",  func_idx_sys_time_uptime_ms);
    BIND_FN(time, "uptimeStr", func_idx_sys_time_uptime_str);
    BIND_FN(time, "now",       func_idx_sys_time_now);
    BIND_FN(time, "parts",     func_idx_sys_time_parts);
    BIND_FN(time, "format",    func_idx_sys_time_format);

    /* sys.app.* —— 持久化 */
    BIND_FN(app, "saveState",  func_idx_sys_app_save_state);
    BIND_FN(app, "loadState",  func_idx_sys_app_load_state);
    BIND_FN(app, "eraseState", func_idx_sys_app_erase_state);

    /* sys.ble.* —— BLE 透传管道 */
    BIND_FN(ble, "send",        func_idx_sys_ble_send);
    BIND_FN(ble, "onRecv",      func_idx_sys_ble_on_recv);
    BIND_FN(ble, "isConnected", func_idx_sys_ble_is_connected);

    /* sys.fs.* —— 当前 app 沙箱文件 IO（apps/<id>/data/ 下） */
    BIND_FN(fs, "read",   func_idx_sys_fs_read);
    BIND_FN(fs, "write",  func_idx_sys_fs_write);
    BIND_FN(fs, "exists", func_idx_sys_fs_exists);
    BIND_FN(fs, "remove", func_idx_sys_fs_remove);
    BIND_FN(fs, "list",   func_idx_sys_fs_list);

    /* sys.canvas.* —— 像素级绘图（buffer 在 PSRAM） */
    BIND_FN(canvas, "create",   func_idx_sys_canvas_create);
    BIND_FN(canvas, "fill",     func_idx_sys_canvas_fill);
    BIND_FN(canvas, "setPixel", func_idx_sys_canvas_pixel);
    BIND_FN(canvas, "line",     func_idx_sys_canvas_line);
    BIND_FN(canvas, "saveTo",   func_idx_sys_canvas_save_to);
    BIND_FN(canvas, "loadFrom", func_idx_sys_canvas_load_from);

    /* sys.symbols.* —— LVGL 内置 UTF-8 图标字面量 */
    (void)JS_SetPropertyStr(ctx, symbols, "BLUETOOTH", JS_NewString(ctx, LV_SYMBOL_BLUETOOTH));
    (void)JS_SetPropertyStr(ctx, symbols, "EYE_OPEN",  JS_NewString(ctx, LV_SYMBOL_EYE_OPEN));
    (void)JS_SetPropertyStr(ctx, symbols, "SETTINGS",  JS_NewString(ctx, LV_SYMBOL_SETTINGS));
    (void)JS_SetPropertyStr(ctx, symbols, "IMAGE",     JS_NewString(ctx, LV_SYMBOL_IMAGE));
    (void)JS_SetPropertyStr(ctx, symbols, "BELL",      JS_NewString(ctx, LV_SYMBOL_BELL));
    (void)JS_SetPropertyStr(ctx, symbols, "AUDIO",     JS_NewString(ctx, LV_SYMBOL_AUDIO));
    (void)JS_SetPropertyStr(ctx, symbols, "BARS",      JS_NewString(ctx, LV_SYMBOL_BARS));
    (void)JS_SetPropertyStr(ctx, symbols, "PLAY",      JS_NewString(ctx, LV_SYMBOL_PLAY));
    (void)JS_SetPropertyStr(ctx, symbols, "LIST",      JS_NewString(ctx, LV_SYMBOL_LIST));
    (void)JS_SetPropertyStr(ctx, symbols, "LEFT",      JS_NewString(ctx, LV_SYMBOL_LEFT));
    (void)JS_SetPropertyStr(ctx, symbols, "RIGHT",     JS_NewString(ctx, LV_SYMBOL_RIGHT));

    /* sys.style.* —— 必须与 dynamic_app_style_key_t 数值一致 */
    (void)JS_SetPropertyStr(ctx, style, "BG_COLOR",      JS_NewInt32(ctx, DYNAMIC_APP_STYLE_BG_COLOR));
    (void)JS_SetPropertyStr(ctx, style, "TEXT_COLOR",    JS_NewInt32(ctx, DYNAMIC_APP_STYLE_TEXT_COLOR));
    (void)JS_SetPropertyStr(ctx, style, "RADIUS",        JS_NewInt32(ctx, DYNAMIC_APP_STYLE_RADIUS));
    (void)JS_SetPropertyStr(ctx, style, "SIZE",          JS_NewInt32(ctx, DYNAMIC_APP_STYLE_SIZE));
    (void)JS_SetPropertyStr(ctx, style, "ALIGN",         JS_NewInt32(ctx, DYNAMIC_APP_STYLE_ALIGN));
    (void)JS_SetPropertyStr(ctx, style, "PAD",           JS_NewInt32(ctx, DYNAMIC_APP_STYLE_PAD));
    (void)JS_SetPropertyStr(ctx, style, "BORDER_BOTTOM", JS_NewInt32(ctx, DYNAMIC_APP_STYLE_BORDER_BOTTOM));
    (void)JS_SetPropertyStr(ctx, style, "FLEX",          JS_NewInt32(ctx, DYNAMIC_APP_STYLE_FLEX));
    (void)JS_SetPropertyStr(ctx, style, "FONT",          JS_NewInt32(ctx, DYNAMIC_APP_STYLE_FONT));
    (void)JS_SetPropertyStr(ctx, style, "SHADOW",        JS_NewInt32(ctx, DYNAMIC_APP_STYLE_SHADOW));
    (void)JS_SetPropertyStr(ctx, style, "GAP",           JS_NewInt32(ctx, DYNAMIC_APP_STYLE_GAP));
    (void)JS_SetPropertyStr(ctx, style, "SCROLLABLE",    JS_NewInt32(ctx, DYNAMIC_APP_STYLE_SCROLLABLE));
    (void)JS_SetPropertyStr(ctx, style, "OPA",           JS_NewInt32(ctx, DYNAMIC_APP_STYLE_OPA));
    (void)JS_SetPropertyStr(ctx, style, "BG_OPA",        JS_NewInt32(ctx, DYNAMIC_APP_STYLE_BG_OPA));
    (void)JS_SetPropertyStr(ctx, style, "FLEX_GROW",     JS_NewInt32(ctx, DYNAMIC_APP_STYLE_FLEX_GROW));
    (void)JS_SetPropertyStr(ctx, style, "TEXT_ALIGN",    JS_NewInt32(ctx, DYNAMIC_APP_STYLE_TEXT_ALIGN));
    (void)JS_SetPropertyStr(ctx, style, "LONG_MODE",     JS_NewInt32(ctx, DYNAMIC_APP_STYLE_LONG_MODE));
    (void)JS_SetPropertyStr(ctx, style, "ROTATION",      JS_NewInt32(ctx, DYNAMIC_APP_STYLE_ROTATION));
    (void)JS_SetPropertyStr(ctx, style, "FLEX_ALIGN",    JS_NewInt32(ctx, DYNAMIC_APP_STYLE_FLEX_ALIGN));
    (void)JS_SetPropertyStr(ctx, style, "BORDER",        JS_NewInt32(ctx, DYNAMIC_APP_STYLE_BORDER));
    (void)JS_SetPropertyStr(ctx, style, "PRESSED_BG",    JS_NewInt32(ctx, DYNAMIC_APP_STYLE_PRESSED_BG));
    (void)JS_SetPropertyStr(ctx, style, "HIDDEN",        JS_NewInt32(ctx, DYNAMIC_APP_STYLE_HIDDEN));

    /* sys.align.* —— 必须与 styles.c 的 k_align_map[] 索引一致 */
    (void)JS_SetPropertyStr(ctx, align, "TOP_LEFT",     JS_NewInt32(ctx, 0));
    (void)JS_SetPropertyStr(ctx, align, "TOP_MID",      JS_NewInt32(ctx, 1));
    (void)JS_SetPropertyStr(ctx, align, "TOP_RIGHT",    JS_NewInt32(ctx, 2));
    (void)JS_SetPropertyStr(ctx, align, "LEFT_MID",     JS_NewInt32(ctx, 3));
    (void)JS_SetPropertyStr(ctx, align, "CENTER",       JS_NewInt32(ctx, 4));
    (void)JS_SetPropertyStr(ctx, align, "RIGHT_MID",    JS_NewInt32(ctx, 5));
    (void)JS_SetPropertyStr(ctx, align, "BOTTOM_LEFT",  JS_NewInt32(ctx, 6));
    (void)JS_SetPropertyStr(ctx, align, "BOTTOM_MID",   JS_NewInt32(ctx, 7));
    (void)JS_SetPropertyStr(ctx, align, "BOTTOM_RIGHT", JS_NewInt32(ctx, 8));

    /* sys.font.* */
    (void)JS_SetPropertyStr(ctx, font, "TEXT",    JS_NewInt32(ctx, 0));
    (void)JS_SetPropertyStr(ctx, font, "TITLE",   JS_NewInt32(ctx, 1));
    (void)JS_SetPropertyStr(ctx, font, "HUGE",    JS_NewInt32(ctx, 2));
    (void)JS_SetPropertyStr(ctx, font, "ICON_24", JS_NewInt32(ctx, 3));
    (void)JS_SetPropertyStr(ctx, font, "ICON_36", JS_NewInt32(ctx, 4));
    (void)JS_SetPropertyStr(ctx, font, "NUM_M",   JS_NewInt32(ctx, 5));

    /* sys.size.* —— size 字段的特殊 sentinel（数值与 styles.c::resolve_size 一致）
     *   CONTENT (-32768) → LV_SIZE_CONTENT，按子内容自适应
     *   普通像素直接传正整数；百分比传负整数（-1..-100）。 */
    JSValue size_obj = JS_NewObject(ctx);
    (void)JS_SetPropertyStr(ctx, size_obj, "CONTENT", JS_NewInt32(ctx, -32768));

    /* 装配 sys */
    BIND_FN(sys, "log", func_idx_sys_log);
    (void)JS_SetPropertyStr(ctx, sys, "ui",      ui);
    (void)JS_SetPropertyStr(ctx, sys, "time",    time);
    (void)JS_SetPropertyStr(ctx, sys, "app",     app);
    (void)JS_SetPropertyStr(ctx, sys, "ble",     ble);
    (void)JS_SetPropertyStr(ctx, sys, "fs",      fs);
    (void)JS_SetPropertyStr(ctx, sys, "canvas",  canvas);
    (void)JS_SetPropertyStr(ctx, sys, "symbols", symbols);
    (void)JS_SetPropertyStr(ctx, sys, "style",   style);
    (void)JS_SetPropertyStr(ctx, sys, "align",   align);
    (void)JS_SetPropertyStr(ctx, sys, "font",    font);
    (void)JS_SetPropertyStr(ctx, sys, "size",    size_obj);

    /* sys.icons.* —— Material Symbols Rounded 字符（UTF-8）。
     * label 的 font 必须是 sys.font.ICON_24 / ICON_14 才能正确渲染。
     * 与 app/app_fonts.h 的 ICON_* 宏保持一致。 */
    JSValue icons = JS_NewObject(ctx);
    (void)JS_SetPropertyStr(ctx, icons, "BLUETOOTH",     JS_NewString(ctx, "\xEE\x86\xA7"));
    (void)JS_SetPropertyStr(ctx, icons, "BT_DISABLED",   JS_NewString(ctx, "\xEE\x86\xA8"));
    (void)JS_SetPropertyStr(ctx, icons, "SCHEDULE",      JS_NewString(ctx, "\xEE\xA2\xB5"));
    (void)JS_SetPropertyStr(ctx, icons, "WEATHER",       JS_NewString(ctx, "\xEF\x85\xB2"));
    (void)JS_SetPropertyStr(ctx, icons, "NOTIFICATIONS", JS_NewString(ctx, "\xEE\x9F\xB4"));
    (void)JS_SetPropertyStr(ctx, icons, "MUSIC",         JS_NewString(ctx, "\xEE\x90\x85"));
    (void)JS_SetPropertyStr(ctx, icons, "TUNE",          JS_NewString(ctx, "\xEE\x90\xA9"));
    (void)JS_SetPropertyStr(ctx, icons, "SETTINGS",      JS_NewString(ctx, "\xEE\xA2\xB8"));
    (void)JS_SetPropertyStr(ctx, icons, "BRIGHTNESS",    JS_NewString(ctx, "\xEE\x86\xA9"));
    (void)JS_SetPropertyStr(ctx, icons, "INFO",          JS_NewString(ctx, "\xEE\xA2\x8E"));
    (void)JS_SetPropertyStr(ctx, icons, "EDIT_CALENDAR", JS_NewString(ctx, "\xEE\x95\x96"));
    (void)JS_SetPropertyStr(ctx, icons, "APPS",          JS_NewString(ctx, "\xEE\x97\x83"));
    (void)JS_SetPropertyStr(ctx, icons, "CHEVRON_LEFT",  JS_NewString(ctx, "\xEE\x97\x8B"));
    (void)JS_SetPropertyStr(ctx, icons, "CHEVRON_RIGHT", JS_NewString(ctx, "\xEE\x97\x8C"));
    (void)JS_SetPropertyStr(ctx, icons, "DOT",           JS_NewString(ctx, "\xEE\xBD\x8A"));
    (void)JS_SetPropertyStr(ctx, icons, "DOT_SMALL",     JS_NewString(ctx, "\xEE\x81\xA1"));
    /* 业务 app 图标（与 dynamic_app_registry.c::k_icon_table 一致） */
    (void)JS_SetPropertyStr(ctx, icons, "ALARM",         JS_NewString(ctx, "\xEE\xA1\x95"));
    (void)JS_SetPropertyStr(ctx, icons, "TIMER",         JS_NewString(ctx, "\xEE\x90\xA5"));
    (void)JS_SetPropertyStr(ctx, icons, "STOPWATCH",     JS_NewString(ctx, "\xEE\x90\xA5"));
    (void)JS_SetPropertyStr(ctx, icons, "HABIT",         JS_NewString(ctx, "\xEE\xA1\xAC"));
    (void)JS_SetPropertyStr(ctx, icons, "NOTE",          JS_NewString(ctx, "\xEE\xA1\xB3"));
    (void)JS_SetPropertyStr(ctx, icons, "GAME",          JS_NewString(ctx, "\xEE\x8C\xB8"));
    (void)JS_SetPropertyStr(ctx, icons, "CALCULATOR",    JS_NewString(ctx, "\xEE\xA9\x9F"));
    (void)JS_SetPropertyStr(ctx, icons, "IMAGE",         JS_NewString(ctx, "\xEE\x8F\xB4"));
    (void)JS_SetPropertyStr(ctx, icons, "MEMORY",        JS_NewString(ctx, "\xEE\x8C\xA2"));
    (void)JS_SetPropertyStr(ctx, icons, "DASHBOARD",     JS_NewString(ctx, "\xEE\xA1\xB1"));
    (void)JS_SetPropertyStr(ctx, icons, "PUZZLE",        JS_NewString(ctx, "\xEE\xA1\xBB"));
    (void)JS_SetPropertyStr(ctx, icons, "TARGET",        JS_NewString(ctx, "\xEE\x86\xB3"));
    (void)JS_SetPropertyStr(ctx, icons, "PETS",          JS_NewString(ctx, "\xEE\xA4\x9D"));
    (void)JS_SetPropertyStr(ctx, icons, "AQUARIUM",      JS_NewString(ctx, "\xEE\xA4\x9D"));
    (void)JS_SetPropertyStr(ctx, icons, "ECHO",          JS_NewString(ctx, "\xEE\xBD\x89"));
    (void)JS_SetPropertyStr(ctx, sys, "icons", icons);

    /* sys.tokens.* —— 与 app/ui/ui_tokens.h 同步的设计 token。
     * 颜色用 0xRRGGBB 整数；间距/圆角/动画时长用 px/ms。 */
    JSValue tokens = JS_NewObject(ctx);
    /* 颜色（iOS 浅色） */
    (void)JS_SetPropertyStr(ctx, tokens, "C_BG",         JS_NewInt32(ctx, 0xF2F2F7));
    (void)JS_SetPropertyStr(ctx, tokens, "C_PANEL",      JS_NewInt32(ctx, 0xFFFFFF));
    (void)JS_SetPropertyStr(ctx, tokens, "C_PANEL_HI",   JS_NewInt32(ctx, 0xE5E5EA));
    (void)JS_SetPropertyStr(ctx, tokens, "C_BORDER",     JS_NewInt32(ctx, 0xC6C6C8));
    (void)JS_SetPropertyStr(ctx, tokens, "C_TEXT",       JS_NewInt32(ctx, 0x000000));
    (void)JS_SetPropertyStr(ctx, tokens, "C_TEXT_DIM",   JS_NewInt32(ctx, 0x3C3C43));
    (void)JS_SetPropertyStr(ctx, tokens, "C_TEXT_MUTED", JS_NewInt32(ctx, 0x6E6E73));
    (void)JS_SetPropertyStr(ctx, tokens, "C_ACCENT",     JS_NewInt32(ctx, 0x007AFF));
    (void)JS_SetPropertyStr(ctx, tokens, "C_ACCENT_2",   JS_NewInt32(ctx, 0xAF52DE));
    (void)JS_SetPropertyStr(ctx, tokens, "C_OK",         JS_NewInt32(ctx, 0x34C759));
    (void)JS_SetPropertyStr(ctx, tokens, "C_WARN",       JS_NewInt32(ctx, 0xFF9500));
    (void)JS_SetPropertyStr(ctx, tokens, "C_ERR",        JS_NewInt32(ctx, 0xFF3B30));
    (void)JS_SetPropertyStr(ctx, tokens, "C_INFO",       JS_NewInt32(ctx, 0x5AC8FA));
    /* 间距（8 倍数体系） */
    (void)JS_SetPropertyStr(ctx, tokens, "SP_XS",  JS_NewInt32(ctx, 4));
    (void)JS_SetPropertyStr(ctx, tokens, "SP_SM",  JS_NewInt32(ctx, 8));
    (void)JS_SetPropertyStr(ctx, tokens, "SP_MD",  JS_NewInt32(ctx, 12));
    (void)JS_SetPropertyStr(ctx, tokens, "SP_LG",  JS_NewInt32(ctx, 16));
    (void)JS_SetPropertyStr(ctx, tokens, "SP_XL",  JS_NewInt32(ctx, 24));
    (void)JS_SetPropertyStr(ctx, tokens, "SP_2XL", JS_NewInt32(ctx, 32));
    /* 圆角 */
    (void)JS_SetPropertyStr(ctx, tokens, "R_SM",   JS_NewInt32(ctx, 6));
    (void)JS_SetPropertyStr(ctx, tokens, "R_MD",   JS_NewInt32(ctx, 10));
    (void)JS_SetPropertyStr(ctx, tokens, "R_LG",   JS_NewInt32(ctx, 14));
    (void)JS_SetPropertyStr(ctx, tokens, "R_PILL", JS_NewInt32(ctx, 1000));
    /* 动画时长 */
    (void)JS_SetPropertyStr(ctx, tokens, "DUR_FAST", JS_NewInt32(ctx, 150));
    (void)JS_SetPropertyStr(ctx, tokens, "DUR_NORM", JS_NewInt32(ctx, 250));
    (void)JS_SetPropertyStr(ctx, tokens, "DUR_SLOW", JS_NewInt32(ctx, 400));
    (void)JS_SetPropertyStr(ctx, sys, "tokens", tokens);

    /* 挂到全局 */
    (void)JS_SetPropertyStr(ctx, sys, "__setDispatcher",
        JS_NewCFunctionParams(ctx, s_rt.func_idx_sys_set_dispatcher, JS_UNDEFINED));
    (void)JS_SetPropertyStr(ctx, global, "sys", sys);
    BIND_FN(global, "setInterval",   func_idx_set_interval);
    BIND_FN(global, "clearInterval", func_idx_clear_interval);

    return ESP_OK;
}

#undef BIND_FN
