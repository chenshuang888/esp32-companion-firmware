/* ============================================================================
 * dynamic_app_ui.c —— UI 侧"调度层"
 *
 * 职责：
 *   1. 持有共享全局状态（s_ui_queue / s_event_queue / s_root / s_registry / fonts）
 *   2. 接收脚本入队的命令（enqueue_*）
 *   3. UI Task 主循环里 drain 命令并分发：CREATE / SET_TEXT / SET_STYLE / ATTACH_ROOT_LISTENER
 *   4. 反向事件入队（on_lv_root_event）+ 出队（pop_event）
 *   5. 生命周期：init / set_root / set_fonts / unregister_all / clear_event_queue
 *
 * 不做的事：
 *   - 字符串拷贝细节 → registry 文件（utf8_copy_trunc）
 *   - id↔obj 映射查找 → registry 文件（registry_find / resolve_parent）
 *   - 样式 9 个 case  → styles 文件（apply_style）
 *
 * 文件目录：
 *   §1. 全局变量与配置
 *   §2. 生命周期
 *   §3. 字体注入 / root 门禁
 *   §4. Script→UI 入队 API
 *   §5. UI→Script 反向事件 API
 *   §6. drain 主分发
 * ========================================================================= */

#include "dynamic_app_ui.h"
#include "dynamic_app_ui_internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "dynapp_fs_worker.h"
#include "dynapp_script_store.h"

static const char *TAG = "dynamic_app_ui";

/* ============================================================================
 * §1. 全局变量与配置
 * ========================================================================= */

#define UI_QUEUE_LEN 128   /* 命令队列容量。脚本一次 build 可能瞬间灌 60+ 条 */

QueueHandle_t        s_ui_queue    = NULL;
QueueHandle_t        s_event_queue = NULL;
volatile lv_obj_t   *s_root        = NULL;
ui_registry_entry_t  s_registry[DYNAMIC_APP_UI_REGISTRY_MAX];

/* 字体由 page 通过 set_fonts 注入，避免本组件反向依赖 app 层 */
const lv_font_t *s_font_text   = NULL;
const lv_font_t *s_font_title  = NULL;
const lv_font_t *s_font_huge   = NULL;
const lv_font_t *s_font_icon24 = NULL;
const lv_font_t *s_font_icon36 = NULL;
const lv_font_t *s_font_num_m  = NULL;

/* 当前手势会话状态（同时只有一根手指，单 indev 假设）。
 * 在 root listener 的 PRESSED 中记录，PRESSING 累计位移，
 * RELEASED 清空。unregister_all 也会清，避免残留指针。 */
static struct {
    lv_obj_t *active_target;
    char      active_id[DYNAMIC_APP_UI_ID_MAX_LEN];
    int16_t   acc_dx, acc_dy;
    bool      long_press_fired;   /* 本次手势是否已触发 LONG_PRESS；true 时吞掉 CLICKED */
} s_gesture = { .active_target = NULL };

/* 一次性 build-ready 回调：drain 在处理完 ATTACH_ROOT_LISTENER 后触发并自动清空。
 * 用于宿主页判断"脚本已经把对象树搭完，可以提交给 router 切屏"。 */
static dynamic_app_ui_ready_cb_t s_ready_cb       = NULL;
static void                     *s_ready_cb_ud   = NULL;

/* 模态状态（实际使用在文件后半段；提前声明便于 unregister_all 清理） */
static struct {
    lv_obj_t *overlay;
    uint32_t  modal_id;
} s_modal = { 0 };
static int s_modal_press_y0 = -1;

/* ============================================================================
 * §2. 生命周期
 * ========================================================================= */

esp_err_t dynamic_app_ui_init(void)
{
    if (s_ui_queue && s_event_queue) {
        return ESP_OK;
    }

    if (!s_ui_queue) {
        s_ui_queue = xQueueCreate(UI_QUEUE_LEN, sizeof(dynamic_app_ui_command_t));
        if (!s_ui_queue) return ESP_ERR_NO_MEM;
    }
    if (!s_event_queue) {
        s_event_queue = xQueueCreate(DYNAMIC_APP_UI_EVENT_QUEUE_LEN,
                                     sizeof(dynamic_app_ui_event_t));
        if (!s_event_queue) return ESP_ERR_NO_MEM;
    }

    memset(s_registry, 0, sizeof(s_registry));
    return ESP_OK;
}

void dynamic_app_ui_unregister_all(void)
{
    /* LVGL 对象本身由页面 lv_obj_del(screen) 级联释放，这里只清 registry。
     * 但 aux 指针指向我们 malloc 的额外缓冲（如 canvas 像素 buffer），LVGL
     * 不知道它的存在，必须手动 free。 */
    for (int i = 0; i < DYNAMIC_APP_UI_REGISTRY_MAX; i++) {
        if (!s_registry[i].used) continue;
        if (s_registry[i].aux) {
            free(s_registry[i].aux);
            s_registry[i].aux = NULL;
        }
    }
    memset(s_registry, 0, sizeof(s_registry));
    /* 手势状态也清，避免下次进 app 时残留指针 */
    s_gesture.active_target = NULL;
    s_gesture.active_id[0] = '\0';
    s_gesture.acc_dx = s_gesture.acc_dy = 0;
    s_gesture.long_press_fired = false;
    /* modal/toast 也归零：lvgl 对象会随 screen 一起被级联删除，
     * 这里只把 C 端跟踪指针清掉，避免下次进 app 误用悬挂指针 */
    s_modal.overlay  = NULL;
    s_modal.modal_id = 0;
    s_modal_press_y0 = -1;
    dynamic_app_ui_clear_event_queue();
    /* prepare 中途取消时，没机会触发 ready_cb，这里强清避免下次误触发 */
    s_ready_cb    = NULL;
    s_ready_cb_ud = NULL;
}

/* ============================================================================
 * §3. 字体注入 / root 门禁
 * ========================================================================= */

void dynamic_app_ui_set_root(lv_obj_t *root)
{
    s_root = root;
}

void dynamic_app_ui_set_ready_cb(dynamic_app_ui_ready_cb_t cb, void *user_data)
{
    s_ready_cb    = cb;
    s_ready_cb_ud = user_data;
}

void dynamic_app_ui_set_fonts(const lv_font_t *text,
                              const lv_font_t *title,
                              const lv_font_t *huge,
                              const lv_font_t *icon24,
                              const lv_font_t *icon36,
                              const lv_font_t *num_m)
{
    s_font_text   = text;
    s_font_title  = title;
    s_font_huge   = huge;
    s_font_icon24 = icon24;
    s_font_icon36 = icon36;
    s_font_num_m  = num_m;
}

/* ============================================================================
 * §4. Script→UI 入队 API
 *
 *   全部用 100ms 阻塞 send：队列满则让 Script Task 等 UI 消化，绝不丢命令。
 * ========================================================================= */

bool dynamic_app_ui_enqueue_set_text(const char *id, size_t id_len,
                                     const char *text, size_t text_len)
{
    if (!s_ui_queue || !id || !text) return false;

    dynamic_app_ui_command_t cmd = {0};
    cmd.type = DYNAMIC_APP_UI_CMD_SET_TEXT;
    utf8_copy_trunc(cmd.id, sizeof(cmd.id), id, id_len);
    utf8_copy_trunc(cmd.u.text, sizeof(cmd.u.text), text, text_len);

    if (cmd.id[0] == '\0') return false;
    return xQueueSend(s_ui_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE;
}

/* CREATE_LABEL/PANEL/BUTTON 共用入队逻辑：仅 cmd.type 不同 */
static bool enqueue_create(dynamic_app_ui_cmd_type_t type,
                           const char *id, size_t id_len,
                           const char *parent_id, size_t parent_len)
{
    if (!s_ui_queue || !id) return false;
    if (s_root == NULL) return false;   /* root 门禁：只允许在 PAGE_DYNAMIC_APP 期间创建 */

    dynamic_app_ui_command_t cmd = {0};
    cmd.type = type;
    utf8_copy_trunc(cmd.id,           sizeof(cmd.id),           id,        id_len);
    utf8_copy_trunc(cmd.u.parent_id,  sizeof(cmd.u.parent_id),  parent_id, parent_len);

    if (cmd.id[0] == '\0') return false;
    return xQueueSend(s_ui_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE;
}

bool dynamic_app_ui_enqueue_create_label(const char *id, size_t id_len,
                                         const char *parent_id, size_t parent_len)
{
    return enqueue_create(DYNAMIC_APP_UI_CMD_CREATE_LABEL, id, id_len, parent_id, parent_len);
}

bool dynamic_app_ui_enqueue_create_panel(const char *id, size_t id_len,
                                         const char *parent_id, size_t parent_len)
{
    return enqueue_create(DYNAMIC_APP_UI_CMD_CREATE_PANEL, id, id_len, parent_id, parent_len);
}

bool dynamic_app_ui_enqueue_create_button(const char *id, size_t id_len,
                                          const char *parent_id, size_t parent_len)
{
    return enqueue_create(DYNAMIC_APP_UI_CMD_CREATE_BUTTON, id, id_len, parent_id, parent_len);
}

bool dynamic_app_ui_enqueue_create_image(const char *id, size_t id_len,
                                         const char *parent_id, size_t parent_len,
                                         const char *src, size_t src_len)
{
    if (!s_ui_queue || !id) return false;
    if (s_root == NULL) return false;

    dynamic_app_ui_command_t cmd = {0};
    cmd.type = DYNAMIC_APP_UI_CMD_CREATE_IMAGE;
    utf8_copy_trunc(cmd.id,                       sizeof(cmd.id),
                    id,        id_len);
    utf8_copy_trunc(cmd.u.image_create.parent_id, sizeof(cmd.u.image_create.parent_id),
                    parent_id, parent_len);
    if (src && src_len > 0) {
        utf8_copy_trunc(cmd.u.image_create.src, sizeof(cmd.u.image_create.src),
                        src, src_len);
    }

    if (cmd.id[0] == '\0') return false;
    return xQueueSend(s_ui_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE;
}

bool dynamic_app_ui_enqueue_set_image_src(const char *id, size_t id_len,
                                          const char *src, size_t src_len)
{
    if (!s_ui_queue || !id) return false;

    dynamic_app_ui_command_t cmd = {0};
    cmd.type = DYNAMIC_APP_UI_CMD_SET_IMAGE_SRC;
    utf8_copy_trunc(cmd.id,    sizeof(cmd.id),    id,  id_len);
    if (src && src_len > 0) {
        utf8_copy_trunc(cmd.u.src, sizeof(cmd.u.src), src, src_len);
    }

    if (cmd.id[0] == '\0') return false;
    return xQueueSend(s_ui_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE;
}

bool dynamic_app_ui_enqueue_set_style(const char *id, size_t id_len,
                                      dynamic_app_style_key_t key,
                                      int32_t a, int32_t b, int32_t c, int32_t d)
{
    if (!s_ui_queue || !id) return false;

    dynamic_app_ui_command_t cmd = {0};
    cmd.type = DYNAMIC_APP_UI_CMD_SET_STYLE;
    utf8_copy_trunc(cmd.id, sizeof(cmd.id), id, id_len);
    cmd.u.style.key = (int32_t)key;
    cmd.u.style.a = a;
    cmd.u.style.b = b;
    cmd.u.style.c = c;
    cmd.u.style.d = d;

    if (cmd.id[0] == '\0') return false;
    return xQueueSend(s_ui_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE;
}

bool dynamic_app_ui_enqueue_attach_root_listener(const char *id, size_t id_len)
{
    if (!s_ui_queue || !id) return false;

    dynamic_app_ui_command_t cmd = {0};
    cmd.type = DYNAMIC_APP_UI_CMD_ATTACH_ROOT_LISTENER;
    utf8_copy_trunc(cmd.id, sizeof(cmd.id), id, id_len);

    if (cmd.id[0] == '\0') return false;
    return xQueueSend(s_ui_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE;
}

bool dynamic_app_ui_enqueue_destroy(const char *id, size_t id_len)
{
    if (!s_ui_queue || !id) return false;

    dynamic_app_ui_command_t cmd = {0};
    cmd.type = DYNAMIC_APP_UI_CMD_DESTROY;
    utf8_copy_trunc(cmd.id, sizeof(cmd.id), id, id_len);

    if (cmd.id[0] == '\0') return false;
    return xQueueSend(s_ui_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE;
}

bool dynamic_app_ui_enqueue_show_modal(uint32_t modal_id,
                                       const char *title, size_t title_len,
                                       const char *body,  size_t body_len,
                                       const char *action0, size_t a0_len,
                                       const char *action1, size_t a1_len)
{
    if (!s_ui_queue) return false;

    dynamic_app_ui_command_t cmd = {0};
    cmd.type = DYNAMIC_APP_UI_CMD_SHOW_MODAL;
    cmd.u.modal.modal_id = modal_id;
    if (title)   utf8_copy_trunc(cmd.u.modal.title,   sizeof(cmd.u.modal.title),   title,   title_len);
    if (body)    utf8_copy_trunc(cmd.u.modal.body,    sizeof(cmd.u.modal.body),    body,    body_len);
    if (action0) utf8_copy_trunc(cmd.u.modal.action0, sizeof(cmd.u.modal.action0), action0, a0_len);
    if (action1) utf8_copy_trunc(cmd.u.modal.action1, sizeof(cmd.u.modal.action1), action1, a1_len);

    return xQueueSend(s_ui_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE;
}

bool dynamic_app_ui_enqueue_toast(const char *text, size_t text_len, uint16_t dur_ms)
{
    if (!s_ui_queue || !text) return false;

    dynamic_app_ui_command_t cmd = {0};
    cmd.type = DYNAMIC_APP_UI_CMD_TOAST;
    utf8_copy_trunc(cmd.u.toast.text, sizeof(cmd.u.toast.text), text, text_len);
    cmd.u.toast.dur_ms = dur_ms;

    if (cmd.u.toast.text[0] == '\0') return false;
    return xQueueSend(s_ui_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE;
}

bool dynamic_app_ui_enqueue_fade_in(const char *id, size_t id_len, uint16_t delay_ms)
{
    if (!s_ui_queue || !id) return false;

    dynamic_app_ui_command_t cmd = {0};
    cmd.type = DYNAMIC_APP_UI_CMD_FADE_IN;
    utf8_copy_trunc(cmd.id, sizeof(cmd.id), id, id_len);
    cmd.u.fade.delay_ms = delay_ms;

    if (cmd.id[0] == '\0') return false;
    return xQueueSend(s_ui_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE;
}

/* ---- P2 canvas enqueue --------------------------------------------------- */

bool dynamic_app_ui_enqueue_create_canvas(const char *id, size_t id_len,
                                           const char *parent_id, size_t parent_len,
                                           uint16_t w, uint16_t h)
{
    if (!s_ui_queue || !id) return false;
    dynamic_app_ui_command_t cmd = {0};
    cmd.type = DYNAMIC_APP_UI_CMD_CREATE_CANVAS;
    utf8_copy_trunc(cmd.id, sizeof(cmd.id), id, id_len);
    if (parent_id) {
        utf8_copy_trunc(cmd.u.canvas_create.parent_id,
                        sizeof(cmd.u.canvas_create.parent_id),
                        parent_id, parent_len);
    }
    cmd.u.canvas_create.w = w;
    cmd.u.canvas_create.h = h;
    if (cmd.id[0] == '\0') return false;
    return xQueueSend(s_ui_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE;
}

bool dynamic_app_ui_enqueue_canvas_fill(const char *id, size_t id_len,
                                         uint32_t color)
{
    if (!s_ui_queue || !id) return false;
    dynamic_app_ui_command_t cmd = {0};
    cmd.type = DYNAMIC_APP_UI_CMD_CANVAS_FILL;
    utf8_copy_trunc(cmd.id, sizeof(cmd.id), id, id_len);
    cmd.u.canvas_fill.color = color;
    if (cmd.id[0] == '\0') return false;
    return xQueueSend(s_ui_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE;
}

bool dynamic_app_ui_enqueue_canvas_pixel(const char *id, size_t id_len,
                                          int16_t x, int16_t y, uint32_t color)
{
    if (!s_ui_queue || !id) return false;
    dynamic_app_ui_command_t cmd = {0};
    cmd.type = DYNAMIC_APP_UI_CMD_CANVAS_PIXEL;
    utf8_copy_trunc(cmd.id, sizeof(cmd.id), id, id_len);
    cmd.u.canvas_pixel.x = x;
    cmd.u.canvas_pixel.y = y;
    cmd.u.canvas_pixel.color = color;
    if (cmd.id[0] == '\0') return false;
    return xQueueSend(s_ui_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE;
}

bool dynamic_app_ui_enqueue_canvas_line(const char *id, size_t id_len,
                                         int16_t x0, int16_t y0,
                                         int16_t x1, int16_t y1,
                                         uint32_t color, uint8_t thickness)
{
    if (!s_ui_queue || !id) return false;
    if (thickness == 0) thickness = 1;
    if (thickness > 6)  thickness = 6;
    dynamic_app_ui_command_t cmd = {0};
    cmd.type = DYNAMIC_APP_UI_CMD_CANVAS_LINE;
    utf8_copy_trunc(cmd.id, sizeof(cmd.id), id, id_len);
    cmd.u.canvas_line.x0 = x0;
    cmd.u.canvas_line.y0 = y0;
    cmd.u.canvas_line.x1 = x1;
    cmd.u.canvas_line.y1 = y1;
    cmd.u.canvas_line.color = color;
    cmd.u.canvas_line.thickness = thickness;
    if (cmd.id[0] == '\0') return false;
    return xQueueSend(s_ui_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE;
}

bool dynamic_app_ui_enqueue_canvas_save(const char *id, size_t id_len,
                                         const char *relpath, size_t rp_len)
{
    if (!s_ui_queue || !id || !relpath) return false;
    dynamic_app_ui_command_t cmd = {0};
    cmd.type = DYNAMIC_APP_UI_CMD_CANVAS_SAVE;
    utf8_copy_trunc(cmd.id, sizeof(cmd.id), id, id_len);
    utf8_copy_trunc(cmd.u.canvas_io.relpath, sizeof(cmd.u.canvas_io.relpath),
                    relpath, rp_len);
    if (cmd.id[0] == '\0' || cmd.u.canvas_io.relpath[0] == '\0') return false;
    return xQueueSend(s_ui_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE;
}

bool dynamic_app_ui_enqueue_canvas_load(const char *id, size_t id_len,
                                         const char *relpath, size_t rp_len)
{
    if (!s_ui_queue || !id || !relpath) return false;
    dynamic_app_ui_command_t cmd = {0};
    cmd.type = DYNAMIC_APP_UI_CMD_CANVAS_LOAD;
    utf8_copy_trunc(cmd.id, sizeof(cmd.id), id, id_len);
    utf8_copy_trunc(cmd.u.canvas_io.relpath, sizeof(cmd.u.canvas_io.relpath),
                    relpath, rp_len);
    if (cmd.id[0] == '\0' || cmd.u.canvas_io.relpath[0] == '\0') return false;
    return xQueueSend(s_ui_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE;
}

/* ============================================================================
 * §5. UI→Script 反向事件
 *
 *   on_lv_root_event 在 LVGL UI 线程上下文执行：绝不能 JS_Call，只能入队。
 *   pop_event 由 Script Task 主循环调用。
 *
 *   一个 cb 处理 4 种 LVGL 事件：
 *     PRESSED   → EV_PRESS   记录按下对象，重置累计位移
 *     PRESSING  → EV_DRAG    每帧从 indev 拿增量，累计 ≥2px 才入队
 *     RELEASED  → EV_RELEASE 释放按下对象
 *     CLICKED   → EV_CLICK   LVGL 已保证只在没拖动时触发
 *
 *   PRESSING/RELEASED 的 target 用按下时记下的，避免手指拖出按钮范围
 *   后 LVGL 报错 target 导致业务接到不一致的 id。
 * ========================================================================= */

/* 当前手势会话状态（同时只有一根手指，单 indev 假设）
 * 定义在文件顶部 s_gesture，这里的 cb 直接读写。 */

#define DRAG_THRESHOLD_PX  2

static void enqueue_event(uint8_t type, const char *id, int16_t dx, int16_t dy)
{
    if (!s_event_queue || !id || id[0] == '\0') return;
    dynamic_app_ui_event_t ev = {0};
    ev.type = type;
    ev.dx = dx;
    ev.dy = dy;
    strncpy(ev.node_id, id, sizeof(ev.node_id) - 1);
    ev.node_id[sizeof(ev.node_id) - 1] = '\0';
    (void)xQueueSend(s_event_queue, &ev, 0);
}

static void on_lv_root_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        lv_obj_t *target = lv_event_get_target_obj(e);
        if (!target) return;
        int slot = registry_find_by_obj(target);
        if (slot < 0) return;
        s_gesture.active_target = target;
        s_gesture.acc_dx = s_gesture.acc_dy = 0;
        s_gesture.long_press_fired = false;
        strncpy(s_gesture.active_id, s_registry[slot].id,
                sizeof(s_gesture.active_id) - 1);
        s_gesture.active_id[sizeof(s_gesture.active_id) - 1] = '\0';
        enqueue_event(DYNAMIC_APP_UI_EV_PRESS, s_gesture.active_id, 0, 0);
        return;
    }

    if (code == LV_EVENT_PRESSING) {
        if (!s_gesture.active_target) return;
        lv_indev_t *indev = lv_indev_active();
        if (!indev) return;
        lv_point_t v = {0};
        lv_indev_get_vect(indev, &v);
        s_gesture.acc_dx += (int16_t)v.x;
        s_gesture.acc_dy += (int16_t)v.y;
        if (abs(s_gesture.acc_dx) + abs(s_gesture.acc_dy) >= DRAG_THRESHOLD_PX) {
            enqueue_event(DYNAMIC_APP_UI_EV_DRAG, s_gesture.active_id,
                          s_gesture.acc_dx, s_gesture.acc_dy);
            s_gesture.acc_dx = s_gesture.acc_dy = 0;
        }
        return;
    }

    if (code == LV_EVENT_RELEASED) {
        if (!s_gesture.active_target) return;
        enqueue_event(DYNAMIC_APP_UI_EV_RELEASE, s_gesture.active_id, 0, 0);
        s_gesture.active_target = NULL;
        s_gesture.active_id[0] = '\0';
        return;
    }

    if (code == LV_EVENT_CLICKED) {
        /* CLICKED 只在没拖动时触发；用 LVGL 报告的 target，
         * 因为 active_target 在 RELEASED 已清。
         * 长按已触发 LONG_PRESS 时吞掉本次 CLICKED，避免业务侧同时触发
         * onClick + onLongPress（典型场景：长按删除时不希望同时进入条目）。 */
        if (s_gesture.long_press_fired) {
            s_gesture.long_press_fired = false;
            return;
        }
        lv_obj_t *target = lv_event_get_target_obj(e);
        if (!target) return;
        int slot = registry_find_by_obj(target);
        if (slot < 0) return;
        enqueue_event(DYNAMIC_APP_UI_EV_CLICK, s_registry[slot].id, 0, 0);
        return;
    }

    if (code == LV_EVENT_LONG_PRESSED) {
        /* LVGL 默认 ~400ms 触发；与 PRESSED 共享 active_target，
         * 没拖动时才发（拖过会被 LVGL 自己抑制） */
        if (!s_gesture.active_target) return;
        s_gesture.long_press_fired = true;
        enqueue_event(DYNAMIC_APP_UI_EV_LONG_PRESS, s_gesture.active_id, 0, 0);
        return;
    }
}

bool dynamic_app_ui_pop_event(dynamic_app_ui_event_t *out)
{
    if (!s_event_queue || !out) return false;
    return xQueueReceive(s_event_queue, out, 0) == pdTRUE;
}

void dynamic_app_ui_clear_event_queue(void)
{
    if (!s_event_queue) return;
    xQueueReset(s_event_queue);
}

/* ============================================================================
 * §6. drain 主分发
 *
 *   被 UI 线程主循环（app_main.c）周期性调用。
 *   每次最多消 max_count 条命令，避免长时间阻塞 LVGL 渲染。
 *
 *   分发 6 条路径：
 *     CREATE_LABEL / PANEL / BUTTON → do_create()
 *     SET_TEXT             → 查 registry → lv_label_set_text
 *     SET_STYLE            → 查 registry → apply_style()  [styles.c]
 *     ATTACH_ROOT_LISTENER → 查 registry → lv_obj_add_event_cb(on_lv_root_event ×4)
 *     DESTROY              → 查 registry → lv_obj_del + slot.used=false
 * ========================================================================= */

/* 创建 LVGL 对象并入 registry。返回 0 = 成功。
 * parent_id: 来自 union 不同字段（CREATE_LABEL/PANEL/BUTTON 用 cmd->u.parent_id，
 *            CREATE_IMAGE 用 cmd->u.image_create.parent_id），由调用方传入。
 * IMAGE 类型只创建空对象；src 由 drain 在 CREATE_IMAGE 分支随后调 do_set_image_src 填。 */
static int do_create(const dynamic_app_ui_command_t *cmd, ui_obj_type_t type,
                     const char *parent_id)
{
    lv_obj_t *root = (lv_obj_t *)s_root;
    if (!root || !lv_obj_is_valid(root)) {
        s_root = NULL;
        return -1;
    }

    /* 同 id 复用：已存在且对象有效则直接返回 */
    int slot = registry_find(cmd->id);
    if (slot >= 0) {
        if (s_registry[slot].obj && lv_obj_is_valid(s_registry[slot].obj)) {
            return 0;
        }
        /* 对象已失效，回收槽位重建 */
        s_registry[slot].used = false;
    }

    lv_obj_t *parent = resolve_parent(parent_id);
    if (!parent || !lv_obj_is_valid(parent)) {
        ESP_LOGW(TAG, "parent invalid, drop create id=%s", cmd->id);
        return -2;
    }

    lv_obj_t *obj = NULL;
    switch (type) {
        case UI_OBJ_LABEL: {
            obj = lv_label_create(parent);
            lv_label_set_text(obj, "");
            lv_label_set_long_mode(obj, LV_LABEL_LONG_WRAP);
            break;
        }
        case UI_OBJ_PANEL: {
            obj = lv_obj_create(parent);
            lv_obj_remove_style_all(obj);
            /* 默认透明、无边框、无 pad；脚本可后续覆盖 */
            lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(obj, 0, 0);
            lv_obj_set_style_pad_all(obj, 0, 0);
            /* 默认禁用滚动：嵌套页面里多个 panel 同时 scrollable
             * 会出现"页面内容意外被推走"的怪象。需要滚动时业务自己开。 */
            lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            break;
        }
        case UI_OBJ_BUTTON: {
            obj = lv_btn_create(parent);
            lv_obj_remove_style_all(obj);
            lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(obj, 0, 0);
            lv_obj_set_style_shadow_width(obj, 0, 0);
            lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            /* 按下态：青色蒙层，给点击一个明显的视觉反馈（与菜单页一致） */
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x06B6D4), LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa  (obj, LV_OPA_30,              LV_STATE_PRESSED);
            break;
        }
        case UI_OBJ_IMAGE: {
            obj = lv_image_create(parent);
            /* src 由 drain 在 CREATE_IMAGE 之后通过 do_set_image_src 填，
             * 这里不直接处理路径拼接，保持 do_create 与 src 解耦。 */
            break;
        }
        case UI_OBJ_CANVAS:
            /* canvas 走 do_create_canvas，不会走到这里 */
            return -3;
    }

    if (!obj) return -3;

    /* 默认开启事件冒泡，让 click 能传到 root listener。
     * LVGL 默认 EVENT_BUBBLE 关闭 —— 子对象的事件不会触发父级 cb。
     * 我们这里统一开启，root listener 才能收到所有子按钮的点击。 */
    lv_obj_add_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* 默认开启 PRESS_LOCK：手指按下后即使移出 obj 范围，PRESSING/RELEASED
     * 仍持续发给该 obj。LVGL 默认只 button 自带这个 flag；label/panel 没有，
     * 导致用户按在卡片内的子 label 上稍微一动 LVGL 就视为 release，
     * 我们的手势状态机会立即回弹。统一开启此 flag 后，按在哪都能拖。 */
    lv_obj_add_flag(obj, LV_OBJ_FLAG_PRESS_LOCK);

    if (registry_alloc(cmd->id, type, obj) < 0) {
        ESP_LOGW(TAG, "UI registry full, drop create id=%s", cmd->id);
        lv_obj_del(obj);
        return -4;
    }
    return 0;
}

/* 把脚本传入的 src（如 "fish.bin"）拼成 LVGL FS 绝对路径
 *   "A:/littlefs/apps/<current_app>/assets/<src>"
 * 并调 lv_image_set_src。由 dynamic_app_registry_current() 拿当前 app id。
 *
 * src 为空 / 当前 app 不可用 → 调 lv_image_set_src(obj, NULL) 清掉源。 */
static void do_set_image_src(lv_obj_t *obj, const char *src)
{
    extern const char *dynamic_app_registry_current(void);
    const char *app = dynamic_app_registry_current();
    if (!obj || !lv_obj_is_valid(obj)) return;
    if (!src || !src[0] || !app || !app[0]) {
        lv_image_set_src(obj, NULL);
        return;
    }
    /* 路径长度上限：A:/littlefs/apps/<id>/assets/<src>
     * 7+9+5+15+1+7+32 ≈ 76，给 96 足够。 */
    static char path[96];
    int n = snprintf(path, sizeof(path),
                     "A:/littlefs/apps/%s/assets/%s", app, src);
    if (n <= 0 || n >= (int)sizeof(path)) {
        ESP_LOGW(TAG, "image src path too long: app=%s src=%s", app, src);
        return;
    }
    lv_image_set_src(obj, path);
}

/* ============================================================================
 * Canvas helpers（P2）
 *
 *   - canvas obj 自挂 PRESSED/PRESSING/RELEASED：dx/dy 字段填**屏幕绝对坐标**
 *     （而非现有 root listener 的相对增量），让画笔逻辑能直接拿到 (x, y)。
 *   - aux 字段持有 PSRAM 中的 RGB565 buffer（unregister/destroy 时 free）。
 *   - LVGL 9.x 的 lv_canvas_set_px 内部已 invalidate；fill/line 我们手动调
 *     lv_obj_invalidate 触发重绘。
 * ========================================================================= */

#define CANVAS_DEFAULT_W   240
#define CANVAS_DEFAULT_H   320
#define CANVAS_BPP         2     /* RGB565 */

static void on_canvas_press_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *obj = lv_event_get_current_target_obj(e);
    if (!obj) return;
    int slot = registry_find_by_obj(obj);
    if (slot < 0) return;

    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p; lv_indev_get_point(indev, &p);

    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    int16_t lx = (int16_t)(p.x - coords.x1);
    int16_t ly = (int16_t)(p.y - coords.y1);

    uint8_t ev_type = 0;
    if      (code == LV_EVENT_PRESSED)  ev_type = DYNAMIC_APP_UI_EV_PRESS;
    else if (code == LV_EVENT_PRESSING) ev_type = DYNAMIC_APP_UI_EV_DRAG;
    else if (code == LV_EVENT_RELEASED) ev_type = DYNAMIC_APP_UI_EV_RELEASE;
    else return;

    /* 复用 enqueue_event 的字段，但 dx/dy 含义在 canvas 路径下=画布内坐标 */
    enqueue_event(ev_type, s_registry[slot].id, lx, ly);
}

static int do_create_canvas(const dynamic_app_ui_command_t *cmd)
{
    lv_obj_t *root = (lv_obj_t *)s_root;
    if (!root || !lv_obj_is_valid(root)) {
        s_root = NULL;
        return -1;
    }
    /* 同 id 复用：已存在且对象有效则直接返回（不重新分配 buffer） */
    int slot = registry_find(cmd->id);
    if (slot >= 0 && s_registry[slot].obj && lv_obj_is_valid(s_registry[slot].obj)) {
        return 0;
    }
    if (slot >= 0) {
        if (s_registry[slot].aux) { free(s_registry[slot].aux); s_registry[slot].aux = NULL; }
        s_registry[slot].used = false;
    }

    lv_obj_t *parent = resolve_parent(cmd->u.canvas_create.parent_id);
    if (!parent || !lv_obj_is_valid(parent)) {
        ESP_LOGW(TAG, "parent invalid, drop canvas id=%s", cmd->id);
        return -2;
    }

    uint16_t w = cmd->u.canvas_create.w ? cmd->u.canvas_create.w : CANVAS_DEFAULT_W;
    uint16_t h = cmd->u.canvas_create.h ? cmd->u.canvas_create.h : CANVAS_DEFAULT_H;
    /* 限制画布尺寸，避免 OOM */
    if (w > 320) w = 320;
    if (h > 320) h = 320;

    size_t buf_len = (size_t)w * h * CANVAS_BPP;
    uint8_t *buf = (uint8_t *)heap_caps_malloc(buf_len,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = (uint8_t *)heap_caps_malloc(buf_len, MALLOC_CAP_8BIT);  /* fallback */
    }
    if (!buf) {
        ESP_LOGE(TAG, "canvas buf alloc %u failed", (unsigned)buf_len);
        return -3;
    }
    memset(buf, 0xFF, buf_len);  /* 默认白底 */

    lv_obj_t *obj = lv_canvas_create(parent);
    if (!obj) { free(buf); return -4; }
    lv_canvas_set_buffer(obj, buf, w, h, LV_COLOR_FORMAT_RGB565);

    /* 让 canvas 能收到指针事件（默认 lv_canvas 不 clickable） */
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_PRESS_LOCK);
    /* 不让事件冒泡到 root listener：避免 (画布坐标) 和 (root: dx=0,dy=0)
     * 双发，导致 state.cur 被反复重置。canvas 自己处理这套手势事件。 */
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(obj, on_canvas_press_event, LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(obj, on_canvas_press_event, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(obj, on_canvas_press_event, LV_EVENT_RELEASED, NULL);

    int new_slot = registry_alloc(cmd->id, UI_OBJ_CANVAS, obj);
    if (new_slot < 0) {
        ESP_LOGW(TAG, "registry full, drop canvas id=%s", cmd->id);
        lv_obj_del(obj);
        free(buf);
        return -5;
    }
    s_registry[new_slot].aux = buf;
    return 0;
}

/* RGB565 单像素写入 */
static inline void canvas_putpx(uint8_t *buf, int w, int h, int x, int y,
                                 uint32_t rgb888)
{
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    uint16_t r = ((rgb888 >> 16) & 0xFF) >> 3;
    uint16_t g = ((rgb888 >>  8) & 0xFF) >> 2;
    uint16_t b = ( rgb888        & 0xFF) >> 3;
    uint16_t v = (uint16_t)((r << 11) | (g << 5) | b);
    /* LVGL RGB565 默认小端：低字节在前 */
    uint8_t *p = buf + (y * w + x) * 2;
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static void canvas_fill_buf(uint8_t *buf, int w, int h, uint32_t rgb888)
{
    uint16_t r = ((rgb888 >> 16) & 0xFF) >> 3;
    uint16_t g = ((rgb888 >>  8) & 0xFF) >> 2;
    uint16_t b = ( rgb888        & 0xFF) >> 3;
    uint16_t v = (uint16_t)((r << 11) | (g << 5) | b);
    uint8_t lo = (uint8_t)(v & 0xFF), hi = (uint8_t)(v >> 8);
    int n = w * h;
    uint8_t *p = buf;
    for (int i = 0; i < n; i++) { *p++ = lo; *p++ = hi; }
}

/* Bresenham 线 + thickness（沿主线每点画一个 (2t-1)×(2t-1) 方块） */
static void canvas_line_buf(uint8_t *buf, int w, int h,
                             int x0, int y0, int x1, int y1,
                             uint32_t rgb888, int thickness)
{
    int dx =  abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;
    int half = thickness / 2;
    for (;;) {
        for (int oy = -half; oy <= half; oy++) {
            for (int ox = -half; ox <= half; ox++) {
                canvas_putpx(buf, w, h, x0 + ox, y0 + oy, rgb888);
            }
        }
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* 把指定 canvas 的 buffer 同步落盘到 data/<rel>。
 * 走 fs_worker 的 large 路径：拷一份到 PSRAM 后 worker 串行写 + free。 */
static void do_canvas_save(const char *id, const char *relpath)
{
    extern const char *dynamic_app_registry_current(void);
    const char *app = dynamic_app_registry_current();
    if (!app || !app[0]) return;
    int slot = registry_find(id);
    if (slot < 0 || s_registry[slot].type != UI_OBJ_CANVAS) {
        ESP_LOGW(TAG, "canvas_save: id=%s not a canvas", id);
        return;
    }
    lv_obj_t *obj = s_registry[slot].obj;
    if (!obj || !lv_obj_is_valid(obj)) return;
    uint8_t *buf = (uint8_t *)s_registry[slot].aux;
    if (!buf) return;

    int32_t w = lv_obj_get_width(obj);
    int32_t h = lv_obj_get_height(obj);
    if (w <= 0 || h <= 0) return;
    size_t len = (size_t)w * h * CANVAS_BPP;

    if (!dynapp_fs_worker_submit_user_write_large(app, relpath, buf, len)) {
        ESP_LOGW(TAG, "canvas_save submit failed (%uB)", (unsigned)len);
    }
}

/* 同步从 data/<rel> 读回 buffer（read_file 同步），完了 invalidate canvas。 */
static void do_canvas_load(const char *id, const char *relpath)
{
    extern const char *dynamic_app_registry_current(void);
    const char *app = dynamic_app_registry_current();
    if (!app || !app[0]) return;
    int slot = registry_find(id);
    if (slot < 0 || s_registry[slot].type != UI_OBJ_CANVAS) return;
    lv_obj_t *obj = s_registry[slot].obj;
    if (!obj || !lv_obj_is_valid(obj)) return;
    uint8_t *buf = (uint8_t *)s_registry[slot].aux;
    if (!buf) return;

    int32_t w = lv_obj_get_width(obj);
    int32_t h = lv_obj_get_height(obj);
    if (w <= 0 || h <= 0) return;
    size_t expect = (size_t)w * h * CANVAS_BPP;

    uint8_t *file_buf = NULL;
    size_t   file_len = 0;
    if (dynapp_user_data_read(app, relpath, &file_buf, &file_len) != ESP_OK) {
        ESP_LOGW(TAG, "canvas_load %s/%s read failed", app, relpath);
        return;
    }
    size_t copy = file_len < expect ? file_len : expect;
    memcpy(buf, file_buf, copy);
    if (copy < expect) {
        memset(buf + copy, 0xFF, expect - copy);   /* 文件短了用白色补齐 */
    }
    dynapp_script_store_release(file_buf);
    lv_obj_invalidate(obj);
}

/* ============================================================================
 * Modal / Toast / Fade-in（自包含 LVGL 实现，避免组件循环依赖 app）
 * ========================================================================= */

#define MODAL_W       224
#define MODAL_MAX_H   270
#define MODAL_PAD     12
#define TOKEN_PANEL   0xFFFFFF
#define TOKEN_BG      0xF2F2F7
#define TOKEN_BORDER  0xC6C6C8
#define TOKEN_TEXT    0x000000
#define TOKEN_DIM     0x3C3C43
#define TOKEN_MUTED   0x6E6E73
#define TOKEN_ACCENT  0x007AFF

/* 当前活动 modal 的 modal_id（实际状态定义在文件顶部 s_modal / s_modal_press_y0）。
 * 同时只有一个 modal，新弹出会先关旧的。 */

/* 把 uint32_t modal_id 写成字符串，事件队列回 JS */
static void modal_emit(int8_t action_idx)
{
    if (!s_event_queue || !s_modal.overlay) return;
    dynamic_app_ui_event_t ev = {0};
    ev.type = DYNAMIC_APP_UI_EV_MODAL;
    ev.dx   = (int16_t)action_idx;
    snprintf(ev.node_id, sizeof(ev.node_id), "%u",
             (unsigned)s_modal.modal_id);
    (void)xQueueSend(s_event_queue, &ev, 0);
}

static void modal_destroy(void)
{
    if (s_modal.overlay) {
        lv_obj_del(s_modal.overlay);
        s_modal.overlay = NULL;
    }
    s_modal.modal_id = 0;
}

static void on_modal_overlay_clicked(lv_event_t *e)
{
    lv_obj_t *target = lv_event_get_target_obj(e);
    if (target != s_modal.overlay) return;   /* 只点遮罩自己才关 */
    modal_emit(-1);
    modal_destroy();
}

static void on_modal_action_clicked(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    modal_emit((int8_t)idx);
    modal_destroy();
}

/* y 方向滑出关闭的状态在文件顶部 s_modal_press_y0 */

static void on_modal_card_pressed(lv_event_t *e)
{
    (void)e;
    lv_indev_t *id = lv_indev_active();
    if (!id) { s_modal_press_y0 = -1; return; }
    lv_point_t p; lv_indev_get_point(id, &p);
    s_modal_press_y0 = p.y;
}

static void on_modal_card_released(lv_event_t *e)
{
    (void)e;
    if (s_modal_press_y0 < 0) return;
    lv_indev_t *id = lv_indev_active();
    if (!id) { s_modal_press_y0 = -1; return; }
    lv_point_t p; lv_indev_get_point(id, &p);
    int dy = p.y - s_modal_press_y0;
    s_modal_press_y0 = -1;
    if (dy >= 30) {   /* 下滑关闭 */
        modal_emit(-1);
        modal_destroy();
    }
}

static void do_show_modal(const dynamic_app_ui_command_t *cmd)
{
    /* 已有 modal：先发"取消"事件再清理 */
    if (s_modal.overlay) {
        modal_emit(-1);
        modal_destroy();
    }

    lv_obj_t *parent = lv_screen_active();
    if (!parent) return;

    s_modal.overlay = lv_obj_create(parent);
    s_modal.modal_id = cmd->u.modal.modal_id;
    lv_obj_remove_style_all(s_modal.overlay);
    lv_obj_set_size(s_modal.overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_align(s_modal.overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_modal.overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa  (s_modal.overlay, LV_OPA_60, 0);
    lv_obj_clear_flag(s_modal.overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag  (s_modal.overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_modal.overlay, on_modal_overlay_clicked,
                        LV_EVENT_CLICKED, NULL);

    lv_obj_t *card = lv_obj_create(s_modal.overlay);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, MODAL_W);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(card, MODAL_MAX_H, 0);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(TOKEN_PANEL), 0);
    lv_obj_set_style_bg_opa  (card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius  (card, 14, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(TOKEN_BORDER), 0);
    lv_obj_set_style_pad_all(card, MODAL_PAD, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, 8, 0);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, on_modal_card_pressed,  LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(card, on_modal_card_released, LV_EVENT_RELEASED, NULL);

    /* title（可选） */
    if (cmd->u.modal.title[0]) {
        lv_obj_t *t = lv_label_create(card);
        lv_label_set_text(t, cmd->u.modal.title);
        lv_obj_set_width(t, LV_PCT(100));
        lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(t, lv_color_hex(TOKEN_TEXT), 0);
        if (s_font_title) lv_obj_set_style_text_font(t, s_font_title, 0);
    }

    /* body（可选） */
    if (cmd->u.modal.body[0]) {
        lv_obj_t *b = lv_label_create(card);
        lv_label_set_text(b, cmd->u.modal.body);
        lv_obj_set_width(b, LV_PCT(100));
        lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(b, lv_color_hex(TOKEN_DIM), 0);
        if (s_font_text) lv_obj_set_style_text_font(b, s_font_text, 0);
    }

    /* 按钮栏 */
    bool has_a0 = cmd->u.modal.action0[0] != '\0';
    bool has_a1 = cmd->u.modal.action1[0] != '\0';
    if (has_a0 || has_a1) {
        lv_obj_t *bar = lv_obj_create(card);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, LV_PCT(100), 44);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_EVENLY,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(bar, 8, 0);
        lv_obj_set_style_pad_top(bar, 4, 0);

        const char *labels[2] = { cmd->u.modal.action0, cmd->u.modal.action1 };
        for (int i = 0; i < 2; i++) {
            if (!labels[i][0]) continue;
            lv_obj_t *btn = lv_btn_create(bar);
            lv_obj_remove_style_all(btn);
            lv_obj_set_height(btn, 32);
            lv_obj_set_flex_grow(btn, 1);
            lv_obj_set_style_radius(btn, 1000, 0);
            /* 0=次按钮（灰底黑字），1=主按钮（蓝底白字）*/
            bool is_primary = (i == 1) || !has_a1;
            if (is_primary) {
                lv_obj_set_style_bg_color(btn, lv_color_hex(TOKEN_ACCENT), 0);
                lv_obj_set_style_bg_opa  (btn, LV_OPA_COVER, 0);
            } else {
                lv_obj_set_style_bg_color(btn, lv_color_hex(TOKEN_BG), 0);
                lv_obj_set_style_bg_opa  (btn, LV_OPA_COVER, 0);
                lv_obj_set_style_border_width(btn, 1, 0);
                lv_obj_set_style_border_color(btn, lv_color_hex(TOKEN_BORDER), 0);
            }
            lv_obj_set_style_bg_opa(btn, LV_OPA_70, LV_STATE_PRESSED);
            lv_obj_add_event_cb(btn, on_modal_action_clicked,
                                LV_EVENT_CLICKED, (void *)(intptr_t)i);

            lv_obj_t *lbl = lv_label_create(btn);
            lv_label_set_text(lbl, labels[i]);
            lv_obj_set_style_text_color(lbl,
                is_primary ? lv_color_hex(TOKEN_PANEL) : lv_color_hex(TOKEN_TEXT), 0);
            if (s_font_title) lv_obj_set_style_text_font(lbl, s_font_title, 0);
            lv_obj_center(lbl);
        }
    }

    /* 淡入动画 */
    lv_obj_set_style_opa(s_modal.overlay, LV_OPA_TRANSP, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_modal.overlay);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&a, 150);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_style_opa);
    lv_anim_start(&a);
}

/* ---- toast：屏底 1500ms 自动消失 ---- */

static void toast_anim_end_cb(lv_anim_t *a)
{
    lv_obj_t *obj = (lv_obj_t *)a->var;
    if (obj && lv_obj_is_valid(obj)) lv_obj_del(obj);
}

static void toast_dismiss_timer_cb(lv_timer_t *t)
{
    lv_obj_t *obj = (lv_obj_t *)lv_timer_get_user_data(t);
    lv_timer_del(t);
    if (!obj || !lv_obj_is_valid(obj)) return;
    /* 淡出后销毁 */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&a, 200);
    lv_anim_set_exec_cb (&a, (lv_anim_exec_xcb_t)lv_obj_set_style_opa);
    lv_anim_set_completed_cb(&a, toast_anim_end_cb);
    lv_anim_start(&a);
}

static void do_toast(const dynamic_app_ui_command_t *cmd)
{
    lv_obj_t *parent = lv_screen_active();
    if (!parent) return;

    lv_obj_t *t = lv_obj_create(parent);
    lv_obj_remove_style_all(t);
    lv_obj_set_size(t, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(t, 200, 0);
    lv_obj_align(t, LV_ALIGN_BOTTOM_MID, 0, -50);
    lv_obj_set_style_bg_color(t, lv_color_hex(0x222222), 0);
    lv_obj_set_style_bg_opa  (t, LV_OPA_80, 0);
    lv_obj_set_style_radius  (t, 1000, 0);
    lv_obj_set_style_pad_hor (t, 16, 0);
    lv_obj_set_style_pad_ver (t, 8, 0);
    lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(t);
    lv_label_set_text(lbl, cmd->u.toast.text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(TOKEN_PANEL), 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    if (s_font_text) lv_obj_set_style_text_font(lbl, s_font_text, 0);

    /* 淡入 */
    lv_obj_set_style_opa(t, LV_OPA_TRANSP, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, t);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&a, 150);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_style_opa);
    lv_anim_start(&a);

    /* 自动消失 timer */
    uint32_t dur = cmd->u.toast.dur_ms ? cmd->u.toast.dur_ms : 1500;
    lv_timer_t *tm = lv_timer_create(toast_dismiss_timer_cb, dur, t);
    lv_timer_set_repeat_count(tm, 1);
}

/* ---- fade in：给已存在的对象做 opa 0→cover ---- */

static void do_fade_in(lv_obj_t *obj, uint16_t delay_ms)
{
    if (!obj || !lv_obj_is_valid(obj)) return;
    lv_obj_set_style_opa(obj, LV_OPA_TRANSP, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&a, 250);
    lv_anim_set_delay   (&a, delay_ms);
    lv_anim_set_exec_cb (&a, (lv_anim_exec_xcb_t)lv_obj_set_style_opa);
    lv_anim_start(&a);
}

void dynamic_app_ui_drain(int max_count)
{
    if (!s_ui_queue || max_count <= 0) return;

    dynamic_app_ui_command_t cmd;
    int handled = 0;

    while (handled < max_count && xQueueReceive(s_ui_queue, &cmd, 0) == pdTRUE) {
        handled++;

        switch (cmd.type) {
            case DYNAMIC_APP_UI_CMD_CREATE_LABEL:
                (void)do_create(&cmd, UI_OBJ_LABEL, cmd.u.parent_id);
                break;

            case DYNAMIC_APP_UI_CMD_CREATE_PANEL:
                (void)do_create(&cmd, UI_OBJ_PANEL, cmd.u.parent_id);
                break;

            case DYNAMIC_APP_UI_CMD_CREATE_BUTTON:
                (void)do_create(&cmd, UI_OBJ_BUTTON, cmd.u.parent_id);
                break;

            case DYNAMIC_APP_UI_CMD_CREATE_IMAGE: {
                int rc = do_create(&cmd, UI_OBJ_IMAGE, cmd.u.image_create.parent_id);
                if (rc != 0) break;
                /* 创建成功后立即填 src（如果 enqueue 时附带了的话） */
                if (cmd.u.image_create.src[0]) {
                    int slot = registry_find(cmd.id);
                    if (slot >= 0) {
                        do_set_image_src(s_registry[slot].obj,
                                         cmd.u.image_create.src);
                    }
                }
                break;
            }

            case DYNAMIC_APP_UI_CMD_SET_IMAGE_SRC: {
                int slot = registry_find(cmd.id);
                if (slot < 0) break;
                lv_obj_t *obj = s_registry[slot].obj;
                if (!obj || !lv_obj_is_valid(obj)) {
                    s_registry[slot].obj = NULL;
                    break;
                }
                if (s_registry[slot].type != UI_OBJ_IMAGE) {
                    ESP_LOGW(TAG, "setImageSrc on non-image id=%s", cmd.id);
                    break;
                }
                do_set_image_src(obj, cmd.u.src);
                break;
            }

            case DYNAMIC_APP_UI_CMD_SET_TEXT: {
                int slot = registry_find(cmd.id);
                if (slot < 0) break;
                lv_obj_t *obj = s_registry[slot].obj;
                if (!obj || !lv_obj_is_valid(obj)) {
                    s_registry[slot].obj = NULL;
                    break;
                }
                if (lv_obj_has_class(obj, &lv_label_class)) {
                    lv_label_set_text(obj, cmd.u.text);
                }
                break;
            }

            case DYNAMIC_APP_UI_CMD_SET_STYLE: {
                int slot = registry_find(cmd.id);
                if (slot < 0) break;
                lv_obj_t *obj = s_registry[slot].obj;
                if (!obj || !lv_obj_is_valid(obj)) {
                    s_registry[slot].obj = NULL;
                    break;
                }
                apply_style(obj, &cmd);   /* → styles.c */
                break;
            }

            case DYNAMIC_APP_UI_CMD_ATTACH_ROOT_LISTENER: {
                int slot = registry_find(cmd.id);
                if (slot < 0) {
                    ESP_LOGW(TAG, "attach_root_listener: id %s not found", cmd.id);
                    break;
                }
                lv_obj_t *obj = s_registry[slot].obj;
                if (!obj || !lv_obj_is_valid(obj)) {
                    s_registry[slot].obj = NULL;
                    break;
                }
                /* 一个 cb 订四种事件，cb 内部按 code 分发 */
                lv_obj_add_event_cb(obj, on_lv_root_event, LV_EVENT_PRESSED,      NULL);
                lv_obj_add_event_cb(obj, on_lv_root_event, LV_EVENT_PRESSING,     NULL);
                lv_obj_add_event_cb(obj, on_lv_root_event, LV_EVENT_RELEASED,     NULL);
                lv_obj_add_event_cb(obj, on_lv_root_event, LV_EVENT_CLICKED,      NULL);
                lv_obj_add_event_cb(obj, on_lv_root_event, LV_EVENT_LONG_PRESSED, NULL);

                /* "build ready" 信号：脚本通常在 mount + 首次 rerender 完成后
                 * 才调 attachRootListener，因此这里也是宿主页可以放心切屏的时机。
                 * 一次性触发：取出 cb 后立即清空，避免重复触发。 */
                if (s_ready_cb) {
                    dynamic_app_ui_ready_cb_t cb = s_ready_cb;
                    void *ud = s_ready_cb_ud;
                    s_ready_cb    = NULL;
                    s_ready_cb_ud = NULL;
                    cb(ud);
                }
                break;
            }

            case DYNAMIC_APP_UI_CMD_DESTROY: {
                int slot = registry_find(cmd.id);
                if (slot < 0) break;   /* 已被前一次 destroy 释放，幂等 */
                lv_obj_t *obj = s_registry[slot].obj;
                if (obj && lv_obj_is_valid(obj)) {
                    /* lv_obj_del 会级联删子对象。约定 JS 侧自底向上调 destroy，
                     * 子 slot 已先被释放；万一没递归，残留 slot 留待后续操作时
                     * 由 lv_obj_is_valid 检查清掉，最坏 page 退出 unregister_all。 */
                    lv_obj_del(obj);
                }
                if (s_registry[slot].aux) {
                    free(s_registry[slot].aux);
                    s_registry[slot].aux = NULL;
                }
                s_registry[slot].used = false;
                s_registry[slot].obj = NULL;
                s_registry[slot].id[0] = '\0';
                break;
            }

            case DYNAMIC_APP_UI_CMD_SHOW_MODAL:
                do_show_modal(&cmd);
                break;

            case DYNAMIC_APP_UI_CMD_TOAST:
                do_toast(&cmd);
                break;

            case DYNAMIC_APP_UI_CMD_FADE_IN: {
                int slot = registry_find(cmd.id);
                if (slot < 0) break;
                lv_obj_t *obj = s_registry[slot].obj;
                if (obj && lv_obj_is_valid(obj)) {
                    do_fade_in(obj, cmd.u.fade.delay_ms);
                } else {
                    s_registry[slot].obj = NULL;
                }
                break;
            }

            case DYNAMIC_APP_UI_CMD_CREATE_CANVAS:
                (void)do_create_canvas(&cmd);
                break;

            case DYNAMIC_APP_UI_CMD_CANVAS_FILL: {
                int slot = registry_find(cmd.id);
                if (slot < 0 || s_registry[slot].type != UI_OBJ_CANVAS) break;
                lv_obj_t *obj = s_registry[slot].obj;
                uint8_t *buf = (uint8_t *)s_registry[slot].aux;
                if (!obj || !lv_obj_is_valid(obj) || !buf) break;
                int32_t w = lv_obj_get_width(obj);
                int32_t h = lv_obj_get_height(obj);
                if (w <= 0 || h <= 0) break;
                canvas_fill_buf(buf, w, h, cmd.u.canvas_fill.color);
                lv_obj_invalidate(obj);
                break;
            }

            case DYNAMIC_APP_UI_CMD_CANVAS_PIXEL: {
                int slot = registry_find(cmd.id);
                if (slot < 0 || s_registry[slot].type != UI_OBJ_CANVAS) break;
                lv_obj_t *obj = s_registry[slot].obj;
                uint8_t *buf = (uint8_t *)s_registry[slot].aux;
                if (!obj || !lv_obj_is_valid(obj) || !buf) break;
                int32_t w = lv_obj_get_width(obj);
                int32_t h = lv_obj_get_height(obj);
                canvas_putpx(buf, w, h,
                             cmd.u.canvas_pixel.x, cmd.u.canvas_pixel.y,
                             cmd.u.canvas_pixel.color);
                lv_obj_invalidate(obj);
                break;
            }

            case DYNAMIC_APP_UI_CMD_CANVAS_LINE: {
                int slot = registry_find(cmd.id);
                if (slot < 0 || s_registry[slot].type != UI_OBJ_CANVAS) break;
                lv_obj_t *obj = s_registry[slot].obj;
                uint8_t *buf = (uint8_t *)s_registry[slot].aux;
                if (!obj || !lv_obj_is_valid(obj) || !buf) break;
                int32_t w = lv_obj_get_width(obj);
                int32_t h = lv_obj_get_height(obj);
                canvas_line_buf(buf, w, h,
                                cmd.u.canvas_line.x0, cmd.u.canvas_line.y0,
                                cmd.u.canvas_line.x1, cmd.u.canvas_line.y1,
                                cmd.u.canvas_line.color,
                                cmd.u.canvas_line.thickness);
                lv_obj_invalidate(obj);
                break;
            }

            case DYNAMIC_APP_UI_CMD_CANVAS_SAVE:
                do_canvas_save(cmd.id, cmd.u.canvas_io.relpath);
                break;

            case DYNAMIC_APP_UI_CMD_CANVAS_LOAD:
                do_canvas_load(cmd.id, cmd.u.canvas_io.relpath);
                break;

            default:
                break;
        }
    }
}
