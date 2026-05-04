/* ============================================================================
 * dynamic_app_ui_registry.c —— UI 侧"注册表层"
 *
 * 职责：
 *   把"id 字符串"翻译成"lv_obj_t* 指针"。所有 LVGL 对象都通过 id 查找，
 *   JS 永远只持有 id，永远不会拿到 LVGL 指针 —— 这是防野指针的关键。
 *
 * 提供：
 *   - UTF-8 安全截断 utf8_copy_trunc
 *   - registry_find / registry_alloc / resolve_parent
 *
 * 不直接调度 LVGL：
 *   本文件不调 lv_obj_create / lv_label_set_text 等 LVGL"动作 API"，
 *   只读 registry 里已经存在的 obj 指针。真正的 LVGL 动作在 dynamic_app_ui.c
 *   的 drain 主 switch 里。
 * ========================================================================= */

#include "dynamic_app_ui_internal.h"

#include <string.h>
#include "esp_log.h"

static const char *TAG = "dynamic_app_ui_reg";

/* ============================================================================
 * §1. UTF-8 安全截断
 *
 * 背景：
 *   id / text 拷贝进定长 buffer 时需要做长度裁剪。如果按字节直接截，
 *   可能截到一个多字节 UTF-8 字符的中间，导致 LVGL/日志显示乱码甚至异常。
 *
 * 策略：
 *   给定最多 len 字节，从尾部回退到上一个"字符边界"。
 * ========================================================================= */

static size_t utf8_truncate_len(const char *s, size_t len)
{
    if (!s || len == 0) return 0;

    size_t n = len;
    while (n > 0) {
        unsigned char c = (unsigned char)s[n - 1];
        if ((c & 0x80) == 0) {
            /* ASCII，直接是边界 */
            return n;
        }

        /* 多字节字符：回退到 lead byte */
        size_t start = n - 1;
        while (start > 0 && (((unsigned char)s[start] & 0xC0) == 0x80)) {
            start--;
        }

        unsigned char lead = (unsigned char)s[start];
        size_t need = 1;
        if      ((lead & 0xE0) == 0xC0) need = 2;
        else if ((lead & 0xF0) == 0xE0) need = 3;
        else if ((lead & 0xF8) == 0xF0) need = 4;
        else {
            /* 非法 lead byte，丢弃这段 */
            n = start;
            continue;
        }

        if (start + need <= n) {
            /* 整个字符都在 [0..n)，OK */
            return n;
        }
        /* 字符被截断，回退到字符开头之前 */
        n = start;
    }
    return 0;
}

void utf8_copy_trunc(char *dst, size_t dst_size, const char *src, size_t src_len)
{
    if (!dst || dst_size == 0) return;
    dst[0] = '\0';
    if (!src || src_len == 0) return;

    size_t max_copy = dst_size - 1;
    if (src_len < max_copy) max_copy = src_len;
    size_t safe_len = utf8_truncate_len(src, max_copy);
    if (safe_len > 0) {
        memcpy(dst, src, safe_len);
    }
    dst[safe_len] = '\0';
}

/* ============================================================================
 * §2. Registry CRUD
 * ========================================================================= */

int registry_find(const char *id)
{
    if (!id || id[0] == '\0') return -1;
    for (int i = 0; i < DYNAMIC_APP_UI_REGISTRY_MAX; i++) {
        if (!s_registry[i].used) continue;
        if (strncmp(s_registry[i].id, id, sizeof(s_registry[i].id)) == 0) {
            return i;
        }
    }
    return -1;
}

int registry_find_by_obj(const lv_obj_t *obj)
{
    if (!obj) return -1;
    for (int i = 0; i < DYNAMIC_APP_UI_REGISTRY_MAX; i++) {
        if (!s_registry[i].used) continue;
        if (s_registry[i].obj == obj) return i;
    }
    return -1;
}

int registry_alloc(const char *id, ui_obj_type_t type, lv_obj_t *obj)
{
    for (int i = 0; i < DYNAMIC_APP_UI_REGISTRY_MAX; i++) {
        if (!s_registry[i].used) {
            s_registry[i].used = true;
            s_registry[i].type = type;
            s_registry[i].obj = obj;
            strncpy(s_registry[i].id, id, sizeof(s_registry[i].id) - 1);
            s_registry[i].id[sizeof(s_registry[i].id) - 1] = '\0';
            return i;
        }
    }
    return -1;
}

/* ============================================================================
 * §3. parent 解析
 *
 *   传入字符串 → 返回 lv_obj_t*。
 *   - parent_id 为 NULL/空 → 返回 root（顶层挂载）
 *   - parent_id 非空但找不到 → 返回 NULL（让 create 失败，避免孤儿）
 *   - parent_id 找到但 obj 失效 → 返回 NULL（同上）
 * ========================================================================= */

lv_obj_t *resolve_parent(const char *parent_id)
{
    lv_obj_t *root = (lv_obj_t *)s_root;

    if (!parent_id || parent_id[0] == '\0') {
        return root;
    }

    int slot = registry_find(parent_id);
    if (slot < 0) {
        ESP_LOGW(TAG, "parent id '%s' not found, drop create", parent_id);
        return NULL;
    }

    lv_obj_t *p = s_registry[slot].obj;
    if (!p || !lv_obj_is_valid(p)) {
        ESP_LOGW(TAG, "parent id '%s' obj invalid, drop create", parent_id);
        return NULL;
    }
    return p;
}
