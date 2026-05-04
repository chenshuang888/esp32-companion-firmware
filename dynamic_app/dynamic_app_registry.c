/* ============================================================================
 * dynamic_app_registry.c —— "app_id → main.js + manifest" 单源（FS）查找
 *
 * 设计：
 *   - 业务 app 全部来自 LittleFS：/littlefs/apps/<app_id>/main.js
 *     manifest 元信息可选；缺失时 display name 回退到 app_id
 *   - prelude.js 仍是唯一内嵌脚本（rodata）
 * ========================================================================= */

#include "dynamic_app_registry.h"
#include "dynapp_script_store.h"

#include <string.h>

extern const uint8_t prelude_js_start[]  asm("_binary_prelude_js_start");
extern const uint8_t prelude_js_end[]    asm("_binary_prelude_js_end");

/* ============================================================================
 * manifest.icon / iconColor 名字 → 字体 codepoint / 颜色值
 *
 * codepoint UTF-8 字面量与 app/app_fonts.h::ICON_* 同步（避免反向依赖 app
 * 组件，改在此处直接硬编码；新增图标两边都要改）。
 * 颜色值与 app/ui/ui_tokens.h::UI_C_* 同步。
 * 这两份表都是 manifest 字符串到字节/数值的桥接。
 * ========================================================================= */

typedef struct {
    const char *name;
    const char *utf8;
} icon_entry_t;

static const icon_entry_t k_icon_table[] = {
    /* 与 sys.icons.* 对齐（codepoint 与 app_fonts.h::ICON_* 同步） */
    { "BLUETOOTH",     "\xEE\x86\xA7" },
    { "BT_DISABLED",   "\xEE\x86\xA8" },
    { "SCHEDULE",      "\xEE\xA2\xB5" },
    { "WEATHER",       "\xEF\x85\xB2" },
    { "NOTIFICATIONS", "\xEE\x9F\xB4" },
    { "MUSIC",         "\xEE\x90\x85" },
    { "TUNE",          "\xEE\x90\xA9" },
    { "SETTINGS",      "\xEE\xA2\xB8" },
    { "BRIGHTNESS",    "\xEE\x86\xA9" },
    { "INFO",          "\xEE\xA2\x8E" },
    { "EDIT_CALENDAR", "\xEE\x95\x96" },
    { "APPS",          "\xEE\x97\x83" },
    { "CHEVRON_LEFT",  "\xEE\x97\x8B" },
    { "CHEVRON_RIGHT", "\xEE\x97\x8C" },
    { "DOT",           "\xEE\xBD\x8A" },
    { "DOT_SMALL",     "\xEE\x81\xA1" },
    /* 业务 app 图标（本轮新增） */
    { "ALARM",         "\xEE\xA1\x95" },
    { "TIMER",         "\xEE\x90\xA5" },
    { "STOPWATCH",     "\xEE\x90\xA5" },   /* 同图标 */
    { "HABIT",         "\xEE\xA1\xAC" },
    { "NOTE",          "\xEE\xA1\xB3" },
    { "GAME",          "\xEE\x8C\xB8" },
    { "CALCULATOR",    "\xEE\xA9\x9F" },
    { "IMAGE",         "\xEE\x8F\xB4" },
    { "MEMORY",        "\xEE\x8C\xA2" },
    { "DASHBOARD",     "\xEE\xA1\xB1" },
    { "PUZZLE",        "\xEE\xA1\xBB" },
    { "TARGET",        "\xEE\x86\xB3" },
    { "PETS",          "\xEE\xA4\x9D" },
    { "AQUARIUM",      "\xEE\xA4\x9D" },   /* 同 PETS */
    { "ECHO",          "\xEE\xBD\x89" },
};
#define K_ICON_TABLE_LEN ((int)(sizeof(k_icon_table) / sizeof(k_icon_table[0])))

typedef struct {
    const char *name;
    uint32_t    rgb;
} color_entry_t;

static const color_entry_t k_color_table[] = {
    { "BG",         0xF2F2F7 },
    { "PANEL",      0xFFFFFF },
    { "PANEL_HI",   0xE5E5EA },
    { "BORDER",     0xC6C6C8 },
    { "TEXT",       0x000000 },
    { "TEXT_DIM",   0x3C3C43 },
    { "TEXT_MUTED", 0x6E6E73 },
    { "ACCENT",     0x007AFF },
    { "ACCENT_2",   0xAF52DE },
    { "OK",         0x34C759 },
    { "WARN",       0xFF9500 },
    { "ERR",        0xFF3B30 },
    { "INFO",       0x5AC8FA },
};
#define K_COLOR_TABLE_LEN ((int)(sizeof(k_color_table) / sizeof(k_color_table[0])))

static const char *resolve_icon_name(const char *name)
{
    if (!name || !name[0]) return "";
    for (int i = 0; i < K_ICON_TABLE_LEN; i++) {
        if (strcmp(k_icon_table[i].name, name) == 0) {
            return k_icon_table[i].utf8;
        }
    }
    return "";
}

static uint32_t resolve_color_name(const char *name)
{
    if (!name || !name[0]) return 0;
    for (int i = 0; i < K_COLOR_TABLE_LEN; i++) {
        if (strcmp(k_color_table[i].name, name) == 0) {
            return k_color_table[i].rgb;
        }
    }
    return 0;
}

bool dynamic_app_registry_get(const char *app_id,
                              const uint8_t **out_buf,
                              size_t *out_len)
{
    if (!app_id || !*app_id || !out_buf || !out_len) return false;

    uint8_t *buf = NULL;
    size_t   len = 0;
    if (dynapp_app_file_read(app_id, DYNAPP_FILE_MAIN, &buf, &len) != 0) return false;

    *out_buf = buf;
    *out_len = len;
    return true;
}

void dynamic_app_registry_release(const uint8_t *buf)
{
    if (!buf) return;
    dynapp_script_store_release((uint8_t *)buf);
}

void dynamic_app_registry_get_prelude(const uint8_t **out_buf, size_t *out_len)
{
    if (out_buf) *out_buf = prelude_js_start;
    if (out_len) *out_len = (size_t)(prelude_js_end - prelude_js_start);
}

int dynamic_app_registry_list(dynamic_app_entry_t *out, int max)
{
    if (!out || max <= 0) return 0;

    /* 中转 buffer 容量必须 >= launcher 的 MAX_DYN_APPS（16），不然会
     * 在调用 dynapp_script_store_list 这一层先被截断，调用方传再大的 max
     * 也救不回来。提到 24 给未来留点裕量。 */
    char fs_names[24][DYNAPP_SCRIPT_STORE_MAX_NAME + 1];
    int  fs_count = dynapp_script_store_list(fs_names,
                       (int)(sizeof(fs_names) / sizeof(fs_names[0])));

    int n = 0;
    for (int i = 0; i < fs_count && n < max; i++) {
        strncpy(out[n].id, fs_names[i], DYNAPP_REGISTRY_NAME_MAX);
        out[n].id[DYNAPP_REGISTRY_NAME_MAX] = '\0';
        out[n].icon[0]    = '\0';
        out[n].icon_color = 0;

        dynapp_manifest_t mf;
        if (dynapp_manifest_read(out[n].id, &mf) == 0 && mf.name[0]) {
            strncpy(out[n].display, mf.name, DYNAPP_REGISTRY_DISP_MAX);
            out[n].display[DYNAPP_REGISTRY_DISP_MAX] = '\0';
            out[n].has_manifest = true;

            /* manifest 可选 icon / iconColor 字段（字符串名）→ 翻译为
             * UTF-8 codepoint / 0xRRGGBB。失败回空串/0，launcher 自己回退。 */
            const char *utf8 = resolve_icon_name(mf.icon);
            if (utf8 && utf8[0]) {
                strncpy(out[n].icon, utf8, sizeof(out[n].icon) - 1);
                out[n].icon[sizeof(out[n].icon) - 1] = '\0';
            }
            out[n].icon_color = resolve_color_name(mf.icon_color);
        } else {
            strncpy(out[n].display, out[n].id, DYNAPP_REGISTRY_DISP_MAX);
            out[n].display[DYNAPP_REGISTRY_DISP_MAX] = '\0';
            out[n].has_manifest = false;
        }
        n++;
    }
    return n;
}

static char s_current_id[16] = "";

void dynamic_app_registry_set_current(const char *app_id)
{
    if (!app_id) { s_current_id[0] = '\0'; return; }
    strncpy(s_current_id, app_id, sizeof(s_current_id) - 1);
    s_current_id[sizeof(s_current_id) - 1] = '\0';
}

const char *dynamic_app_registry_current(void)
{
    return s_current_id;
}
