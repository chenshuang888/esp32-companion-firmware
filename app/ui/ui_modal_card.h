#pragma once

#include "lvgl.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * ui_modal_card —— 通用模态层（遮罩 + 居中卡片）
 *
 * 设计：
 *   - 全屏遮罩（半透明黑），点击遮罩或下滑关闭
 *   - 居中卡片自适应高度（最大 270px，超出内部纵向滚动）
 *   - 卡片由调用方填充内容（content 容器是纵向 flex，每个元素自动堆叠）
 *   - 可选底部双按钮（"删除" / "关闭" 等）
 *
 * 用法：
 *   ui_modal_card_t *m = ui_modal_card_create();
 *   lv_obj_t *content = ui_modal_card_content(m);
 *   // 往 content 里加 label/icon 等任意 LVGL 对象（自动纵向堆叠）
 *   ui_modal_card_add_action(m, "删除", on_del_clicked, NULL);
 *   ui_modal_card_add_action(m, "关闭", NULL, NULL);   // NULL cb = 仅关闭
 *   ui_modal_card_show(m);
 *
 * 关闭时机：
 *   - 用户点击遮罩
 *   - 用户从卡片下边界向下滑（LV_DIR_BOTTOM）
 *   - 调用 ui_modal_card_close(m)
 *   - action 按钮回调里返回（按钮触发后自动关闭）
 *
 * 关闭后 ui_modal_card_t 句柄失效，无需手动 destroy。
 * ========================================================================= */

typedef struct ui_modal_card ui_modal_card_t;

/* action 按钮回调；user_data 由 add_action 传入。回调返回后模态自动关闭。*/
typedef void (*ui_modal_action_cb_t)(void *user_data);

/* 创建模态层（不立即显示，先填充内容） */
ui_modal_card_t *ui_modal_card_create(void);

/* 拿到内部 content 容器，往里 add LVGL 对象。
 * content 是纵向 flex，宽度 200px，高度自适应；超过最大高时整张卡内部滚动。 */
lv_obj_t *ui_modal_card_content(ui_modal_card_t *m);

/* 添加底部按钮。最多 2 个。
 * label  : 按钮文字
 * cb     : 点击回调（NULL = 仅关闭模态）
 * user_data : 透传到 cb */
void ui_modal_card_add_action(ui_modal_card_t *m, const char *label,
                               ui_modal_action_cb_t cb, void *user_data);

/* 显示（带淡入动画） */
void ui_modal_card_show(ui_modal_card_t *m);

/* 主动关闭（一般不需要，按钮 / 手势 / 点遮罩都会自动关闭） */
void ui_modal_card_close(ui_modal_card_t *m);

#ifdef __cplusplus
}
#endif
