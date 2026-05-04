/* ============================================================================
 * dynapp_upload_manager.c —— BLE 上传协议状态机（无 task / 无 queue）
 *
 * 文件结构：
 *   §1. 状态机数据
 *   §2. submit_*  各 op 的入口（BLE host task 上调）
 *   §3. fs_worker done callback
 *   §4. CRC32 + helpers
 *   §5. init
 *
 * 线程模型：
 *   submit_*  → 永远 BLE host task；状态字段单线程访问，无锁
 *   fs_done   → fs_worker task；只解 cb_arg 拿 (op,seq) 调 status_cb，
 *               不读状态字段，不与 submit_* 竞态
 * ========================================================================= */

#include "dynapp_upload_manager.h"
#include "dynapp_fs_worker.h"
#include "dynapp_script_store.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "dynapp_upload";

/* cb_arg 编码：高 8 位 op，低 8 位 seq。fs_worker 透传到 done cb。 */
#define MK_CBARG(op, seq)   ((void *)(uintptr_t)(((uint32_t)(op) << 8) | (seq)))
#define CBARG_OP(arg)       ((uint8_t)((((uintptr_t)(arg)) >> 8) & 0xFF))
#define CBARG_SEQ(arg)      ((uint8_t)(((uintptr_t)(arg)) & 0xFF))

/* ============================================================================
 * §1. 状态机
 * ========================================================================= */

typedef enum { SESS_IDLE, SESS_RECEIVING } session_state_t;

static struct {
    session_state_t state;
    char            app_id[DYNAPP_UPLOAD_NAME_MAX + 1];
    char            filename[DYNAPP_UPLOAD_FNAME_MAX + 1];
    uint32_t        total_len;
    uint32_t        crc_expected;
    uint32_t        crc_running;
    uint32_t        next_offset;
} s_sess = { .state = SESS_IDLE };

static upload_status_cb_t s_cbs[DYNAPP_UPLOAD_MAX_STATUS_CBS];
static int                s_cb_count = 0;

/* ============================================================================
 * §2. helpers
 * ========================================================================= */

static void notify(upload_op_t op, upload_result_t r, uint8_t seq,
                    const char *name, uint32_t extra,
                    const uint8_t *list_buf, size_t list_len)
{
    /* s_cbs 只在 init / register 阶段追加写、运行时只读，不需要互斥。 */
    for (int i = 0; i < s_cb_count; i++) {
        s_cbs[i](op, r, seq, name, extra, list_buf, list_len);
    }
}

static void session_reset_and_abort(void)
{
    if (s_sess.state == SESS_RECEIVING) {
        /* 通知 fs worker 取消活跃 writer（fire-and-forget） */
        (void)dynapp_fs_worker_submit_writer_abort();
    }
    memset(&s_sess, 0, sizeof(s_sess));
    s_sess.state = SESS_IDLE;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *buf, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & -(int32_t)(crc & 1));
        }
    }
    return ~crc;
}

/* "alarm/main.js"           → app_id="alarm",   filename="main.js"
 * "alarm/assets/icon.bin"   → app_id="alarm",   filename="assets/icon.bin"
 *
 * 只在第一个 '/' 处拆 app_id；剩余部分原样作为 filename，
 * 由 dynapp_script_store.filename_is_valid 决定是否合法（接受平铺名或 "assets/<base>"）。 */
static bool split_path(const char *s,
                        char *app_id, size_t aid_cap,
                        char *filename, size_t fn_cap)
{
    if (!s) return false;
    const char *slash = strchr(s, '/');
    if (!slash) return false;
    size_t aid_len = (size_t)(slash - s);
    size_t fn_len  = strlen(slash + 1);
    if (aid_len == 0 || aid_len >= aid_cap) return false;
    if (fn_len == 0  || fn_len  >= fn_cap)  return false;
    memcpy(app_id, s, aid_len); app_id[aid_len] = '\0';
    memcpy(filename, slash + 1, fn_len); filename[fn_len] = '\0';
    return true;
}

/* ============================================================================
 * §3. fs_worker done callbacks
 *
 *   注意：cb 在 fs_worker task 上调。这里只调 status_cb（service 内部调
 *   NimBLE notify，自带锁）。绝不能写 s_sess —— 它属于 BLE host task。
 * ========================================================================= */

static void on_commit_done(esp_err_t result, void *cb_arg)
{
    uint8_t seq = CBARG_SEQ(cb_arg);
    /* commit 是 END 的延后结果：op echo 用 UPL_OP_END */
    upload_result_t r = (result == ESP_OK) ? UPL_RESULT_OK : UPL_RESULT_FS_ERROR;
    notify(UPL_OP_END, r, seq, NULL, 0, NULL, 0);
}

static void on_delete_done(esp_err_t result, void *cb_arg)
{
    uint8_t seq = CBARG_SEQ(cb_arg);
    upload_result_t r;
    if (result == ESP_OK)                    r = UPL_RESULT_OK;
    else if (result == ESP_ERR_NOT_FOUND)    r = UPL_RESULT_OK;     /* 幂等 */
    else if (result == ESP_ERR_INVALID_STATE) r = UPL_RESULT_BUSY;  /* running check 拦截 */
    else                                     r = UPL_RESULT_FS_ERROR;
    notify(UPL_OP_DELETE, r, seq, NULL, 0, NULL, 0);
}

static void on_list_done(const char names[][DYNAPP_SCRIPT_STORE_MAX_NAME + 1],
                          int count, void *cb_arg)
{
    uint8_t seq = CBARG_SEQ(cb_arg);

    /* 打包 NUL 分隔列表（service 直接当 BLE notify payload 发） */
    uint8_t buf[8 * (DYNAPP_SCRIPT_STORE_MAX_NAME + 1)];
    size_t  off = 0;
    for (int i = 0; i < count; i++) {
        size_t len = strlen(names[i]) + 1;   /* 含 \0 */
        if (off + len > sizeof(buf)) break;
        memcpy(buf + off, names[i], len);
        off += len;
    }
    notify(UPL_OP_LIST, UPL_RESULT_OK, seq, NULL,
           (uint32_t)count, buf, off);
}

/* ============================================================================
 * §4. submit_*（service 在 BLE host task 上调）
 * ========================================================================= */

bool dynapp_upload_submit_start(const char *app_slash_file,
                                 uint32_t total_len, uint32_t crc32,
                                 uint8_t seq)
{
    char app_id[DYNAPP_UPLOAD_NAME_MAX + 1];
    char filename[DYNAPP_UPLOAD_FNAME_MAX + 1];
    if (!split_path(app_slash_file,
                     app_id,   sizeof(app_id),
                     filename, sizeof(filename))) {
        notify(UPL_OP_START, UPL_RESULT_BAD_FRAME, seq, NULL, 0, NULL, 0);
        return false;
    }

    if (s_sess.state != SESS_IDLE) {
        ESP_LOGW(TAG, "START while active, abort prev %s/%s",
                 s_sess.app_id, s_sess.filename);
        session_reset_and_abort();
    }

    /* 委派 fs_worker 开 writer。fire-and-forget；如果 open 失败，
     * 后续 append 在 worker 里会被静默丢，commit 时回 FS_ERROR。 */
    if (!dynapp_fs_worker_submit_writer_open(app_id, filename)) {
        notify(UPL_OP_START, UPL_RESULT_FS_ERROR, seq, app_id, 0, NULL, 0);
        return false;
    }

    s_sess.state = SESS_RECEIVING;
    strncpy(s_sess.app_id,   app_id,   sizeof(s_sess.app_id)   - 1);
    strncpy(s_sess.filename, filename, sizeof(s_sess.filename) - 1);
    s_sess.total_len     = total_len;
    s_sess.crc_expected  = crc32;
    s_sess.crc_running   = 0;
    s_sess.next_offset   = 0;

    ESP_LOGI(TAG, "START %s/%s (%u B, crc=0x%08x)",
             app_id, filename, (unsigned)total_len, (unsigned)crc32);
    notify(UPL_OP_START, UPL_RESULT_OK, seq, app_id, 0, NULL, 0);
    return true;
}

bool dynapp_upload_submit_chunk(uint32_t offset, const uint8_t *data, uint16_t len,
                                 uint8_t seq)
{
    if (!data || len == 0) {
        notify(UPL_OP_CHUNK, UPL_RESULT_BAD_FRAME, seq, NULL, 0, NULL, 0);
        return false;
    }
    if (s_sess.state != SESS_RECEIVING) {
        notify(UPL_OP_CHUNK, UPL_RESULT_NO_SESSION, seq, NULL, 0, NULL, 0);
        return false;
    }
    if (offset != s_sess.next_offset) {
        ESP_LOGW(TAG, "CHUNK offset mismatch: got %u expected %u",
                 (unsigned)offset, (unsigned)s_sess.next_offset);
        notify(UPL_OP_CHUNK, UPL_RESULT_BAD_FRAME, seq,
                s_sess.app_id, s_sess.next_offset, NULL, 0);
        session_reset_and_abort();
        return false;
    }
    if (s_sess.next_offset + len > s_sess.total_len) {
        ESP_LOGW(TAG, "CHUNK overflow: %u + %u > %u",
                 (unsigned)s_sess.next_offset, (unsigned)len, (unsigned)s_sess.total_len);
        notify(UPL_OP_CHUNK, UPL_RESULT_TOO_LARGE, seq, s_sess.app_id, 0, NULL, 0);
        session_reset_and_abort();
        return false;
    }

    /* 异步 append；写盘失败由 commit 时统一报告。
     * 队列若满（fs_worker 跟不上 BLE 速率，理论上不会发生因为 PC 等 ack）
     * → 直接 reset session 让 PC 重发。 */
    if (!dynapp_fs_worker_submit_writer_append(data, len)) {
        ESP_LOGW(TAG, "fs queue full at offset %u", (unsigned)offset);
        notify(UPL_OP_CHUNK, UPL_RESULT_BUSY, seq, s_sess.app_id, 0, NULL, 0);
        session_reset_and_abort();
        return false;
    }

    s_sess.crc_running = crc32_update(s_sess.crc_running, data, len);
    s_sess.next_offset += len;
    notify(UPL_OP_CHUNK, UPL_RESULT_OK, seq, s_sess.app_id, s_sess.next_offset, NULL, 0);
    return true;
}

bool dynapp_upload_submit_end(uint8_t seq)
{
    if (s_sess.state != SESS_RECEIVING) {
        notify(UPL_OP_END, UPL_RESULT_NO_SESSION, seq, NULL, 0, NULL, 0);
        return false;
    }
    if (s_sess.next_offset != s_sess.total_len) {
        ESP_LOGW(TAG, "END length mismatch: got %u expected %u",
                 (unsigned)s_sess.next_offset, (unsigned)s_sess.total_len);
        notify(UPL_OP_END, UPL_RESULT_BAD_FRAME, seq,
                s_sess.app_id, s_sess.next_offset, NULL, 0);
        session_reset_and_abort();
        return false;
    }
    if (s_sess.crc_running != s_sess.crc_expected) {
        ESP_LOGW(TAG, "END crc mismatch: got 0x%08x expected 0x%08x",
                 (unsigned)s_sess.crc_running, (unsigned)s_sess.crc_expected);
        notify(UPL_OP_END, UPL_RESULT_CRC_MISMATCH, seq,
                s_sess.app_id, 0, NULL, 0);
        session_reset_and_abort();
        return false;
    }

    /* 异步 commit；on_commit_done 会回 BLE notify。
     * 同步重置状态机：下一个 START 立即可用（fs_worker 串行，自然排队）。 */
    bool ok = dynapp_fs_worker_submit_writer_commit(on_commit_done,
                                                     MK_CBARG(UPL_OP_END, seq));
    if (!ok) {
        notify(UPL_OP_END, UPL_RESULT_BUSY, seq, s_sess.app_id, 0, NULL, 0);
        session_reset_and_abort();
        return false;
    }
    ESP_LOGI(TAG, "END %s queued for commit", s_sess.app_id);
    memset(&s_sess, 0, sizeof(s_sess));
    s_sess.state = SESS_IDLE;
    return true;
}

bool dynapp_upload_submit_delete(const char *app_id, uint8_t seq)
{
    if (!app_id || !*app_id) {
        notify(UPL_OP_DELETE, UPL_RESULT_BAD_FRAME, seq, NULL, 0, NULL, 0);
        return false;
    }
    /* 删除时不要破坏正在进行的上传 */
    if (s_sess.state == SESS_RECEIVING && strcmp(s_sess.app_id, app_id) == 0) {
        notify(UPL_OP_DELETE, UPL_RESULT_BUSY, seq, app_id, 0, NULL, 0);
        return false;
    }
    bool ok = dynapp_fs_worker_submit_app_delete(app_id, on_delete_done,
                                                  MK_CBARG(UPL_OP_DELETE, seq));
    if (!ok) {
        notify(UPL_OP_DELETE, UPL_RESULT_BUSY, seq, app_id, 0, NULL, 0);
        return false;
    }
    return true;
}

bool dynapp_upload_submit_list(uint8_t seq)
{
    bool ok = dynapp_fs_worker_submit_list_apps(on_list_done,
                                                 MK_CBARG(UPL_OP_LIST, seq));
    if (!ok) {
        notify(UPL_OP_LIST, UPL_RESULT_BUSY, seq, NULL, 0, NULL, 0);
        return false;
    }
    return true;
}

/* ============================================================================
 * §5. init
 * ========================================================================= */

esp_err_t dynapp_upload_manager_register_status_cb(upload_status_cb_t cb)
{
    if (!cb) return ESP_ERR_INVALID_ARG;
    for (int i = 0; i < s_cb_count; i++) {
        if (s_cbs[i] == cb) return ESP_OK;   /* idempotent */
    }
    if (s_cb_count >= DYNAPP_UPLOAD_MAX_STATUS_CBS) return ESP_ERR_NO_MEM;
    s_cbs[s_cb_count++] = cb;
    return ESP_OK;
}

esp_err_t dynapp_upload_manager_init(upload_status_cb_t status_cb)
{
    s_cb_count = 0;
    if (status_cb) s_cbs[s_cb_count++] = status_cb;
    return ESP_OK;
}
