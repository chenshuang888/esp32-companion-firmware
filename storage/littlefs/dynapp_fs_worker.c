/* ============================================================================
 * dynapp_fs_worker.c —— LittleFS 后台串行写入 task
 *
 * 文件结构：
 *   §1. 队列 item 类型
 *   §2. submit_*  各线程入口
 *   §3. dispatch_* worker 内部处理
 *   §4. consumer task 主循环 + init
 *
 * 关键不变量：
 *   - 所有真正碰 LittleFS 的代码都跑在 s_task 上下文（单线程）
 *   - submit_* 永远 xQueueSend(timeout=0)，不阻塞调用者
 *   - active writer 只在 worker task 里读写，无需 mutex
 * ========================================================================= */

#include "dynapp_fs_worker.h"
#include "dynapp_script_store.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "dynapp_fs";

/* 队列容量：上传一次最多 64KB / 196B/chunk ≈ 335 帧；BLE 每帧需要等 ack 才发下一帧
 * （uploader_client._send_and_wait），所以队列里同时挤的 IO 任务数极少。
 * 8 是为 chunk + commit + 别的小任务留富余。 */
#define FS_QUEUE_LEN          16
#define FS_CHUNK_MAX_BYTES    196   /* 与 BLE 上传一帧 payload 大小对齐 */
#define FS_LIST_MAX           24    /* PC GUI 列表能返回的最大 app 数；
                                       * 超过部分被截断，注意 launcher 那边
                                       * MAX_DYN_APPS 是独立的 16 槽位 */

/* ============================================================================
 * §1. 队列 item
 * ========================================================================= */

typedef enum {
    FS_OP_USER_WRITE = 1,
    FS_OP_USER_REMOVE,
    FS_OP_USER_WRITE_LARGE,   /* 大块用户数据写：data 指针由 worker free */
    FS_OP_WRITER_OPEN,
    FS_OP_WRITER_APPEND,
    FS_OP_WRITER_COMMIT,
    FS_OP_WRITER_ABORT,
    FS_OP_APP_DELETE,
    FS_OP_LIST_APPS,
} fs_op_t;

typedef struct {
    fs_op_t op;
    union {
        struct {
            char     app_id[DYNAPP_SCRIPT_STORE_MAX_NAME + 1];
            char     relpath[DYNAPP_USER_DATA_MAX_PATH + 1];
            uint16_t len;
            uint8_t  data[FS_CHUNK_MAX_BYTES];
        } user_write;
        struct {
            char     app_id[DYNAPP_SCRIPT_STORE_MAX_NAME + 1];
            char     relpath[DYNAPP_USER_DATA_MAX_PATH + 1];
            uint8_t *data;       /* malloc 出来，worker 处理后 free */
            size_t   len;
        } user_write_large;
        struct {
            char app_id[DYNAPP_SCRIPT_STORE_MAX_NAME + 1];
            char relpath[DYNAPP_USER_DATA_MAX_PATH + 1];
        } user_remove;
        struct {
            char app_id[DYNAPP_SCRIPT_STORE_MAX_NAME + 1];
            char filename[DYNAPP_SCRIPT_STORE_MAX_FNAME + 1];
        } writer_open;
        struct {
            uint16_t len;
            uint8_t  data[FS_CHUNK_MAX_BYTES];
        } writer_append;
        struct {
            dynapp_fs_writer_done_cb_t cb;
            void *cb_arg;
        } writer_commit;
        struct {
            char app_id[DYNAPP_SCRIPT_STORE_MAX_NAME + 1];
            dynapp_fs_delete_done_cb_t cb;
            void *cb_arg;
        } app_delete;
        struct {
            dynapp_fs_list_done_cb_t cb;
            void *cb_arg;
        } list_apps;
    };
} fs_item_t;

static QueueHandle_t s_queue = NULL;
static TaskHandle_t  s_task  = NULL;

/* worker task 内独占：当前打开的 writer，open 后 append/commit 都作用于它 */
static dynapp_script_writer_t *s_writer = NULL;

/* 删除前的 "running check"。NULL = 不拦截。 */
static dynapp_fs_app_running_cb_t s_running_check = NULL;

/* ============================================================================
 * §2. submit_*
 * ========================================================================= */

static bool send_item(const fs_item_t *it)
{
    if (!s_queue) return false;
    return xQueueSend(s_queue, it, 0) == pdTRUE;
}

bool dynapp_fs_worker_submit_user_write(const char *app_id, const char *relpath,
                                         const uint8_t *data, size_t len)
{
    if (!app_id || !relpath || !data) return false;
    if (len == 0 || len > FS_CHUNK_MAX_BYTES) return false;
    if (strlen(app_id)  > DYNAPP_SCRIPT_STORE_MAX_NAME) return false;
    if (strlen(relpath) > DYNAPP_USER_DATA_MAX_PATH)    return false;

    fs_item_t it = { .op = FS_OP_USER_WRITE };
    strncpy(it.user_write.app_id,  app_id,  sizeof(it.user_write.app_id)  - 1);
    strncpy(it.user_write.relpath, relpath, sizeof(it.user_write.relpath) - 1);
    it.user_write.len = (uint16_t)len;
    memcpy(it.user_write.data, data, len);
    return send_item(&it);
}

bool dynapp_fs_worker_submit_user_write_large(const char *app_id,
                                               const char *relpath,
                                               const uint8_t *data, size_t len)
{
    if (!app_id || !relpath || !data) return false;
    if (len == 0 || len > DYNAPP_USER_DATA_MAX_BYTES) return false;
    if (strlen(app_id)  > DYNAPP_SCRIPT_STORE_MAX_NAME) return false;
    if (strlen(relpath) > DYNAPP_USER_DATA_MAX_PATH)    return false;

    /* 拷贝到 PSRAM（256KB 不能放栈，也不该让上层 buffer 受 worker 处理时机牵制） */
    uint8_t *copy = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!copy) {
        copy = (uint8_t *)malloc(len);   /* PSRAM 不可用时降级到 internal heap */
    }
    if (!copy) {
        ESP_LOGE(TAG, "user_write_large alloc %u failed", (unsigned)len);
        return false;
    }
    memcpy(copy, data, len);

    fs_item_t it = { .op = FS_OP_USER_WRITE_LARGE };
    strncpy(it.user_write_large.app_id,  app_id,  sizeof(it.user_write_large.app_id)  - 1);
    strncpy(it.user_write_large.relpath, relpath, sizeof(it.user_write_large.relpath) - 1);
    it.user_write_large.data = copy;
    it.user_write_large.len  = len;
    if (!send_item(&it)) {
        ESP_LOGW(TAG, "user_write_large queue full, dropping (%uB)", (unsigned)len);
        free(copy);
        return false;
    }
    return true;
}

bool dynapp_fs_worker_submit_user_remove(const char *app_id, const char *relpath)
{
    if (!app_id || !relpath) return false;
    if (strlen(app_id)  > DYNAPP_SCRIPT_STORE_MAX_NAME) return false;
    if (strlen(relpath) > DYNAPP_USER_DATA_MAX_PATH)    return false;

    fs_item_t it = { .op = FS_OP_USER_REMOVE };
    strncpy(it.user_remove.app_id,  app_id,  sizeof(it.user_remove.app_id)  - 1);
    strncpy(it.user_remove.relpath, relpath, sizeof(it.user_remove.relpath) - 1);
    return send_item(&it);
}

bool dynapp_fs_worker_submit_writer_open(const char *app_id, const char *filename)
{
    if (!app_id || !filename) return false;
    if (strlen(app_id)   > DYNAPP_SCRIPT_STORE_MAX_NAME)  return false;
    if (strlen(filename) > DYNAPP_SCRIPT_STORE_MAX_FNAME) return false;

    fs_item_t it = { .op = FS_OP_WRITER_OPEN };
    strncpy(it.writer_open.app_id,   app_id,   sizeof(it.writer_open.app_id)   - 1);
    strncpy(it.writer_open.filename, filename, sizeof(it.writer_open.filename) - 1);
    return send_item(&it);
}

bool dynapp_fs_worker_submit_writer_append(const uint8_t *data, size_t len)
{
    if (!data) return false;
    if (len == 0 || len > FS_CHUNK_MAX_BYTES) return false;

    fs_item_t it = { .op = FS_OP_WRITER_APPEND };
    it.writer_append.len = (uint16_t)len;
    memcpy(it.writer_append.data, data, len);
    return send_item(&it);
}

bool dynapp_fs_worker_submit_writer_abort(void)
{
    fs_item_t it = { .op = FS_OP_WRITER_ABORT };
    return send_item(&it);
}

bool dynapp_fs_worker_submit_writer_commit(dynapp_fs_writer_done_cb_t cb,
                                            void *cb_arg)
{
    fs_item_t it = { .op = FS_OP_WRITER_COMMIT };
    it.writer_commit.cb     = cb;
    it.writer_commit.cb_arg = cb_arg;
    return send_item(&it);
}

bool dynapp_fs_worker_submit_app_delete(const char *app_id,
                                         dynapp_fs_delete_done_cb_t cb,
                                         void *cb_arg)
{
    if (!app_id || strlen(app_id) > DYNAPP_SCRIPT_STORE_MAX_NAME) return false;
    fs_item_t it = { .op = FS_OP_APP_DELETE };
    strncpy(it.app_delete.app_id, app_id, sizeof(it.app_delete.app_id) - 1);
    it.app_delete.cb     = cb;
    it.app_delete.cb_arg = cb_arg;
    return send_item(&it);
}

bool dynapp_fs_worker_submit_list_apps(dynapp_fs_list_done_cb_t cb, void *cb_arg)
{
    fs_item_t it = { .op = FS_OP_LIST_APPS };
    it.list_apps.cb     = cb;
    it.list_apps.cb_arg = cb_arg;
    return send_item(&it);
}

void dynapp_fs_worker_set_running_check(dynapp_fs_app_running_cb_t cb)
{
    s_running_check = cb;
}

/* ============================================================================
 * §3. dispatch_*
 *
 *   只在 worker task 调；可以慢慢碰 FS。
 * ========================================================================= */

static void dispatch_user_write(const fs_item_t *it)
{
    esp_err_t e = dynapp_user_data_write(it->user_write.app_id,
                                          it->user_write.relpath,
                                          it->user_write.data,
                                          it->user_write.len);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "user_write %s/%s err=0x%x",
                 it->user_write.app_id, it->user_write.relpath, e);
    }
}

static void dispatch_user_write_large(fs_item_t *it)
{
    esp_err_t e = dynapp_user_data_write(it->user_write_large.app_id,
                                          it->user_write_large.relpath,
                                          it->user_write_large.data,
                                          it->user_write_large.len);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "user_write_large %s/%s err=0x%x (len=%u)",
                 it->user_write_large.app_id, it->user_write_large.relpath,
                 e, (unsigned)it->user_write_large.len);
    }
    free(it->user_write_large.data);
    it->user_write_large.data = NULL;
}

static void dispatch_user_remove(const fs_item_t *it)
{
    esp_err_t e = dynapp_user_data_remove(it->user_remove.app_id,
                                           it->user_remove.relpath);
    if (e != ESP_OK && e != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "user_remove %s/%s err=0x%x",
                 it->user_remove.app_id, it->user_remove.relpath, e);
    }
}

static void dispatch_writer_open(const fs_item_t *it)
{
    /* script_store 内部本身就有"单 active writer"语义：旧的没关会自动 abort。
     * 这里也跟着把本地指针清掉，统一从 store 那边重新拿。 */
    s_writer = dynapp_script_store_open_writer(it->writer_open.app_id,
                                                it->writer_open.filename);
    if (!s_writer) {
        ESP_LOGW(TAG, "writer_open %s/%s failed",
                 it->writer_open.app_id, it->writer_open.filename);
    }
}

static void dispatch_writer_append(const fs_item_t *it)
{
    if (!s_writer) return;   /* open 失败后的所有 append 静默丢 */
    esp_err_t e = dynapp_script_writer_append(s_writer,
                                               it->writer_append.data,
                                               it->writer_append.len);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "writer_append err=0x%x (writer marked failed)", e);
        /* writer 内部已置 failed=true；commit 时会回 ESP_FAIL */
    }
}

static void dispatch_writer_commit(const fs_item_t *it)
{
    esp_err_t result;
    if (!s_writer) {
        result = ESP_ERR_INVALID_STATE;
    } else {
        result = dynapp_script_writer_commit(s_writer);
        s_writer = NULL;   /* commit 内部已 free */
    }
    if (it->writer_commit.cb) {
        it->writer_commit.cb(result, it->writer_commit.cb_arg);
    }
}

static void dispatch_writer_abort(void)
{
    if (s_writer) {
        dynapp_script_writer_abort(s_writer);
        s_writer = NULL;
    }
}

static void dispatch_app_delete(const fs_item_t *it)
{
    esp_err_t result;
    if (s_running_check && s_running_check(it->app_delete.app_id)) {
        ESP_LOGW(TAG, "app_delete refused: %s is running", it->app_delete.app_id);
        result = ESP_ERR_INVALID_STATE;
    } else {
        result = dynapp_app_delete(it->app_delete.app_id);
    }
    if (it->app_delete.cb) {
        it->app_delete.cb(result, it->app_delete.cb_arg);
    }
}

static void dispatch_list_apps(const fs_item_t *it)
{
    char names[FS_LIST_MAX][DYNAPP_SCRIPT_STORE_MAX_NAME + 1];
    int n = dynapp_script_store_list(names, FS_LIST_MAX);
    if (it->list_apps.cb) {
        it->list_apps.cb((const char (*)[DYNAPP_SCRIPT_STORE_MAX_NAME + 1])names,
                         n, it->list_apps.cb_arg);
    }
}

/* ============================================================================
 * §4. consumer task + init
 * ========================================================================= */

static void worker_task(void *arg)
{
    (void)arg;
    fs_item_t it;
    ESP_LOGI(TAG, "fs worker task running");
    for (;;) {
        if (xQueueReceive(s_queue, &it, portMAX_DELAY) != pdTRUE) continue;
        switch (it.op) {
        case FS_OP_USER_WRITE:        dispatch_user_write(&it);       break;
        case FS_OP_USER_REMOVE:       dispatch_user_remove(&it);      break;
        case FS_OP_USER_WRITE_LARGE:  dispatch_user_write_large(&it); break;
        case FS_OP_WRITER_OPEN:       dispatch_writer_open(&it);      break;
        case FS_OP_WRITER_APPEND:     dispatch_writer_append(&it);    break;
        case FS_OP_WRITER_COMMIT:     dispatch_writer_commit(&it);    break;
        case FS_OP_WRITER_ABORT:      dispatch_writer_abort();        break;
        case FS_OP_APP_DELETE:        dispatch_app_delete(&it);       break;
        case FS_OP_LIST_APPS:         dispatch_list_apps(&it);        break;
        default:
            ESP_LOGW(TAG, "unknown op %d", (int)it.op);
            break;
        }
    }
}

esp_err_t dynapp_fs_worker_init(void)
{
    if (s_task) return ESP_OK;

    s_queue = xQueueCreate(FS_QUEUE_LEN, sizeof(fs_item_t));
    if (!s_queue) return ESP_ERR_NO_MEM;

    /* prio=2: 比 LVGL UI（4）低、比 idle(0) 高
     * stack=4KB: dispatch_chunk fwrite + writer_commit rmtree 几百字节足够 */
    BaseType_t ok = xTaskCreatePinnedToCore(worker_task, "dyn_fs",
                                             4096, NULL, 2, &s_task,
                                             tskNO_AFFINITY);
    if (ok != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}
