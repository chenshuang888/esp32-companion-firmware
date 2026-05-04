#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 动态 App 注册表 —— 把 app_id 映射到入口脚本 main.js 与 manifest 元信息。
 *
 * 单源：业务 app 一律来自 LittleFS，目录布局：
 *   /littlefs/apps/<app_id>/main.js          入口
 *   /littlefs/apps/<app_id>/manifest.json    {"id":..., "name":...}
 *   /littlefs/apps/<app_id>/data/...         JS sys.fs.* 的沙箱写区
 *
 * 例外：prelude.js 编译期内嵌进固件（runtime 标准库，不在 FS）。
 */

#define DYNAPP_REGISTRY_NAME_MAX  15
#define DYNAPP_REGISTRY_DISP_MAX  31

typedef struct {
    char id[DYNAPP_REGISTRY_NAME_MAX + 1];   /* app_id（目录名） */
    char display[DYNAPP_REGISTRY_DISP_MAX + 1]; /* manifest.name；缺失时回退 id */
    bool has_manifest;                       /* manifest 存在且解析成功 */

    /* launcher 图标：UTF-8 codepoint 字符串（从 manifest.icon 名字翻译）。
     * 长度足够装下任意 Material Symbols PUA codepoint（最多 4 字节）。
     * 空串表示 manifest 没指定，launcher 应回退到通用 ICON_APPS。 */
    char icon[8];
    /* launcher 图标颜色：0xRRGGBB（从 manifest.iconColor 名字翻译）。
     * 0 表示 manifest 没指定，launcher 应回退到中性灰。 */
    uint32_t icon_color;
} dynamic_app_entry_t;

/**
 * 取入口 main.js buffer。释放走 dynamic_app_registry_release()。
 */
bool dynamic_app_registry_get(const char *app_id,
                              const uint8_t **out_buf,
                              size_t *out_len);

void dynamic_app_registry_release(const uint8_t *buf);

void dynamic_app_registry_get_prelude(const uint8_t **out_buf,
                                      size_t *out_len);

/* 列举所有 FS 上的 app（按子目录）。每条会尝试读 manifest.json。 */
int dynamic_app_registry_list(dynamic_app_entry_t *out, int max);

/* 当前正在跑的 app_id：start 时由控制层写入，sys.* native 取用。 */
void        dynamic_app_registry_set_current(const char *app_id);
const char *dynamic_app_registry_current(void);

#ifdef __cplusplus
}
#endif
