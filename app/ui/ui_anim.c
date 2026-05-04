#include "ui_anim.h"
#include "ui_tokens.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * fade_in —— opa 0→cover + y +12→0
 * ========================================================================= */

static void anim_opa_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, v, 0);
}

static void anim_y_cb(void *obj, int32_t v)
{
    lv_obj_set_style_translate_y((lv_obj_t *)obj, v, 0);
}

void ui_anim_fade_in(lv_obj_t *obj, uint32_t delay_ms)
{
    if (!obj) return;

    lv_obj_set_style_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_translate_y(obj, 12, 0);

    lv_anim_t a;

    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, anim_opa_cb);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&a, UI_DUR_NORM);
    lv_anim_set_delay(&a, delay_ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, anim_y_cb);
    lv_anim_set_values(&a, 12, 0);
    lv_anim_set_duration(&a, UI_DUR_NORM);
    lv_anim_set_delay(&a, delay_ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/* ============================================================================
 * press_feedback —— scale 256(=1.0) ↔ 244(≈0.95)
 * ========================================================================= */

static void anim_scale_cb(void *obj, int32_t v)
{
    lv_obj_set_style_transform_scale_x((lv_obj_t *)obj, v, 0);
    lv_obj_set_style_transform_scale_y((lv_obj_t *)obj, v, 0);
}

static void on_press(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, anim_scale_cb);
    lv_anim_set_values(&a, 256, 244);
    lv_anim_set_duration(&a, 80);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void on_release(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, anim_scale_cb);
    lv_anim_set_values(&a, 244, 256);
    lv_anim_set_duration(&a, 160);
    lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
    lv_anim_start(&a);
}

void ui_anim_press_feedback(lv_obj_t *obj)
{
    if (!obj) return;
    /* 让 transform_scale 围绕中心 */
    lv_obj_set_style_transform_pivot_x(obj, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(obj, LV_PCT(50), 0);
    lv_obj_add_event_cb(obj, on_press,   LV_EVENT_PRESSED,         NULL);
    lv_obj_add_event_cb(obj, on_release, LV_EVENT_RELEASED,        NULL);
    lv_obj_add_event_cb(obj, on_release, LV_EVENT_PRESS_LOST,      NULL);
}

/* ============================================================================
 * number_rolling —— anim 把 from→to 的中间值实时格式化进 label
 * ========================================================================= */

typedef struct {
    lv_obj_t *label;
    int decimals;
    char suffix[8];
} num_anim_ctx_t;

static void num_anim_cb(void *var, int32_t v)
{
    num_anim_ctx_t *ctx = (num_anim_ctx_t *)var;
    char buf[24];
    if (ctx->decimals == 0) {
        snprintf(buf, sizeof(buf), "%d%s", (int)(v / 10), ctx->suffix);
    } else {
        int sign = v < 0 ? -1 : 1;
        int av = v * sign;
        snprintf(buf, sizeof(buf), "%s%d.%d%s",
                 sign < 0 ? "-" : "",
                 av / 10, av % 10, ctx->suffix);
    }
    lv_label_set_text(ctx->label, buf);
}

static void num_anim_ready(lv_anim_t *a)
{
    free(a->var);
}

void ui_anim_number_rolling(lv_obj_t *label, int from_x10, int to_x10,
                             uint32_t duration_ms, int decimals,
                             const char *suffix)
{
    if (!label) return;
    if (from_x10 == to_x10) {
        /* 直接 set 一次免得不画 */
        char buf[24];
        const char *sfx = suffix ? suffix : "";
        if (decimals == 0) snprintf(buf, sizeof(buf), "%d%s", to_x10 / 10, sfx);
        else {
            int sign = to_x10 < 0 ? -1 : 1;
            int av = to_x10 * sign;
            snprintf(buf, sizeof(buf), "%s%d.%d%s",
                     sign < 0 ? "-" : "", av / 10, av % 10, sfx);
        }
        lv_label_set_text(label, buf);
        return;
    }

    num_anim_ctx_t *ctx = (num_anim_ctx_t *)malloc(sizeof(num_anim_ctx_t));
    if (!ctx) return;
    ctx->label = label;
    ctx->decimals = decimals;
    if (suffix) {
        size_t n = strlen(suffix);
        if (n >= sizeof(ctx->suffix)) n = sizeof(ctx->suffix) - 1;
        memcpy(ctx->suffix, suffix, n);
        ctx->suffix[n] = '\0';
    } else {
        ctx->suffix[0] = '\0';
    }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, ctx);
    lv_anim_set_exec_cb(&a, num_anim_cb);
    lv_anim_set_values(&a, from_x10, to_x10);
    lv_anim_set_duration(&a, duration_ms ? duration_ms : UI_DUR_SLOW);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, num_anim_ready);
    lv_anim_start(&a);
}
