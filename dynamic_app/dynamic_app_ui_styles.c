/* ============================================================================
 * dynamic_app_ui_styles.c —— UI 侧"样式分发层"
 *
 * 职责：
 *   把 SET_STYLE 命令翻译成对应的 LVGL setter 调用。
 *
 *   设计上脚本通过统一的 sys.ui.setStyle(id, key, a, b, c, d) 调样式：
 *     - key 是 enum（BG_COLOR / SIZE / ALIGN ...）
 *     - a/b/c/d 是 4 个整数槽，含义随 key 变化
 *
 *   本文件就是"key → LVGL API"的大字典。每加一个新样式 key 只需要：
 *     1. 在 dynamic_app_ui.h 的 dynamic_app_style_key_t 加 enum
 *     2. 在 dynamic_app.c 的 bind 里挂 sys.style.XXX 常量
 *     3. 在本文件 apply_style 加一个 case
 *
 * 不持有状态：
 *   纯函数风格，依赖外部传入的 obj + cmd。registry 查找在调用方完成。
 * ========================================================================= */

#include "dynamic_app_ui_internal.h"

#include "esp_log.h"

static const char *TAG = "dynamic_app_ui_sty";

/* ============================================================================
 * §1. JS 端 enum 与 LVGL 常量的映射表
 *
 *   sys.align.TOP_LEFT  = 0
 *   sys.align.TOP_MID   = 1
 *   ...
 *   下标 → lv_align_t。绝对不能改顺序，否则 JS 侧的常量会错位。
 * ========================================================================= */

static const lv_align_t k_align_map[] = {
    LV_ALIGN_TOP_LEFT,    /* 0 */
    LV_ALIGN_TOP_MID,     /* 1 */
    LV_ALIGN_TOP_RIGHT,   /* 2 */
    LV_ALIGN_LEFT_MID,    /* 3 */
    LV_ALIGN_CENTER,      /* 4 */
    LV_ALIGN_RIGHT_MID,   /* 5 */
    LV_ALIGN_BOTTOM_LEFT, /* 6 */
    LV_ALIGN_BOTTOM_MID,  /* 7 */
    LV_ALIGN_BOTTOM_RIGHT,/* 8 */
};
#define K_ALIGN_MAP_LEN ((int)(sizeof(k_align_map) / sizeof(k_align_map[0])))

/* ============================================================================
 * §2. 数值约定 helper
 * ========================================================================= */

/* SIZE 字段编码：
 *   v >= 0     : 像素
 *   -100..-1   : lv_pct(-v)，例如 -100 表示 100%
 *   -32768     : LV_SIZE_CONTENT（按内容自适应）—— 远离百分比合法区间的 sentinel
 *
 *   sentinel 在 prelude.js 暴露为 sys.size.CONTENT。脚本侧用名字而不是裸魔法值。
 */
static lv_coord_t resolve_size(int32_t v)
{
    if (v == -32768) return LV_SIZE_CONTENT;
    if (v < 0)       return lv_pct(-v);
    return (lv_coord_t)v;
}

/* FONT 槽位 → 实际字体指针。
 *   字体由 page 通过 dynamic_app_ui_set_fonts() 注入；未注入时返回 NULL。
 *   调用方 apply_style 必须自行做 NULL 检查。 */
static const lv_font_t *resolve_font(int32_t a)
{
    switch (a) {
        case 0: return s_font_text;
        case 1: return s_font_title;
        case 2: return s_font_huge;
        case 3: return s_font_icon24;
        case 4: return s_font_icon36;
        case 5: return s_font_num_m;
        default: return s_font_text;
    }
}

/* FLEX_ALIGN 子参数 → lv_flex_align_t */
static lv_flex_align_t resolve_flex_align(int32_t v)
{
    switch (v) {
        case 0: return LV_FLEX_ALIGN_START;
        case 1: return LV_FLEX_ALIGN_END;
        case 2: return LV_FLEX_ALIGN_CENTER;
        case 3: return LV_FLEX_ALIGN_SPACE_EVENLY;
        case 4: return LV_FLEX_ALIGN_SPACE_AROUND;
        case 5: return LV_FLEX_ALIGN_SPACE_BETWEEN;
        default: return LV_FLEX_ALIGN_START;
    }
}

/* ============================================================================
 * §3. apply_style —— 主分发器
 *
 *   每个 case 一个样式 key，调对应的 lv_obj_set_style_*。
 *   字段含义参见 dynamic_app_ui.h 的 dynamic_app_style_key_t 注释。
 * ========================================================================= */

void apply_style(lv_obj_t *obj, const dynamic_app_ui_command_t *cmd)
{
    int32_t a = cmd->u.style.a;
    int32_t b = cmd->u.style.b;
    int32_t c = cmd->u.style.c;
    int32_t d = cmd->u.style.d;

    switch ((dynamic_app_style_key_t)cmd->u.style.key) {

        case DYNAMIC_APP_STYLE_BG_COLOR:
            /* a = 0xRRGGBB；同时把 bg_opa 设为 COVER 否则颜色看不见 */
            lv_obj_set_style_bg_color(obj, lv_color_hex((uint32_t)a), 0);
            lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
            break;

        case DYNAMIC_APP_STYLE_TEXT_COLOR:
            lv_obj_set_style_text_color(obj, lv_color_hex((uint32_t)a), 0);
            break;

        case DYNAMIC_APP_STYLE_RADIUS:
            lv_obj_set_style_radius(obj, (lv_coord_t)a, 0);
            break;

        case DYNAMIC_APP_STYLE_SIZE:
            /* a = 宽, b = 高，负值代表百分比 */
            lv_obj_set_size(obj, resolve_size(a), resolve_size(b));
            break;

        case DYNAMIC_APP_STYLE_ALIGN:
            /* a = align id, b = x 偏移, c = y 偏移 */
            if (a >= 0 && a < K_ALIGN_MAP_LEN) {
                lv_obj_align(obj, k_align_map[a], (lv_coord_t)b, (lv_coord_t)c);
            }
            break;

        case DYNAMIC_APP_STYLE_PAD:
            /* a/b/c/d = left/top/right/bottom */
            lv_obj_set_style_pad_left  (obj, (lv_coord_t)a, 0);
            lv_obj_set_style_pad_top   (obj, (lv_coord_t)b, 0);
            lv_obj_set_style_pad_right (obj, (lv_coord_t)c, 0);
            lv_obj_set_style_pad_bottom(obj, (lv_coord_t)d, 0);
            break;

        case DYNAMIC_APP_STYLE_BORDER_BOTTOM:
            /* a = 0xRRGGBB，仅画底边一条线（菜单页列表项分割线效果） */
            lv_obj_set_style_border_width(obj, 1, 0);
            lv_obj_set_style_border_side (obj, LV_BORDER_SIDE_BOTTOM, 0);
            lv_obj_set_style_border_color(obj, lv_color_hex((uint32_t)a), 0);
            break;

        case DYNAMIC_APP_STYLE_FLEX:
            /* a = 0(column) / 1(row)；同时设置一个合理的默认 flex_align */
            lv_obj_set_flex_flow(obj,
                a == 1 ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(obj,
                LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_row(obj, 0, 0);
            break;

        case DYNAMIC_APP_STYLE_FONT: {
            /* a = 0(text) / 1(title) / 2(huge)。字体未注入时静默跳过。 */
            const lv_font_t *f = resolve_font(a);
            if (f) lv_obj_set_style_text_font(obj, f, 0);
            break;
        }

        case DYNAMIC_APP_STYLE_SHADOW:
            /* a = 0xRRGGBB（阴影色）, b = 宽度(px), c = y 偏移(px) */
            lv_obj_set_style_shadow_color (obj, lv_color_hex((uint32_t)a), 0);
            lv_obj_set_style_shadow_width (obj, (lv_coord_t)b, 0);
            lv_obj_set_style_shadow_ofs_y (obj, (lv_coord_t)c, 0);
            lv_obj_set_style_shadow_opa   (obj, LV_OPA_50, 0);
            break;

        case DYNAMIC_APP_STYLE_GAP:
            /* a = 行间距(px), b = 列间距(px)。flex 容器内子项的间距。 */
            lv_obj_set_style_pad_row    (obj, (lv_coord_t)a, 0);
            lv_obj_set_style_pad_column (obj, (lv_coord_t)b, 0);
            break;

        case DYNAMIC_APP_STYLE_SCROLLABLE:
            /* a = 0(关) / 1(开)。默认 panel 已关；list 需要滚动时业务自己开。 */
            if (a) lv_obj_add_flag   (obj, LV_OBJ_FLAG_SCROLLABLE);
            else   lv_obj_clear_flag (obj, LV_OBJ_FLAG_SCROLLABLE);
            break;

        case DYNAMIC_APP_STYLE_OPA: {
            /* a = 0..255，整体不透明度（背景 + 边框 + 文字 + 图片一起）。
             * LVGL 没有"主"opa，挨个 setter 一致最稳。 */
            lv_opa_t op = (lv_opa_t)(a < 0 ? 0 : (a > 255 ? 255 : a));
            lv_obj_set_style_opa(obj, op, 0);
            break;
        }

        case DYNAMIC_APP_STYLE_BG_OPA: {
            lv_opa_t op = (lv_opa_t)(a < 0 ? 0 : (a > 255 ? 255 : a));
            lv_obj_set_style_bg_opa(obj, op, 0);
            break;
        }

        case DYNAMIC_APP_STYLE_FLEX_GROW:
            lv_obj_set_flex_grow(obj, (uint8_t)(a < 0 ? 0 : a));
            break;

        case DYNAMIC_APP_STYLE_TEXT_ALIGN: {
            lv_text_align_t ta = LV_TEXT_ALIGN_LEFT;
            if      (a == 1) ta = LV_TEXT_ALIGN_CENTER;
            else if (a == 2) ta = LV_TEXT_ALIGN_RIGHT;
            lv_obj_set_style_text_align(obj, ta, 0);
            break;
        }

        case DYNAMIC_APP_STYLE_LONG_MODE: {
            /* 仅对 label 生效；其它对象直接跳过避免 LVGL warning */
            if (lv_obj_has_class(obj, &lv_label_class)) {
                lv_label_long_mode_t m = LV_LABEL_LONG_WRAP;
                if      (a == 1) m = LV_LABEL_LONG_DOT;
                else if (a == 2) m = LV_LABEL_LONG_SCROLL;
                else if (a == 3) m = LV_LABEL_LONG_CLIP;
                lv_label_set_long_mode(obj, m);
            }
            break;
        }

        case DYNAMIC_APP_STYLE_ROTATION: {
            /* 通用旋转（不限 image）。a = 0.1° 单位（LVGL 约定，0..3600）；
             * b/c = pivot 相对对象左上角的 px 偏移。 */
            lv_obj_set_style_transform_pivot_x(obj, (lv_coord_t)b, 0);
            lv_obj_set_style_transform_pivot_y(obj, (lv_coord_t)c, 0);
            lv_obj_set_style_transform_rotation(obj, (int32_t)a, 0);
            break;
        }

        case DYNAMIC_APP_STYLE_FLEX_ALIGN: {
            /* a = main, b = cross, c = track */
            lv_obj_set_flex_align(obj,
                                  resolve_flex_align(a),
                                  resolve_flex_align(b),
                                  resolve_flex_align(c));
            break;
        }

        case DYNAMIC_APP_STYLE_BORDER: {
            /* a = 0xRRGGBB color, b = width(px), c = side bitmap, d = opa(0..255)
             * side bitmap：bit0=top bit1=bottom bit2=left bit3=right bit4=full
             *              0 → 默认 FULL */
            lv_obj_set_style_border_color(obj, lv_color_hex((uint32_t)a), 0);
            lv_obj_set_style_border_width(obj, (lv_coord_t)(b < 0 ? 0 : b), 0);
            lv_border_side_t side = LV_BORDER_SIDE_FULL;
            if (c != 0) {
                int s = 0;
                if (c & 0x01) s |= LV_BORDER_SIDE_TOP;
                if (c & 0x02) s |= LV_BORDER_SIDE_BOTTOM;
                if (c & 0x04) s |= LV_BORDER_SIDE_LEFT;
                if (c & 0x08) s |= LV_BORDER_SIDE_RIGHT;
                if (c & 0x10) s = LV_BORDER_SIDE_FULL;
                if (s != 0) side = (lv_border_side_t)s;
            }
            lv_obj_set_style_border_side(obj, side, 0);
            int32_t op = (d <= 0) ? 255 : d;
            if (op > 255) op = 255;
            lv_obj_set_style_border_opa(obj, (lv_opa_t)op, 0);
            break;
        }

        case DYNAMIC_APP_STYLE_PRESSED_BG: {
            /* 按下态背景：a=color，b=opa（0=透明,255=不透明；0 时按 LV_OPA_30 默认） */
            lv_obj_set_style_bg_color(obj, lv_color_hex((uint32_t)a), LV_STATE_PRESSED);
            int32_t op = (b <= 0) ? LV_OPA_30 : b;
            if (op > 255) op = 255;
            lv_obj_set_style_bg_opa(obj, (lv_opa_t)op, LV_STATE_PRESSED);
            break;
        }

        case DYNAMIC_APP_STYLE_HIDDEN: {
            if (a) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
            else   lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
            break;
        }

        default:
            ESP_LOGW(TAG, "unknown style key %d", (int)cmd->u.style.key);
            break;
    }
}
