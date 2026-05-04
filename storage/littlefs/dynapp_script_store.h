#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * dynapp_script_store —— "动态 App 仓库"业务层（按 app_id 文件夹布局）
 *
 * 与 fs_littlefs 的分工：
 *   fs_littlefs        只知道"挂了一个 LittleFS"
 *   dynapp_script_store 知道每个 app 占一个文件夹，里面有：
 *     /littlefs/apps/<id>/main.js          脚本入口
 *     /littlefs/apps/<id>/manifest.json    元信息（id/name 必填）
 *     /littlefs/apps/<id>/data/...         JS 侧 sys.fs.* 的沙箱写区
 *
 * 名称规则：app_id 与 user 文件 relpath 都仅允许 [a-zA-Z0-9_.-]，
 *          不允许 '/'、'..'、绝对路径，避免穿越。
 */

#define DYNAPP_SCRIPT_STORE_MAX_BYTES   (64 * 1024)
#define DYNAPP_SCRIPT_STORE_MAX_NAME    15           /* app_id 长度上限 */
#define DYNAPP_SCRIPT_STORE_MAX_FNAME   31           /* 仓内单个文件名上限 */
#define DYNAPP_USER_DATA_MAX_PATH       31           /* sys.fs 相对路径上限 */
#define DYNAPP_USER_DATA_MAX_BYTES      (256 * 1024) /* sys.fs.write 大文件上限
                                                      * （走分段写路径，PSRAM 中转） */

/* 入口与元信息文件名约定（不在 manifest 声明，固定下来） */
#define DYNAPP_FILE_MAIN      "main.js"
#define DYNAPP_FILE_MANIFEST  "manifest.json"
#define DYNAPP_DATA_SUBDIR    "data"

/* 在 fs_littlefs_init() 之后调用一次。负责确保 /littlefs/apps/ 目录存在，
 * 并清理上次崩溃留下的孤儿 .tmp。 */
esp_err_t dynapp_script_store_init(void);

/* ------------------------------------------------------------------
 * App 仓内文件级 IO（main.js / manifest.json / 任意 app 自描资源）
 *
 * 调用方需提供 (app_id, filename)。filename 通常用宏 DYNAPP_FILE_MAIN 等。
 * ------------------------------------------------------------------ */

esp_err_t dynapp_app_file_read(const char *app_id, const char *filename,
                               uint8_t **out_buf, size_t *out_len);

esp_err_t dynapp_app_file_write(const char *app_id, const char *filename,
                                const uint8_t *buf, size_t len);

bool      dynapp_app_file_exists(const char *app_id, const char *filename);

/* 删除整个 app 目录（含 main.js / manifest.json / data/ 全部清空）。 */
esp_err_t dynapp_app_delete(const char *app_id);

/* 释放 read 返回的 buffer。NULL 安全。 */
void dynapp_script_store_release(uint8_t *buf);

/* 列举所有 FS 上的 app_id（按文件夹）。 */
int  dynapp_script_store_list(char out[][DYNAPP_SCRIPT_STORE_MAX_NAME + 1], int max);

/* ------------------------------------------------------------------
 * Manifest 极简解析 / 写入
 *
 * MVP 字段只有 id 与 name；缺失或 JSON 损坏时 read 返回 ESP_ERR_INVALID_ARG，
 * 调用方可回落到 app_id 作为 display name。
 * ------------------------------------------------------------------ */

typedef struct {
    char id[DYNAPP_SCRIPT_STORE_MAX_NAME + 1];
    char name[32];     /* 显示名，UTF-8，可中文 */
    /* 可选：launcher 图标，对应 app/app_fonts.h::ICON_* 名字（去前缀，如
     * "ALARM" / "NOTIFICATIONS"）。固件查表翻译为字体 codepoint。
     * 字段为空时 launcher 回退到通用 ICON_APPS。 */
    char icon[24];
    /* 可选：launcher 图标颜色，对应 app/ui/ui_tokens.h::UI_C_* 名字（去前缀，
     * 如 "ACCENT" / "WARN" / "OK"）。空时回退到中性灰。 */
    char icon_color[16];
} dynapp_manifest_t;

esp_err_t dynapp_manifest_read(const char *app_id, dynapp_manifest_t *out);

/* ------------------------------------------------------------------
 * sys.fs.* 用户数据沙箱
 *
 * 内部把 relpath 拼成 /littlefs/apps/<app_id>/data/<relpath>，
 * 强制拒绝 '..'、'/'、'\\'、'\0' 之外的非法字符；data/ 按需 mkdir。
 * ------------------------------------------------------------------ */

esp_err_t dynapp_user_data_read(const char *app_id, const char *relpath,
                                uint8_t **out_buf, size_t *out_len);

esp_err_t dynapp_user_data_write(const char *app_id, const char *relpath,
                                 const uint8_t *buf, size_t len);

esp_err_t dynapp_user_data_remove(const char *app_id, const char *relpath);

bool      dynapp_user_data_exists(const char *app_id, const char *relpath);

int       dynapp_user_data_list(const char *app_id,
                                char out[][DYNAPP_USER_DATA_MAX_PATH + 1], int max);

/* ------------------------------------------------------------------
 * 流式写入（给 BLE 上传 worker 用）
 *
 *   open_writer(app_id, filename) → append × N → commit / abort
 *   单一活跃 writer；新 open 时若旧的没关会先 abort。
 * ------------------------------------------------------------------ */

typedef struct dynapp_script_writer dynapp_script_writer_t;

dynapp_script_writer_t *dynapp_script_store_open_writer(const char *app_id,
                                                         const char *filename);

esp_err_t dynapp_script_writer_append(dynapp_script_writer_t *w,
                                      const uint8_t *data, size_t len);

esp_err_t dynapp_script_writer_commit(dynapp_script_writer_t *w);
void      dynapp_script_writer_abort(dynapp_script_writer_t *w);
bool      dynapp_script_store_has_active_writer(void);

#ifdef __cplusplus
}
#endif
