#include "ui_statusbar.h"
#include "ui_tokens.h"
#include "app_fonts.h"
#include "ble_driver.h"
#include "battery_sim.h"

#include <stdio.h>
#include <time.h>

/* ============================================================================
 * 内部状态：每个 statusbar 实例一份
 * ========================================================================= */

typedef struct {
    lv_obj_t *time_lbl;
    lv_obj_t *bt_lbl;
    lv_obj_t *batt_shell;     /* 横向电池外壳 */
    lv_obj_t *batt_fill;      /* 内部填充条（宽度按 % 调） */
    lv_obj_t *batt_tip;       /* 右端正极凸起 */
    lv_obj_t *batt_pct_lbl;   /* 居中数字 "87" */
    lv_timer_t *tick;
    bool dark;
} statusbar_ctx_t;

/* 横向电池尺寸 */
#define BATT_W       32
#define BATT_H       14
#define BATT_PAD     1     /* 外壳内 padding（border + 内壳 fill 之间） */
#define BATT_FILL_W  (BATT_W - 2 - 2 * BATT_PAD)   /* 28 */
#define BATT_FILL_H  (BATT_H - 2 - 2 * BATT_PAD)   /* 10 */
#define BATT_TIP_W   2
#define BATT_TIP_H   6

/* ============================================================================
 * 主题色：浅色页用黑系文字，深色页用白系文字
 * ========================================================================= */

#define STATUSBAR_DARK_TEXT       lv_color_hex(0xF1ECFF)
#define STATUSBAR_DARK_TEXT_MUTED lv_color_hex(0x9B94B5)
#define STATUSBAR_DARK_ACCENT     lv_color_hex(0x06B6D4)
#define STATUSBAR_DARK_OK         lv_color_hex(0x10B981)
#define STATUSBAR_DARK_WARN       lv_color_hex(0xF97316)
#define STATUSBAR_DARK_ERR        lv_color_hex(0xEF4444)

static lv_color_t txt_color(bool dark)        { return dark ? STATUSBAR_DARK_TEXT       : UI_C_TEXT; }
static lv_color_t txt_muted_color(bool dark)  { return dark ? STATUSBAR_DARK_TEXT_MUTED : UI_C_TEXT_MUTED; }
static lv_color_t accent_color(bool dark)     { return dark ? STATUSBAR_DARK_ACCENT     : UI_C_ACCENT; }
static lv_color_t border_color(bool dark)     { return dark ? STATUSBAR_DARK_TEXT_MUTED : UI_C_BORDER; }

static lv_color_t batt_fill_color(bool dark, battery_state_t st)
{
    switch (st) {
    case BATTERY_OK:       return dark ? STATUSBAR_DARK_OK   : UI_C_OK;
    case BATTERY_LOW:      return dark ? STATUSBAR_DARK_WARN : UI_C_WARN;
    case BATTERY_CRITICAL: return dark ? STATUSBAR_DARK_ERR  : UI_C_ERR;
    default:               return txt_muted_color(dark);
    }
}

/* ============================================================================
 * 数据 → 显示
 * ========================================================================= */

static void refresh(statusbar_ctx_t *ctx)
{
    /* 时间 */
    time_t now;
    struct tm tm;
    time(&now);
    localtime_r(&now, &tm);
    char tbuf[8];
    snprintf(tbuf, sizeof(tbuf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    lv_label_set_text(ctx->time_lbl, tbuf);

    /* 蓝牙 */
    bool bt_on = ble_driver_is_connected();
    lv_label_set_text(ctx->bt_lbl, bt_on ? ICON_BLUETOOTH : ICON_BT_DISABLED);
    lv_obj_set_style_text_color(ctx->bt_lbl,
        bt_on ? accent_color(ctx->dark) : txt_muted_color(ctx->dark), 0);

    /* 电池 */
    uint8_t pct = battery_sim_get_percent();
    if (pct > 100) pct = 100;
    battery_state_t st = battery_sim_get_state();

    /* 填充宽度按 % 缩放（最少 1px 让 0% 也有一丝可见线） */
    int32_t fill_w = (BATT_FILL_W * pct + 50) / 100;
    if (fill_w < 1 && pct > 0) fill_w = 1;
    if (fill_w > BATT_FILL_W) fill_w = BATT_FILL_W;
    lv_obj_set_size(ctx->batt_fill, fill_w, BATT_FILL_H);
    lv_obj_set_style_bg_color(ctx->batt_fill, batt_fill_color(ctx->dark, st), 0);

    /* 数字（不带 % 节省空间） */
    char pbuf[8];
    snprintf(pbuf, sizeof(pbuf), "%u", (unsigned)pct);
    lv_label_set_text(ctx->batt_pct_lbl, pbuf);
}

static void tick_cb(lv_timer_t *t)
{
    statusbar_ctx_t *ctx = (statusbar_ctx_t *)lv_timer_get_user_data(t);
    refresh(ctx);
}

static void on_bar_delete(lv_event_t *e)
{
    statusbar_ctx_t *ctx = (statusbar_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;
    if (ctx->tick) {
        lv_timer_delete(ctx->tick);
        ctx->tick = NULL;
    }
    lv_free(ctx);
}

/* ============================================================================
 * 公开 API
 * ========================================================================= */

lv_obj_t *ui_statusbar_create(lv_obj_t *parent, bool dark)
{
    statusbar_ctx_t *ctx = (statusbar_ctx_t *)lv_malloc_zeroed(sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->dark = dark;

    /* 容器：240×24，顶部贴齐，透明背景 + 1px 底部分隔线 */
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 240, 24);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side (bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(bar, border_color(dark), 0);
    lv_obj_set_style_border_opa  (bar, LV_OPA_30, 0);
    lv_obj_set_style_pad_hor(bar, UI_SP_MD, 0);
    lv_obj_set_style_pad_ver(bar, 0, 0);

    /* 内部 flex 行：time | spacer(flex_grow) | bt | batt_group */
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, UI_SP_XS, 0);   /* 4px：蓝牙和电池靠近 */

    /* 时间（左）*/
    ctx->time_lbl = lv_label_create(bar);
    lv_label_set_text(ctx->time_lbl, "--:--");
    lv_obj_set_style_text_font (ctx->time_lbl, UI_F_LABEL, 0);
    lv_obj_set_style_text_color(ctx->time_lbl, txt_color(dark), 0);
    lv_obj_set_flex_grow(ctx->time_lbl, 1);   /* 撑开把右侧推到右边 */

    /* 蓝牙图标 —— Material Symbols */
    ctx->bt_lbl = lv_label_create(bar);
    lv_label_set_text(ctx->bt_lbl, ICON_BT_DISABLED);
    lv_obj_set_style_text_font (ctx->bt_lbl, APP_FONT_ICONS_24, 0);
    lv_obj_set_style_text_color(ctx->bt_lbl, txt_muted_color(dark), 0);

    /* 电池组：外壳 + 凸起，水平 flex */
    lv_obj_t *batt_group = lv_obj_create(bar);
    lv_obj_remove_style_all(batt_group);
    lv_obj_set_size(batt_group, BATT_W + BATT_TIP_W, BATT_H);
    lv_obj_clear_flag(batt_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(batt_group, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(batt_group, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(batt_group, 0, 0);

    /* 电池外壳：1px 边框 + 圆角，透明底 */
    ctx->batt_shell = lv_obj_create(batt_group);
    lv_obj_remove_style_all(ctx->batt_shell);
    lv_obj_set_size(ctx->batt_shell, BATT_W, BATT_H);
    lv_obj_clear_flag(ctx->batt_shell, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ctx->batt_shell, 3, 0);
    lv_obj_set_style_border_width(ctx->batt_shell, 1, 0);
    lv_obj_set_style_border_color(ctx->batt_shell, txt_color(dark), 0);
    lv_obj_set_style_border_opa  (ctx->batt_shell, LV_OPA_70, 0);
    lv_obj_set_style_bg_opa(ctx->batt_shell, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(ctx->batt_shell, BATT_PAD, 0);

    /* 内部填充条（宽度由 refresh 设置） */
    ctx->batt_fill = lv_obj_create(ctx->batt_shell);
    lv_obj_remove_style_all(ctx->batt_fill);
    lv_obj_clear_flag(ctx->batt_fill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(ctx->batt_fill, 0, BATT_FILL_H);
    lv_obj_set_style_radius(ctx->batt_fill, 1, 0);
    lv_obj_set_style_bg_color(ctx->batt_fill, dark ? STATUSBAR_DARK_OK : UI_C_OK, 0);
    lv_obj_set_style_bg_opa  (ctx->batt_fill, LV_OPA_COVER, 0);
    lv_obj_align(ctx->batt_fill, LV_ALIGN_LEFT_MID, 0, 0);

    /* 居中百分比数字 —— 覆盖在外壳上 */
    ctx->batt_pct_lbl = lv_label_create(ctx->batt_shell);
    lv_label_set_text(ctx->batt_pct_lbl, "--");
    lv_obj_set_style_text_font (ctx->batt_pct_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ctx->batt_pct_lbl, txt_color(dark), 0);
    lv_obj_center(ctx->batt_pct_lbl);

    /* 正极凸起（外壳右侧） */
    ctx->batt_tip = lv_obj_create(batt_group);
    lv_obj_remove_style_all(ctx->batt_tip);
    lv_obj_set_size(ctx->batt_tip, BATT_TIP_W, BATT_TIP_H);
    lv_obj_set_style_radius(ctx->batt_tip, 1, 0);
    lv_obj_set_style_bg_color(ctx->batt_tip, txt_color(dark), 0);
    lv_obj_set_style_bg_opa  (ctx->batt_tip, LV_OPA_70, 0);

    /* 1 Hz 刷新 */
    ctx->tick = lv_timer_create(tick_cb, 1000, ctx);
    refresh(ctx);   /* 先画一次免得初始 1s 都是 -- */

    /* 销毁时清理 ctx + timer */
    lv_obj_add_event_cb(bar, on_bar_delete, LV_EVENT_DELETE, ctx);

    return bar;
}
