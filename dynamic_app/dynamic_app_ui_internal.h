#pragma once

/* ============================================================================
 * dynamic_app_ui_internal.h —— UI 侧三个 .c 文件之间的内部共享头
 *
 * 拆分关系：
 *   dynamic_app_ui.c          调度层：drain 主 switch、enqueue 实现、生命周期
 *   dynamic_app_ui_registry.c 注册表层：UTF-8 helper + id↔obj 映射
 *   dynamic_app_ui_styles.c   样式层：apply_style 9 个 case 分发
 *
 * 三方共享的 registry 数组、字体指针、root 门禁都通过本头暴露给同模块内部。
 * 外部模块（page/app）只用对外公开的 dynamic_app_ui.h，看不到本文件。
 * ========================================================================= */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "lvgl.h"

#include "dynamic_app_ui.h"   /* 公共 enum / cmd 结构 / 容量宏 */

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * §1. 内部数据结构
 * ========================================================================= */

/* widget 类型：drain 创建时打标，便于 setStyle/setText 校验 */
typedef enum {
    UI_OBJ_LABEL = 1,
    UI_OBJ_PANEL,
    UI_OBJ_BUTTON,
    UI_OBJ_IMAGE,
    UI_OBJ_CANVAS,
} ui_obj_type_t;

/* 注册表项：JS 用字符串 id 索引，C 端持有 LVGL 对象指针 */
typedef struct {
    bool used;
    ui_obj_type_t type;
    char id[DYNAMIC_APP_UI_ID_MAX_LEN];
    lv_obj_t *obj;
    /* 类型相关的额外资源；销毁时按 type 释放：
     *   UI_OBJ_CANVAS → uint8_t* RGB565 buffer (240×320×2 = 153600B in PSRAM)
     * 其它 type 暂时为 NULL。 */
    void *aux;
} ui_registry_entry_t;

/* ============================================================================
 * §2. 共享全局变量（定义在 dynamic_app_ui.c）
 * ========================================================================= */

extern QueueHandle_t        s_ui_queue;        /* Script→UI 命令队列 */
extern QueueHandle_t        s_event_queue;     /* UI→Script 事件队列 */
extern volatile lv_obj_t   *s_root;            /* root 门禁：NULL 时拒绝所有 enqueue_create */
extern ui_registry_entry_t  s_registry[DYNAMIC_APP_UI_REGISTRY_MAX];

extern const lv_font_t *s_font_text;
extern const lv_font_t *s_font_title;
extern const lv_font_t *s_font_huge;
extern const lv_font_t *s_font_icon24;
extern const lv_font_t *s_font_icon36;
extern const lv_font_t *s_font_num_m;

/* ============================================================================
 * §3. UTF-8 安全截断（dynamic_app_ui_registry.c 实现）
 *     避免拷贝定长 buffer 时切到多字节字符中间，导致 LVGL 显示乱码。
 * ========================================================================= */
void utf8_copy_trunc(char *dst, size_t dst_size, const char *src, size_t src_len);

/* ============================================================================
 * §4. Registry CRUD（dynamic_app_ui_registry.c 实现）
 *     所有 id↔obj 的查找/分配/解析都走这里，drain switch 不直接 memcmp。
 * ========================================================================= */

/* 按 id 查 slot；未找到返回 -1 */
int registry_find(const char *id);

/* 按 LVGL 对象指针反查 slot；未找到返回 -1。
 * root listener 用：lv_event_get_target 拿到对象 → 反查 id。 */
int registry_find_by_obj(const lv_obj_t *obj);

/* 占用一个空 slot 并填入 type/obj；满则返回 -1 */
int registry_alloc(const char *id, ui_obj_type_t type, lv_obj_t *obj);

/* 解析 parent_id 字符串到 lv_obj_t*：
 *   - NULL / 空串 → 回落到 s_root
 *   - 找不到或对象失效 → 回落到 s_root（带 WARN）
 */
lv_obj_t *resolve_parent(const char *parent_id);

/* ============================================================================
 * §5. 样式分发（dynamic_app_ui_styles.c 实现）
 *
 *     apply_style 是个大 switch，按 cmd.u.style.key 调用对应的 LVGL setter。
 *     拆出来后，drain 主循环只需要一行调用，可读性大幅提升。
 * ========================================================================= */
void apply_style(lv_obj_t *obj, const dynamic_app_ui_command_t *cmd);

#ifdef __cplusplus
}
#endif
