#include "sub_router.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "sub_router";

typedef struct {
    int                       page_id;
    const page_callbacks_t   *cb;
} entry_t;

struct sub_router {
    entry_t  *entries;
    uint8_t   entry_capacity;
    uint8_t   entry_count;

    int      *history;        /* 环形栈：栈底 → 栈顶 */
    uint8_t   history_depth;
    uint8_t   history_count;

    int        current_id;     /* -1 = 无 */
    lv_obj_t  *current_screen;
};

/* ============================================================================
 * 内部
 * ========================================================================= */

static const page_callbacks_t *find_cb(sub_router_t *r, int id)
{
    for (uint8_t i = 0; i < r->entry_count; i++) {
        if (r->entries[i].page_id == id) return r->entries[i].cb;
    }
    return NULL;
}

static void destroy_current_locked(sub_router_t *r)
{
    if (r->current_id < 0) return;
    const page_callbacks_t *old = find_cb(r, r->current_id);
    if (old && old->destroy) {
        old->destroy();
    } else if (r->current_screen) {
        lv_obj_del(r->current_screen);
    }
    r->current_id = -1;
    r->current_screen = NULL;
}

static esp_err_t load_page(sub_router_t *r, int page_id)
{
    const page_callbacks_t *cb = find_cb(r, page_id);
    if (!cb || !cb->create) {
        ESP_LOGE(TAG, "page %d not registered or missing create", page_id);
        return ESP_ERR_NOT_FOUND;
    }

    destroy_current_locked(r);

    lv_obj_t *screen = cb->create();
    if (!screen) {
        ESP_LOGE(TAG, "page %d create returned NULL", page_id);
        return ESP_FAIL;
    }
    lv_scr_load(screen);
    r->current_id = page_id;
    r->current_screen = screen;
    return ESP_OK;
}

/* ============================================================================
 * API
 * ========================================================================= */

sub_router_t *sub_router_create(uint8_t page_capacity, uint8_t history_depth)
{
    if (page_capacity == 0) page_capacity = 4;
    if (history_depth == 0) history_depth = 4;

    sub_router_t *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->entries = calloc(page_capacity, sizeof(entry_t));
    r->history = calloc(history_depth, sizeof(int));
    if (!r->entries || !r->history) {
        free(r->entries);
        free(r->history);
        free(r);
        return NULL;
    }
    r->entry_capacity = page_capacity;
    r->history_depth  = history_depth;
    r->current_id     = -1;
    return r;
}

void sub_router_destroy(sub_router_t *r)
{
    if (!r) return;
    destroy_current_locked(r);
    free(r->entries);
    free(r->history);
    free(r);
}

esp_err_t sub_router_register(sub_router_t *r, int page_id, const page_callbacks_t *cb)
{
    if (!r || !cb || !cb->create) return ESP_ERR_INVALID_ARG;
    if (find_cb(r, page_id)) {
        ESP_LOGE(TAG, "page %d already registered", page_id);
        return ESP_ERR_INVALID_STATE;
    }
    if (r->entry_count >= r->entry_capacity) {
        ESP_LOGE(TAG, "registry full");
        return ESP_ERR_NO_MEM;
    }
    r->entries[r->entry_count].page_id = page_id;
    r->entries[r->entry_count].cb      = cb;
    r->entry_count++;
    return ESP_OK;
}

esp_err_t sub_router_push(sub_router_t *r, int page_id)
{
    if (!r) return ESP_ERR_INVALID_ARG;
    if (r->current_id == page_id) return ESP_OK;

    int from = r->current_id;
    esp_err_t err = load_page(r, page_id);
    if (err != ESP_OK) return err;

    /* 来源入栈（首次进入 from = -1 不入栈） */
    if (from >= 0) {
        if (r->history_count < r->history_depth) {
            r->history[r->history_count++] = from;
        } else {
            /* 栈满：丢弃最早一项（环形覆盖） */
            ESP_LOGW(TAG, "history overflow, dropping oldest");
            for (uint8_t i = 1; i < r->history_depth; i++) {
                r->history[i - 1] = r->history[i];
            }
            r->history[r->history_depth - 1] = from;
        }
    }
    return ESP_OK;
}

esp_err_t sub_router_pop(sub_router_t *r)
{
    if (!r) return ESP_ERR_INVALID_ARG;
    if (r->history_count == 0) return ESP_ERR_INVALID_STATE;

    int target = r->history[--r->history_count];
    return load_page(r, target);
}

esp_err_t sub_router_replace(sub_router_t *r, int page_id)
{
    if (!r) return ESP_ERR_INVALID_ARG;
    r->history_count = 0;
    return load_page(r, page_id);
}

int sub_router_current(sub_router_t *r)
{
    return r ? r->current_id : -1;
}

void sub_router_tick(sub_router_t *r)
{
    if (!r || r->current_id < 0) return;
    const page_callbacks_t *cb = find_cb(r, r->current_id);
    if (cb && cb->update) cb->update();
}
