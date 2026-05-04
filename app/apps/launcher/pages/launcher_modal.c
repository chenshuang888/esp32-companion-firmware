/* ============================================================================
 * page_menu_modal.c —— 菜单页通用确认对话框
 *
 * 当前唯一用途：动态 app 长按删除时的二次确认。
 *
 * 实现要点：
 *   - 挂 lv_layer_top()：LVGL 顶层在所有 screen 之上，切页也不会被销毁。
 *     所以本模块要小心：用户点了 Delete/Cancel 之外，没办法被外部强制清掉。
 *     退化兜底：show 时若发现旧 modal 还在 → 先销毁
 *   - 标题用文件名拷贝（避免外部 free 后悬空）
 *   - on_confirm 在销毁 modal 之后再调用，避免回调里再开 modal 被旧 modal 顶掉
 * ========================================================================= */

#include "launcher_modal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "lvgl.h"
#include "app_fonts.h"

static const char *TAG = "launcher_modal";

/* 配色与 page_menu 对齐 */
#define COLOR_BG         0x1E1B2E
#define COLOR_CARD       0x2D2640
#define COLOR_CARD_ALT   0x3A3354
#define COLOR_ACCENT     0x06B6D4
#define COLOR_TEXT       0xF1ECFF
#define COLOR_MUTED      0x9B94B5
#define COLOR_DANGER     0xEF4444

/* 全屏状态：同一时刻最多一个 modal 活跃 */
typedef struct {
    lv_obj_t *overlay;     /* 半透明遮罩（顶层 root） */
    char     *title_copy;  /* 拷贝的 app_name，destroy 时 free */
    void    (*on_confirm)(void *ud);
    void     *user_data;
} modal_state_t;

static modal_state_t s_modal = {0};

static void modal_destroy(void)
{
    if (s_modal.overlay) {
        lv_obj_del(s_modal.overlay);
        s_modal.overlay = NULL;
    }
    if (s_modal.title_copy) {
        free(s_modal.title_copy);
        s_modal.title_copy = NULL;
    }
    s_modal.on_confirm = NULL;
    s_modal.user_data = NULL;
}

static void on_cancel_clicked(lv_event_t *e)
{
    (void)e;
    modal_destroy();
}

static void on_delete_clicked(lv_event_t *e)
{
    (void)e;
    /* 取出回调 + ud 后立刻销毁 modal，再调回调。
     * 这样回调里若再开新 modal 也不会和旧的状态打架。 */
    void (*cb)(void *) = s_modal.on_confirm;
    void *ud = s_modal.user_data;
    modal_destroy();
    if (cb) cb(ud);
}

void launcher_modal_show_delete_confirm(const char *app_name,
                                        void (*on_confirm)(void *ud),
                                        void *user_data)
{
    if (!app_name) return;

    /* 旧 modal 兜底清理 */
    if (s_modal.overlay) {
        ESP_LOGW(TAG, "show: previous modal still alive, destroying");
        modal_destroy();
    }

    s_modal.title_copy = strdup(app_name);
    if (!s_modal.title_copy) return;
    s_modal.on_confirm = on_confirm;
    s_modal.user_data  = user_data;

    /* —— 半透明遮罩 —— */
    lv_obj_t *overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_70, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    /* 阻止事件穿透 */
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    s_modal.overlay = overlay;

    /* —— 中央卡片 —— */
    lv_obj_t *card = lv_obj_create(overlay);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 200, 130);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* 标题 */
    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Delete app?");
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_font(title, APP_FONT_TITLE, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    /* 副标题 = app 名（限长截断） */
    char buf[32];
    snprintf(buf, sizeof(buf), "\"%s\"", s_modal.title_copy);
    lv_obj_t *sub = lv_label_create(card);
    lv_label_set_text(sub, buf);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
    lv_obj_set_width(sub, 170);
    lv_obj_set_style_text_color(sub, lv_color_hex(COLOR_MUTED), 0);
    lv_obj_set_style_text_font(sub, APP_FONT_TEXT, 0);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 30);

    /* —— Cancel 按钮 —— */
    lv_obj_t *cancel = lv_btn_create(card);
    lv_obj_remove_style_all(cancel);
    lv_obj_set_size(cancel, 80, 36);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(COLOR_CARD_ALT), 0);
    lv_obj_set_style_bg_opa(cancel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(cancel, 8, 0);
    lv_obj_t *cancel_lbl = lv_label_create(cancel);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_set_style_text_color(cancel_lbl, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_font(cancel_lbl, APP_FONT_TEXT, 0);
    lv_obj_center(cancel_lbl);
    lv_obj_add_event_cb(cancel, on_cancel_clicked, LV_EVENT_CLICKED, NULL);

    /* —— Delete 按钮（红） —— */
    lv_obj_t *del = lv_btn_create(card);
    lv_obj_remove_style_all(del);
    lv_obj_set_size(del, 80, 36);
    lv_obj_align(del, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(del, lv_color_hex(COLOR_DANGER), 0);
    lv_obj_set_style_bg_opa(del, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(del, 8, 0);
    lv_obj_t *del_lbl = lv_label_create(del);
    lv_label_set_text(del_lbl, "Delete");
    lv_obj_set_style_text_color(del_lbl, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_font(del_lbl, APP_FONT_TEXT, 0);
    lv_obj_center(del_lbl);
    lv_obj_add_event_cb(del, on_delete_clicked, LV_EVENT_CLICKED, NULL);
}

void launcher_modal_dismiss(void)
{
    if (s_modal.overlay) modal_destroy();
}
