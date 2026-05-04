#include "app_fonts.h"
#include "esp_log.h"

static const char *TAG = "app_fonts";

/* ---- 中文字体 ---- */
extern const uint8_t srhs_ttf_start[] asm("_binary_srhs_sc_subset_ttf_start");
extern const uint8_t srhs_ttf_end[]   asm("_binary_srhs_sc_subset_ttf_end");

/* ---- 图标字体（Material Symbols Rounded 子集） ---- */
extern const uint8_t icons_ttf_start[] asm("_binary_material_icons_subset_ttf_start");
extern const uint8_t icons_ttf_end[]   asm("_binary_material_icons_subset_ttf_end");

lv_font_t *g_app_font_text     = NULL;
lv_font_t *g_app_font_title    = NULL;
lv_font_t *g_app_font_huge     = NULL;
lv_font_t *g_app_font_icons_24 = NULL;
lv_font_t *g_app_font_icons_36 = NULL;

void app_fonts_init(void)
{
    const size_t ttf_size   = srhs_ttf_end - srhs_ttf_start;
    const size_t icons_size = icons_ttf_end - icons_ttf_start;
    ESP_LOGI(TAG, "embedded TTF: zh=%u B, icons=%u B",
             (unsigned)ttf_size, (unsigned)icons_size);

    /* 中文 14/16/48 三档（同前） */
    g_app_font_text  = lv_tiny_ttf_create_data_ex(
        srhs_ttf_start, ttf_size, 14, LV_FONT_KERNING_NONE, 256);
    g_app_font_title = lv_tiny_ttf_create_data_ex(
        srhs_ttf_start, ttf_size, 16, LV_FONT_KERNING_NONE, 256);
    g_app_font_huge  = lv_tiny_ttf_create_data_ex(
        srhs_ttf_start, ttf_size, 48, LV_FONT_KERNING_NONE, 32);

    /* Material Symbols 24px（状态栏）+ 36px（九宫格主图标）*/
    g_app_font_icons_24 = lv_tiny_ttf_create_data_ex(
        icons_ttf_start, icons_size, 24, LV_FONT_KERNING_NONE, 64);
    g_app_font_icons_36 = lv_tiny_ttf_create_data_ex(
        icons_ttf_start, icons_size, 36, LV_FONT_KERNING_NONE, 64);

    if (!g_app_font_text || !g_app_font_title || !g_app_font_huge
        || !g_app_font_icons_24 || !g_app_font_icons_36) {
        ESP_LOGE(TAG, "lv_tiny_ttf_create_data_ex failed (text=%p title=%p huge=%p i24=%p i36=%p)",
                 g_app_font_text, g_app_font_title, g_app_font_huge,
                 g_app_font_icons_24, g_app_font_icons_36);
        return;
    }

    /* fallback 链：CJK → Material 图标 → Montserrat (LV_SYMBOL 老图标兜底)
     * 这样写汉字 / 写 Material 图标 / 写 LV_SYMBOL_LEFT 都能正常显示 */
    g_app_font_text->fallback     = g_app_font_icons_24;
    g_app_font_title->fallback    = g_app_font_icons_24;
    g_app_font_huge->fallback     = g_app_font_icons_36;
    g_app_font_icons_24->fallback = &lv_font_montserrat_14;
    g_app_font_icons_36->fallback = &lv_font_montserrat_24;
}
