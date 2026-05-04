#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * dynamic_app_ui：Script Task <-> UI Task 双向桥接层。
 *
 * 单向（Script -> UI）命令队列 s_ui_queue：
 *   脚本侧把 createLabel/createPanel/createButton/setText/setStyle/
 *   attachRootListener 等"要做什么"封装成 command，UI Task 在
 *   dynamic_app_ui_drain() 出队执行。
 *
 * 反向（UI -> Script）事件队列 s_event_queue：
 *   LVGL root listener 在用户点击时拿到被点对象的 id 字符串入队；
 *   Script Task 在主循环里 pop 并通过 JS 全局 dispatcher 派发。
 *
 * 线程规则：
 * - enqueue_*：可在任意线程调用（典型是脚本线程）
 * - unregister/drain/pop_event：必须在它们对应的所有者线程调用
 */

#define DYNAMIC_APP_UI_ID_MAX_LEN     32
#define DYNAMIC_APP_UI_TEXT_MAX_LEN   128
#define DYNAMIC_APP_UI_REGISTRY_MAX   256
#define DYNAMIC_APP_UI_EVENT_QUEUE_LEN 8

/* 图片资源相对路径上限：相对当前 app 的 assets/ 目录的文件名（如 "fish.bin"）。
 * 31 与 DYNAPP_USER_DATA_MAX_PATH 对齐；C 端会拼成
 *   "A:/littlefs/apps/<id>/assets/<src>"
 * 后传给 lv_image_set_src()。 */
#define DYNAMIC_APP_UI_SRC_MAX_LEN    32

/* canvas save/load 时的 data/ 沙箱相对路径上限（与 DYNAPP_USER_DATA_MAX_PATH 对齐） */
#define DYNAMIC_APP_USER_PATH_MAX     32

typedef enum {
    DYNAMIC_APP_UI_CMD_SET_TEXT = 1,
    DYNAMIC_APP_UI_CMD_CREATE_LABEL,
    DYNAMIC_APP_UI_CMD_CREATE_PANEL,
    DYNAMIC_APP_UI_CMD_CREATE_BUTTON,
    DYNAMIC_APP_UI_CMD_CREATE_IMAGE,
    DYNAMIC_APP_UI_CMD_SET_STYLE,
    DYNAMIC_APP_UI_CMD_SET_IMAGE_SRC,
    DYNAMIC_APP_UI_CMD_ATTACH_ROOT_LISTENER,   /* 在 root 上挂一个总 cb */
    DYNAMIC_APP_UI_CMD_DESTROY,                /* 删除单个 obj 并释放 registry slot */

    /* P1.5 新增：通用 UI 能力 ----------------------------------------------- */
    DYNAMIC_APP_UI_CMD_SHOW_MODAL,             /* 弹模态卡（title + body + 2 actions） */
    DYNAMIC_APP_UI_CMD_TOAST,                  /* 屏底 toast，dur_ms 后自动消失 */
    DYNAMIC_APP_UI_CMD_FADE_IN,                /* 给已存在的对象做淡入动画 */
    DYNAMIC_APP_UI_CMD_FADE_OUT_DESTROY,       /* 淡出 + 销毁（toast 内部用） */

    /* P2 新增：sys.canvas.* —— 像素级绘图 ----------------------------------- */
    DYNAMIC_APP_UI_CMD_CREATE_CANVAS,          /* 240×320 RGB565 buffer in PSRAM */
    DYNAMIC_APP_UI_CMD_CANVAS_FILL,            /* 整张填色 */
    DYNAMIC_APP_UI_CMD_CANVAS_PIXEL,           /* 单像素 */
    DYNAMIC_APP_UI_CMD_CANVAS_LINE,            /* Bresenham 线，含粗细 */
    DYNAMIC_APP_UI_CMD_CANVAS_SAVE,            /* dump buffer → data/<rel>.bin */
    DYNAMIC_APP_UI_CMD_CANVAS_LOAD,            /* data/<rel>.bin → buffer */
} dynamic_app_ui_cmd_type_t;

/*
 * 样式 key：约定与 JS 侧 sys.style.* 数值常量对齐（dynamic_app.c 里绑定）。
 * a/b/c/d 字段含义随 key 变化（详见 dynamic_app_ui.c drain 实现）。
 */
typedef enum {
    DYNAMIC_APP_STYLE_BG_COLOR = 1,    /* a = 0xRRGGBB */
    DYNAMIC_APP_STYLE_TEXT_COLOR,      /* a = 0xRRGGBB */
    DYNAMIC_APP_STYLE_RADIUS,          /* a = px */
    DYNAMIC_APP_STYLE_SIZE,            /* a = w, b = h；负数取 abs 当百分比 */
    DYNAMIC_APP_STYLE_ALIGN,           /* a = align id, b = x, c = y */
    DYNAMIC_APP_STYLE_PAD,              /* a/b/c/d = left/top/right/bottom */
    DYNAMIC_APP_STYLE_BORDER_BOTTOM,   /* a = 0xRRGGBB */
    DYNAMIC_APP_STYLE_FLEX,            /* a = 0(column) / 1(row) */
    DYNAMIC_APP_STYLE_FONT,            /* a = 0(text)/1(title)/2(huge)/3(icon24)/4(icon36)/5(num_m) */
    DYNAMIC_APP_STYLE_SHADOW,          /* a = 0xRRGGBB, b = width(px), c = ofs_y(px) */
    DYNAMIC_APP_STYLE_GAP,             /* a = row_pad(px), b = col_pad(px) */
    DYNAMIC_APP_STYLE_SCROLLABLE,      /* a = 0(关) / 1(开)，默认关 */

    /* P1 新增 ----------------------------------------------------------- */
    DYNAMIC_APP_STYLE_OPA,             /* a = 0..255 整体不透明度（bg + border + text 一起） */
    DYNAMIC_APP_STYLE_BG_OPA,          /* a = 0..255 仅背景不透明度 */
    DYNAMIC_APP_STYLE_FLEX_GROW,       /* a = grow 系数（>=0），让该子项在 flex 容器里弹性伸展 */
    DYNAMIC_APP_STYLE_TEXT_ALIGN,      /* a = 0(left)/1(center)/2(right) */
    DYNAMIC_APP_STYLE_LONG_MODE,       /* label 长文本模式 a=0(wrap)/1(dot)/2(scroll)/3(clip) */
    DYNAMIC_APP_STYLE_ROTATION,        /* a = 0.1°（LVGL 约定 0..3600），b/c = pivot xy */
    DYNAMIC_APP_STYLE_FLEX_ALIGN,      /* a/b/c = main/cross/track 的 align id（0..5） */
    DYNAMIC_APP_STYLE_BORDER,          /* a=color, b=width(px), c=side bitmap, d=opa(0..255) */
    DYNAMIC_APP_STYLE_PRESSED_BG,      /* 按下态背景色：a=0xRRGGBB，b=opa（0..255） */
    DYNAMIC_APP_STYLE_HIDDEN,          /* a = 0(显示) / 1(隐藏) —— LV_OBJ_FLAG_HIDDEN */
} dynamic_app_style_key_t;

typedef struct {
    dynamic_app_ui_cmd_type_t type;
    char id[DYNAMIC_APP_UI_ID_MAX_LEN];
    union {
        char text[DYNAMIC_APP_UI_TEXT_MAX_LEN];          /* SET_TEXT */
        char parent_id[DYNAMIC_APP_UI_ID_MAX_LEN];        /* CREATE_LABEL/PANEL/BUTTON */
        struct {
            char parent_id[DYNAMIC_APP_UI_ID_MAX_LEN];
            char src[DYNAMIC_APP_UI_SRC_MAX_LEN];
        } image_create;                                   /* CREATE_IMAGE */
        char src[DYNAMIC_APP_UI_SRC_MAX_LEN];             /* SET_IMAGE_SRC */
        struct {
            int32_t key;
            int32_t a, b, c, d;
        } style;                                          /* SET_STYLE */
        struct {
            uint32_t modal_id;                            /* JS 侧分配的 modal 标识（事件回传时用） */
            char     title[64];
            char     body[160];
            char     action0[16];                         /* 空字符串表示无 action0 */
            char     action1[16];
        } modal;                                          /* SHOW_MODAL */
        struct {
            char     text[96];
            uint16_t dur_ms;                              /* 0 → 1500ms 默认 */
        } toast;                                          /* TOAST */
        struct {
            uint16_t delay_ms;
        } fade;                                           /* FADE_IN（id 字段标记目标） */

        /* P2 canvas ---------------------------------------------------- */
        struct {
            char parent_id[DYNAMIC_APP_UI_ID_MAX_LEN];
            uint16_t w;        /* 0 → 默认 240 */
            uint16_t h;        /* 0 → 默认 320 */
        } canvas_create;                                  /* CREATE_CANVAS */
        struct {
            uint32_t color;                               /* 0xRRGGBB */
        } canvas_fill;                                    /* CANVAS_FILL */
        struct {
            int16_t  x, y;
            uint32_t color;
        } canvas_pixel;                                   /* CANVAS_PIXEL */
        struct {
            int16_t  x0, y0, x1, y1;
            uint32_t color;
            uint8_t  thickness;                           /* 1..6 */
        } canvas_line;                                    /* CANVAS_LINE */
        struct {
            char relpath[DYNAMIC_APP_USER_PATH_MAX];
        } canvas_io;                                      /* CANVAS_SAVE / CANVAS_LOAD */
    } u;
} dynamic_app_ui_command_t;

/* 反向事件（UI → Script）的类型。
 * 与 JS 侧 dispatcher 的 type 参数一一对应，数值不能改。 */
typedef enum {
    DYNAMIC_APP_UI_EV_CLICK      = 1,
    DYNAMIC_APP_UI_EV_PRESS      = 2,
    DYNAMIC_APP_UI_EV_DRAG       = 3,
    DYNAMIC_APP_UI_EV_RELEASE    = 4,
    DYNAMIC_APP_UI_EV_LONG_PRESS = 5,
    DYNAMIC_APP_UI_EV_MODAL      = 6,    /* 模态结果：dx = action 索引（0/1），-1 = 取消（点遮罩/下滑） */
} dynamic_app_ui_event_type_t;

typedef struct {
    uint8_t type;                                  /* dynamic_app_ui_event_type_t */
    int16_t dx, dy;                                /* 仅 DRAG 用，其它事件为 0 */
    char    node_id[DYNAMIC_APP_UI_ID_MAX_LEN];    /* PRESS 时为按下对象，
                                                      DRAG/RELEASE 时为按下时记录的对象，
                                                      CLICK 时为 LVGL 报告的 target，
                                                      MODAL 时为脚本传入的 modal_id 字符串形式 */
} dynamic_app_ui_event_t;

/* ---------------- 生命周期 ---------------- */

esp_err_t dynamic_app_ui_init(void);

void dynamic_app_ui_set_root(lv_obj_t *root);
void dynamic_app_ui_unregister_all(void);
void dynamic_app_ui_drain(int max_count);

/* 由上层（page）在进入 Dynamic App 页面前注入字体指针。任意指针为 NULL 时
 * 该字体对应的 setStyle(FONT, x) 将回退到 LVGL 默认字体。
 *   text/title/huge ：CJK 正文/标题/巨号
 *   icon24/icon36   ：Material Symbols 矢量图标字体
 *   num_m           ：中号数字（lv_font_montserrat_24 之类） */
void dynamic_app_ui_set_fonts(const lv_font_t *text,
                              const lv_font_t *title,
                              const lv_font_t *huge,
                              const lv_font_t *icon24,
                              const lv_font_t *icon36,
                              const lv_font_t *num_m);

/* ---------------- Script -> UI ---------------- */

bool dynamic_app_ui_enqueue_set_text(const char *id, size_t id_len,
                                     const char *text, size_t text_len);

/* parent_id 可为 NULL（落到 root）；len 同步 */
bool dynamic_app_ui_enqueue_create_label(const char *id, size_t id_len,
                                         const char *parent_id, size_t parent_len);
bool dynamic_app_ui_enqueue_create_panel(const char *id, size_t id_len,
                                         const char *parent_id, size_t parent_len);
bool dynamic_app_ui_enqueue_create_button(const char *id, size_t id_len,
                                          const char *parent_id, size_t parent_len);

/* 创建图片节点：src 是相对当前 app 的 assets/ 子目录的文件名（如 "fish.bin"）。
 * src 为 NULL/空 → 创建空 image，后续靠 setImageSrc 填。 */
bool dynamic_app_ui_enqueue_create_image(const char *id, size_t id_len,
                                         const char *parent_id, size_t parent_len,
                                         const char *src, size_t src_len);

/* 切换已有 image 节点的资源；src 同 createImage 语义。 */
bool dynamic_app_ui_enqueue_set_image_src(const char *id, size_t id_len,
                                          const char *src, size_t src_len);

bool dynamic_app_ui_enqueue_set_style(const char *id, size_t id_len,
                                      dynamic_app_style_key_t key,
                                      int32_t a, int32_t b, int32_t c, int32_t d);

/* 把指定 id 的对象升级为"根监听器"——在它上面挂一个 LVGL cb，
 * 子对象冒泡上来的 click 都从这里捕获，事件入队时携带"被点中对象"的 id。
 * 通常脚本会对最外层 root container 调一次。 */
bool dynamic_app_ui_enqueue_attach_root_listener(const char *id, size_t id_len);

/* 注册"build ready"回调：drain 处理完一条 ATTACH_ROOT_LISTENER 命令后会触发一次
 * 该回调，然后自动清空（一次性，不会重复触发）。
 *
 * 用途：宿主页 (page_dynamic_app) 用它感知"脚本已经把对象树搭完了"，
 * 然后把后台 prepared screen 提交给 page_router，实现"瞬切"效果。
 *
 * 在 UI 任务上下文调用 cb，cb 内部调 LVGL/page_router 是安全的。
 * 传入 NULL 等同于取消注册（用于宿主页 cancel prepare 流程）。 */
typedef void (*dynamic_app_ui_ready_cb_t)(void *user_data);
void dynamic_app_ui_set_ready_cb(dynamic_app_ui_ready_cb_t cb, void *user_data);

/* 销毁单个对象：lv_obj_del(obj) + 释放 registry slot。
 * 调用方（JS VDOM.destroy）负责自底向上递归，C 侧只处理一个 id。
 * 防御：若 JS 没递归就直接 destroy 父对象，LVGL 会级联删子对象，
 *       此时遗留 slot 会在 drain 的 lv_obj_is_valid 检查里被清。 */
bool dynamic_app_ui_enqueue_destroy(const char *id, size_t id_len);

/* 弹模态卡。
 *   modal_id : JS 侧分配的标识（无符号整数转字符串），按钮按下时通过
 *              EV_MODAL 事件回传，dx = action 索引（0/1）；点遮罩/下滑 dx = -1。
 *   action0/action1 长度为 0 表示无该按钮（C 端不调 add_action）。 */
bool dynamic_app_ui_enqueue_show_modal(uint32_t modal_id,
                                       const char *title, size_t title_len,
                                       const char *body,  size_t body_len,
                                       const char *action0, size_t a0_len,
                                       const char *action1, size_t a1_len);

/* 屏底 toast：text 在屏底显示 dur_ms 后自动淡出销毁。
 * dur_ms = 0 → 1500ms 默认。 */
bool dynamic_app_ui_enqueue_toast(const char *text, size_t text_len, uint16_t dur_ms);

/* 给已存在的对象做淡入动画（透明 0 → 全显，可选 delay）。 */
bool dynamic_app_ui_enqueue_fade_in(const char *id, size_t id_len, uint16_t delay_ms);

/* ---- P2 canvas ---------------------------------------------------------- */

/* 创建 240x320 RGB565 canvas（buffer 由本层在 PSRAM 分配并挂在 registry slot 的
 * aux 字段上；unregister/destroy 时自动 free）。返回 false = 队列满 / 参数非法。 */
bool dynamic_app_ui_enqueue_create_canvas(const char *id, size_t id_len,
                                           const char *parent_id, size_t parent_len,
                                           uint16_t w, uint16_t h);
bool dynamic_app_ui_enqueue_canvas_fill(const char *id, size_t id_len,
                                         uint32_t color);
bool dynamic_app_ui_enqueue_canvas_pixel(const char *id, size_t id_len,
                                          int16_t x, int16_t y, uint32_t color);
bool dynamic_app_ui_enqueue_canvas_line(const char *id, size_t id_len,
                                         int16_t x0, int16_t y0,
                                         int16_t x1, int16_t y1,
                                         uint32_t color, uint8_t thickness);
bool dynamic_app_ui_enqueue_canvas_save(const char *id, size_t id_len,
                                         const char *relpath, size_t rp_len);
bool dynamic_app_ui_enqueue_canvas_load(const char *id, size_t id_len,
                                         const char *relpath, size_t rp_len);

/* ---------------- UI -> Script ---------------- */

bool dynamic_app_ui_pop_event(dynamic_app_ui_event_t *out);
void dynamic_app_ui_clear_event_queue(void);

#ifdef __cplusplus
}
#endif
