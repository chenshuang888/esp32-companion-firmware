#include "ui_widgets.h"
#include "app_fonts.h"

/* ============================================================================
 * 内部样式（懒加载，第一次调用时初始化一次）
 * ========================================================================= */

static struct {
    bool inited;
    lv_style_t card;
    lv_style_t card_accent;
    lv_style_t row;
    lv_style_t row_no_div;
    lv_style_t list_row;
    lv_style_t list_row_pressed;
    lv_style_t icon_btn;
    lv_style_t icon_btn_pressed;
} S;

static void styles_init_once(void)
{
    if (S.inited) return;
    S.inited = true;

    /* card —— 白底圆角，1px 浅描边，无阴影（嵌入式性能友好）*/
    lv_style_init(&S.card);
    lv_style_set_bg_color   (&S.card, UI_C_PANEL);
    lv_style_set_bg_opa     (&S.card, LV_OPA_COVER);
    lv_style_set_radius     (&S.card, UI_R_LG);
    lv_style_set_border_width(&S.card, 1);
    lv_style_set_border_color(&S.card, UI_C_BORDER);
    lv_style_set_border_opa (&S.card, LV_OPA_50);
    lv_style_set_shadow_width(&S.card, 0);
    lv_style_set_pad_all    (&S.card, UI_SP_MD);

    /* card_accent —— 同 card，但 accent 色描边更明显 */
    lv_style_init(&S.card_accent);
    lv_style_set_bg_color   (&S.card_accent, UI_C_PANEL);
    lv_style_set_bg_opa     (&S.card_accent, LV_OPA_COVER);
    lv_style_set_radius     (&S.card_accent, UI_R_LG);
    lv_style_set_border_width(&S.card_accent, 1);
    lv_style_set_border_color(&S.card_accent, UI_C_ACCENT);
    lv_style_set_border_opa (&S.card_accent, LV_OPA_COVER);
    lv_style_set_shadow_width(&S.card_accent, 0);
    lv_style_set_pad_all    (&S.card_accent, UI_SP_MD);

    /* row —— 一行，底部 1px 分隔，水平 flex 两端对齐 */
    lv_style_init(&S.row);
    lv_style_set_bg_opa     (&S.row, LV_OPA_TRANSP);
    lv_style_set_radius     (&S.row, 0);
    lv_style_set_border_width(&S.row, 1);
    lv_style_set_border_side (&S.row, LV_BORDER_SIDE_BOTTOM);
    lv_style_set_border_color(&S.row, UI_C_BORDER);
    lv_style_set_border_opa (&S.row, LV_OPA_40);
    lv_style_set_shadow_width(&S.row, 0);
    lv_style_set_pad_hor    (&S.row, 0);
    lv_style_set_pad_ver    (&S.row, UI_SP_SM);

    lv_style_init(&S.row_no_div);
    lv_style_set_bg_opa     (&S.row_no_div, LV_OPA_TRANSP);
    lv_style_set_radius     (&S.row_no_div, 0);
    lv_style_set_border_width(&S.row_no_div, 0);
    lv_style_set_shadow_width(&S.row_no_div, 0);
    lv_style_set_pad_hor    (&S.row_no_div, 0);
    lv_style_set_pad_ver    (&S.row_no_div, UI_SP_SM);

    /* list_row —— 设置列表行：48px 高，水平 padding，底部 1px 分隔；
     * 同 row 但带按下高亮（用作 button） */
    lv_style_init(&S.list_row);
    lv_style_set_bg_opa     (&S.list_row, LV_OPA_TRANSP);
    lv_style_set_radius     (&S.list_row, 0);
    lv_style_set_border_width(&S.list_row, 1);
    lv_style_set_border_side (&S.list_row, LV_BORDER_SIDE_BOTTOM);
    lv_style_set_border_color(&S.list_row, UI_C_BORDER);
    lv_style_set_border_opa (&S.list_row, LV_OPA_40);
    lv_style_set_shadow_width(&S.list_row, 0);
    lv_style_set_pad_hor    (&S.list_row, UI_SP_MD);
    lv_style_set_pad_ver    (&S.list_row, 0);

    lv_style_init(&S.list_row_pressed);
    lv_style_set_bg_color   (&S.list_row_pressed, UI_C_PANEL_HI);
    lv_style_set_bg_opa     (&S.list_row_pressed, LV_OPA_COVER);

    /* icon_btn —— 透明按钮，按下变浅蓝底（accent 弱化） */
    lv_style_init(&S.icon_btn);
    lv_style_set_bg_opa     (&S.icon_btn, LV_OPA_TRANSP);
    lv_style_set_border_width(&S.icon_btn, 0);
    lv_style_set_shadow_width(&S.icon_btn, 0);
    lv_style_set_radius     (&S.icon_btn, UI_R_MD);
    lv_style_set_text_color (&S.icon_btn, UI_C_ACCENT);
    lv_style_set_pad_all    (&S.icon_btn, UI_SP_XS);

    lv_style_init(&S.icon_btn_pressed);
    lv_style_set_bg_color   (&S.icon_btn_pressed, UI_C_ACCENT);
    lv_style_set_bg_opa     (&S.icon_btn_pressed, LV_OPA_20);
}

/* ============================================================================
 * 公开 API
 * ========================================================================= */

void ui_screen_setup(lv_obj_t *screen)
{
    lv_obj_set_style_bg_color(screen, UI_C_BG, 0);
    lv_obj_set_style_bg_opa  (screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all (screen, 0, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t *ui_card(lv_obj_t *parent)
{
    styles_init_once();
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_add_style(o, &S.card, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

lv_obj_t *ui_card_accent(lv_obj_t *parent)
{
    styles_init_once();
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_add_style(o, &S.card_accent, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

lv_obj_t *ui_kv_row(lv_obj_t *parent, const char *key, const char *value,
                     lv_obj_t **out_value, bool with_divider)
{
    styles_init_once();
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_add_style(row, with_divider ? &S.row : &S.row_no_div, 0);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    /* flex: 左右两端 */
    lv_obj_set_flex_flow (row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *k = lv_label_create(row);
    lv_label_set_text(k, key);
    lv_obj_set_style_text_color(k, UI_C_TEXT_MUTED, 0);
    lv_obj_set_style_text_font (k, UI_F_BODY, 0);

    lv_obj_t *v = lv_label_create(row);
    lv_label_set_text(v, value ? value : "--");
    lv_obj_set_style_text_color(v, UI_C_TEXT, 0);
    lv_obj_set_style_text_font (v, UI_F_BODY, 0);

    if (out_value) *out_value = v;
    return row;
}

lv_obj_t *ui_icon_btn(lv_obj_t *parent, const char *symbol, int w, int h)
{
    styles_init_once();
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_add_style(btn, &S.icon_btn, 0);
    lv_obj_add_style(btn, &S.icon_btn_pressed, LV_STATE_PRESSED);
    lv_obj_set_size(btn, w > 0 ? w : 36, h > 0 ? h : 30);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, symbol);
    lv_obj_set_style_text_font(lbl, UI_F_ICON_S, 0);
    lv_obj_set_style_text_color(lbl, UI_C_ACCENT, 0);
    lv_obj_center(lbl);

    return btn;
}

/* ---- 列表行 ----
 * 结构（水平 flex）：
 *   [icon 24px] [label 占满] [value(可选)] [chevron 8px]
 *
 * 整行作为 button：底色透明，按下变 panel_hi。底部 1px 分隔线（最后一行
 * 由调用方决定是否手动 set border_width 0）。
 *
 * 内部结构能撑出 48px 行高（icon 用 ICONS_24 字体 = 24px，加上下 padding）
 */
lv_obj_t *ui_list_row(lv_obj_t *parent,
                       const char *icon, const char *label, const char *value,
                       lv_color_t icon_color, lv_obj_t **out_value)
{
    styles_init_once();

    lv_obj_t *row = lv_btn_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_add_style(row, &S.list_row, 0);
    lv_obj_add_style(row, &S.list_row_pressed, LV_STATE_PRESSED);
    lv_obj_set_size(row, lv_pct(100), 48);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_flex_flow (row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, UI_SP_MD, 0);

    /* 图标 —— 24px Material Symbols，固定宽 24 居中 */
    lv_obj_t *ic = lv_label_create(row);
    lv_label_set_text(ic, icon ? icon : "");
    lv_obj_set_style_text_font (ic, APP_FONT_ICONS_24, 0);
    lv_obj_set_style_text_color(ic, icon_color, 0);
    lv_obj_set_width(ic, 24);
    lv_obj_set_style_text_align(ic, LV_TEXT_ALIGN_CENTER, 0);

    /* label —— 占满中段 */
    lv_obj_t *lb = lv_label_create(row);
    lv_label_set_text(lb, label ? label : "");
    lv_obj_set_style_text_color(lb, UI_C_TEXT, 0);
    lv_obj_set_style_text_font (lb, UI_F_BODY, 0);
    lv_obj_set_flex_grow(lb, 1);

    /* value —— 可选 */
    lv_obj_t *v = NULL;
    if (value) {
        v = lv_label_create(row);
        lv_label_set_text(v, value);
        lv_obj_set_style_text_color(v, UI_C_TEXT_MUTED, 0);
        lv_obj_set_style_text_font (v, UI_F_BODY, 0);
    }

    /* chevron —— 用 Material 图标 24 字体里的 chevron_right */
    lv_obj_t *ch = lv_label_create(row);
    lv_label_set_text(ch, ICON_CHEVRON_RIGHT);
    lv_obj_set_style_text_font (ch, APP_FONT_ICONS_24, 0);
    lv_obj_set_style_text_color(ch, UI_C_TEXT_MUTED, 0);

    if (out_value) *out_value = v;
    return row;
}
