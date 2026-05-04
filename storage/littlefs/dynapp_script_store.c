/* ============================================================================
 * dynapp_script_store.c —— 动态 App 仓（按 app_id 文件夹布局）
 *
 * 文件布局：
 *   /littlefs/apps/<app_id>/main.js                正式入口脚本
 *   /littlefs/apps/<app_id>/main.js.tmp            上传中临时文件（rename 前）
 *   /littlefs/apps/<app_id>/manifest.json          元信息（id/name 必填）
 *   /littlefs/apps/<app_id>/data/<rel>             JS sys.fs.* 写区
 *
 * 名称规则：
 *   - app_id：[a-zA-Z0-9_-]，长度 ≤ DYNAPP_SCRIPT_STORE_MAX_NAME
 *   - 仓内 filename：[a-zA-Z0-9_.-]，长度 ≤ DYNAPP_SCRIPT_STORE_MAX_FNAME
 *   - sys.fs relpath：[a-zA-Z0-9_.-]，长度 ≤ DYNAPP_USER_DATA_MAX_PATH，
 *     不含 '/'、'\\'、'..'，第一字符不为 '.'
 *
 * 设计取舍：
 *   - manifest 解析手撸（不引 cJSON）：MVP 只读 "id"/"name" 两个 string，
 *     20 行 helper 够用，避免给整套 idf 加依赖
 *   - data/ 按需 mkdir，不在 init 时预创建（很多 app 没数据时省一个目录）
 * ========================================================================= */

#include "dynapp_script_store.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "fs_littlefs.h"

static const char *TAG = "dynapp_script";

#define APPS_DIR     FS_LITTLEFS_ROOT "/apps"
#define PATH_BUFSZ   128

/* ============================================================================
 * §1. 名称校验
 * ========================================================================= */

static bool app_id_is_valid(const char *id)
{
    if (!id || !*id) return false;
    size_t n = strlen(id);
    if (n > DYNAPP_SCRIPT_STORE_MAX_NAME) return false;
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
               || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

static bool filename_is_valid(const char *fn)
{
    if (!fn || !*fn) return false;
    size_t n = strlen(fn);
    if (n > DYNAPP_SCRIPT_STORE_MAX_FNAME) return false;
    if (fn[0] == '.') return false;  /* 不允许隐藏文件 / .. */

    /* 允许唯一一种子目录：assets/<base>。其它任何 '/' 都拒绝，
     * 避免穿越或与 data/ 沙箱混用。 */
    static const char ASSETS_PREFIX[] = "assets/";
    const size_t APREF_LEN = sizeof(ASSETS_PREFIX) - 1;
    const char *base = fn;
    if (n > APREF_LEN && memcmp(fn, ASSETS_PREFIX, APREF_LEN) == 0) {
        base = fn + APREF_LEN;
        if (*base == '\0' || *base == '.') return false;
    }
    /* base 段（无论是否带 assets/ 前缀）都走原字符集 */
    for (const char *p = base; *p; p++) {
        char c = *p;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
               || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!ok) return false;
    }
    return true;
}

/* 是否是 assets/ 子目录下的文件 */
static bool filename_is_asset(const char *fn)
{
    static const char ASSETS_PREFIX[] = "assets/";
    return strncmp(fn, ASSETS_PREFIX, sizeof(ASSETS_PREFIX) - 1) == 0;
}

static bool user_relpath_is_valid(const char *rp)
{
    if (!rp || !*rp) return false;
    size_t n = strlen(rp);
    if (n > DYNAPP_USER_DATA_MAX_PATH) return false;
    if (rp[0] == '.' || rp[0] == '/') return false;
    for (size_t i = 0; i < n; i++) {
        char c = rp[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
               || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!ok) return false;
    }
    return true;
}

/* ============================================================================
 * §2. 路径拼接
 * ========================================================================= */

static void build_app_dir(char *out, size_t cap, const char *app_id)
{
    snprintf(out, cap, "%s/%s", APPS_DIR, app_id);
}

static void build_app_file(char *out, size_t cap,
                            const char *app_id, const char *fname, bool tmp)
{
    if (tmp) snprintf(out, cap, "%s/%s/%s.tmp", APPS_DIR, app_id, fname);
    else     snprintf(out, cap, "%s/%s/%s",     APPS_DIR, app_id, fname);
}

static void build_user_data_path(char *out, size_t cap,
                                  const char *app_id, const char *relpath)
{
    snprintf(out, cap, "%s/%s/%s/%s",
             APPS_DIR, app_id, DYNAPP_DATA_SUBDIR, relpath);
}

/* mkdir -p 父目录链（仅一层，apps/<id>/）。已存在不算错。 */
static esp_err_t ensure_dir(const char *path)
{
    if (mkdir(path, 0775) == 0) return ESP_OK;
    if (errno == EEXIST) return ESP_OK;
    ESP_LOGE(TAG, "mkdir %s failed: errno=%d", path, errno);
    return ESP_FAIL;
}

static esp_err_t ensure_app_dir(const char *app_id)
{
    char dir[PATH_BUFSZ];
    build_app_dir(dir, sizeof(dir), app_id);
    return ensure_dir(dir);
}

static esp_err_t ensure_data_dir(const char *app_id)
{
    char dir[PATH_BUFSZ];
    snprintf(dir, sizeof(dir), "%s/%s/%s",
             APPS_DIR, app_id, DYNAPP_DATA_SUBDIR);
    /* 父目录可能也没建（例如尚未上传 main.js 就先 sys.fs.write） */
    char parent[PATH_BUFSZ];
    build_app_dir(parent, sizeof(parent), app_id);
    esp_err_t e = ensure_dir(parent);
    if (e != ESP_OK) return e;
    return ensure_dir(dir);
}

static esp_err_t ensure_assets_dir(const char *app_id)
{
    char parent[PATH_BUFSZ];
    build_app_dir(parent, sizeof(parent), app_id);
    esp_err_t e = ensure_dir(parent);
    if (e != ESP_OK) return e;
    char dir[PATH_BUFSZ];
    snprintf(dir, sizeof(dir), "%s/%s/assets", APPS_DIR, app_id);
    return ensure_dir(dir);
}

/* ============================================================================
 * §3. init —— 清理孤儿 .tmp
 *
 * 旧布局是 apps/<name>.js（平铺），新布局是 apps/<id>/main.js（子目录）。
 * 我们不做兼容迁移：用户已经同意 erase-flash 重灌。
 * ========================================================================= */

static void clean_stale_tmps(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    int cleaned = 0;
    while ((ent = readdir(d)) != NULL) {
        size_t fl = strlen(ent->d_name);
        if (fl <= 4 || strcmp(ent->d_name + fl - 4, ".tmp") != 0) continue;
        char p[PATH_BUFSZ];
        if (snprintf(p, sizeof(p), "%s/%s", dir, ent->d_name) >= (int)sizeof(p)) continue;
        if (unlink(p) == 0) cleaned++;
    }
    closedir(d);
    if (cleaned > 0) ESP_LOGW(TAG, "cleaned %d stale .tmp under %s", cleaned, dir);

    /* 也扫 assets/ 子目录里的孤儿 .tmp（asset 上传中途断电会留下） */
    char assets[PATH_BUFSZ];
    if (snprintf(assets, sizeof(assets), "%s/assets", dir) >= (int)sizeof(assets)) return;
    DIR *ad = opendir(assets);
    if (!ad) return;
    int acleaned = 0;
    while ((ent = readdir(ad)) != NULL) {
        size_t fl = strlen(ent->d_name);
        if (fl <= 4 || strcmp(ent->d_name + fl - 4, ".tmp") != 0) continue;
        char p[PATH_BUFSZ];
        if (snprintf(p, sizeof(p), "%s/%s", assets, ent->d_name) >= (int)sizeof(p)) continue;
        if (unlink(p) == 0) acleaned++;
    }
    closedir(ad);
    if (acleaned > 0) ESP_LOGW(TAG, "cleaned %d stale .tmp under %s", acleaned, assets);
}

esp_err_t dynapp_script_store_init(void)
{
    int rc = mkdir(APPS_DIR, 0775);
    if (rc != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "mkdir %s failed: errno=%d", APPS_DIR, errno);
        return ESP_FAIL;
    }

    /* 扫每个 app 子目录里残留的 .tmp（main.js.tmp / manifest.json.tmp 等） */
    DIR *d = opendir(APPS_DIR);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            char sub[PATH_BUFSZ];
            if (snprintf(sub, sizeof(sub), "%s/%s",
                         APPS_DIR, ent->d_name) >= (int)sizeof(sub)) continue;
            struct stat st;
            if (stat(sub, &st) != 0) continue;
            if (!S_ISDIR(st.st_mode)) continue;
            clean_stale_tmps(sub);
        }
        closedir(d);
    }

    ESP_LOGI(TAG, "apps dir ready at %s", APPS_DIR);
    return ESP_OK;
}

/* ============================================================================
 * §4. 通用 read/write helper（不带名字校验，由公开 API 校验后调）
 * ========================================================================= */

static esp_err_t read_file(const char *path, uint8_t **out_buf, size_t *out_len,
                            size_t max_bytes)
{
    FILE *f = fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return ESP_FAIL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return ESP_FAIL; }
    if ((size_t)sz > max_bytes) {
        ESP_LOGW(TAG, "%s too large (%ld > %u), refusing",
                 path, sz, (unsigned)max_bytes);
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    rewind(f);

    uint8_t *buf = (uint8_t *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return ESP_ERR_NO_MEM; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(buf); return ESP_FAIL; }
    buf[sz] = '\0';

    *out_buf = buf;
    *out_len = (size_t)sz;
    return ESP_OK;
}

/* atomic write：先写 .tmp 再 rename。tmp_path / final_path 容量 ≥ PATH_BUFSZ。 */
static esp_err_t atomic_write(const char *tmp_path, const char *final_path,
                               const uint8_t *buf, size_t len)
{
    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "open %s failed: errno=%d", tmp_path, errno);
        return ESP_FAIL;
    }
    size_t put = fwrite(buf, 1, len, f);
    int rc1 = fflush(f);
    int rc2 = fclose(f);
    if (put != len || rc1 != 0 || rc2 != 0) {
        ESP_LOGE(TAG, "write %s short (%u/%u) flush=%d close=%d",
                 tmp_path, (unsigned)put, (unsigned)len, rc1, rc2);
        unlink(tmp_path);
        return ESP_FAIL;
    }
    unlink(final_path);
    if (rename(tmp_path, final_path) != 0) {
        ESP_LOGE(TAG, "rename %s -> %s errno=%d", tmp_path, final_path, errno);
        unlink(tmp_path);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ============================================================================
 * §5. 公开 API：app 仓内文件
 * ========================================================================= */

esp_err_t dynapp_app_file_read(const char *app_id, const char *filename,
                               uint8_t **out_buf, size_t *out_len)
{
    if (!out_buf || !out_len) return ESP_ERR_INVALID_ARG;
    *out_buf = NULL; *out_len = 0;
    if (!app_id_is_valid(app_id) || !filename_is_valid(filename))
        return ESP_ERR_INVALID_ARG;

    char path[PATH_BUFSZ];
    build_app_file(path, sizeof(path), app_id, filename, false);
    return read_file(path, out_buf, out_len, DYNAPP_SCRIPT_STORE_MAX_BYTES);
}

esp_err_t dynapp_app_file_write(const char *app_id, const char *filename,
                                const uint8_t *buf, size_t len)
{
    if (!app_id_is_valid(app_id) || !filename_is_valid(filename) || !buf)
        return ESP_ERR_INVALID_ARG;
    if (len == 0 || len > DYNAPP_SCRIPT_STORE_MAX_BYTES)
        return ESP_ERR_INVALID_SIZE;

    esp_err_t e = filename_is_asset(filename)
                  ? ensure_assets_dir(app_id)
                  : ensure_app_dir(app_id);
    if (e != ESP_OK) return e;

    char tmp_path[PATH_BUFSZ], final_path[PATH_BUFSZ];
    build_app_file(tmp_path,   sizeof(tmp_path),   app_id, filename, true);
    build_app_file(final_path, sizeof(final_path), app_id, filename, false);
    return atomic_write(tmp_path, final_path, buf, len);
}

bool dynapp_app_file_exists(const char *app_id, const char *filename)
{
    if (!app_id_is_valid(app_id) || !filename_is_valid(filename)) return false;
    char path[PATH_BUFSZ];
    build_app_file(path, sizeof(path), app_id, filename, false);
    struct stat st;
    return stat(path, &st) == 0;
}

void dynapp_script_store_release(uint8_t *buf)
{
    free(buf);
}

/* ============================================================================
 * §6. delete app —— 递归清空 apps/<id>/
 * ========================================================================= */

static void rmtree(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) {
        unlink(dir);   /* 也许是文件 */
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.' &&
           (ent->d_name[1] == '\0' ||
            (ent->d_name[1] == '.' && ent->d_name[2] == '\0'))) continue;
        char sub[PATH_BUFSZ];
        if (snprintf(sub, sizeof(sub), "%s/%s", dir, ent->d_name) >= (int)sizeof(sub))
            continue;
        struct stat st;
        if (stat(sub, &st) == 0 && S_ISDIR(st.st_mode)) {
            rmtree(sub);
        } else {
            unlink(sub);
        }
    }
    closedir(d);
    rmdir(dir);
}

esp_err_t dynapp_app_delete(const char *app_id)
{
    if (!app_id_is_valid(app_id)) return ESP_ERR_INVALID_ARG;
    char dir[PATH_BUFSZ];
    build_app_dir(dir, sizeof(dir), app_id);
    struct stat st;
    if (stat(dir, &st) != 0) return ESP_ERR_NOT_FOUND;
    rmtree(dir);
    ESP_LOGI(TAG, "deleted app %s", app_id);
    return ESP_OK;
}

/* ============================================================================
 * §7. list apps —— 扫子目录
 * ========================================================================= */

int dynapp_script_store_list(char out[][DYNAPP_SCRIPT_STORE_MAX_NAME + 1], int max)
{
    if (!out || max <= 0) return 0;
    DIR *d = opendir(APPS_DIR);
    if (!d) {
        ESP_LOGW(TAG, "list: opendir(%s) failed errno=%d", APPS_DIR, errno);
        return 0;
    }
    ESP_LOGI(TAG, "list: scanning %s ...", APPS_DIR);

    int n = 0;
    int seen = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        seen++;
        if (ent->d_name[0] == '.') {
            ESP_LOGI(TAG, "list:   '%s' SKIP (dotfile)", ent->d_name);
            continue;
        }

        char sub[PATH_BUFSZ];
        if (snprintf(sub, sizeof(sub), "%s/%s",
                     APPS_DIR, ent->d_name) >= (int)sizeof(sub)) {
            ESP_LOGW(TAG, "list:   '%s' SKIP (path too long)", ent->d_name);
            continue;
        }
        struct stat st;
        if (stat(sub, &st) != 0) {
            ESP_LOGW(TAG, "list:   '%s' SKIP (stat err errno=%d)",
                     ent->d_name, errno);
            continue;
        }
        if (!S_ISDIR(st.st_mode)) {
            ESP_LOGI(TAG, "list:   '%s' SKIP (not a dir, mode=0%o)",
                     ent->d_name, (unsigned)st.st_mode);
            continue;
        }

        size_t l = strlen(ent->d_name);
        if (l == 0 || l > DYNAPP_SCRIPT_STORE_MAX_NAME) {
            ESP_LOGW(TAG, "list:   '%s' SKIP (len=%u, max=%u)",
                     ent->d_name, (unsigned)l,
                     (unsigned)DYNAPP_SCRIPT_STORE_MAX_NAME);
            continue;
        }
        if (!app_id_is_valid(ent->d_name)) {
            ESP_LOGW(TAG, "list:   '%s' SKIP (invalid charset)", ent->d_name);
            continue;
        }

        if (n >= max) {
            ESP_LOGW(TAG, "list:   '%s' OK but out full (n=%d max=%d)",
                     ent->d_name, n, max);
            continue;
        }
        memcpy(out[n], ent->d_name, l);
        out[n][l] = '\0';
        ESP_LOGI(TAG, "list:   '%s' OK (slot=%d)", ent->d_name, n);
        n++;
    }
    closedir(d);
    ESP_LOGI(TAG, "list: done, seen=%d valid=%d", seen, n);
    return n;
}

/* ============================================================================
 * §8. manifest.json 极简解析
 *
 * 只识别 {"id":"...", "name":"..."} 两个 string 字段；其它忽略。
 * 实现：扫描键名 "id" / "name"，跳到下一个 ':'，取下一个 "..." 字符串。
 * 不支持转义字符 \" 等，因为 MVP 字段都是 ASCII/UTF-8 普通文本。
 * ========================================================================= */

static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

/* 找下一个 "key" 字符串值；返回值起点写入 *vp，长度写入 *vl。
 * 失败返回 false。 */
static bool find_string_field(const char *json, size_t jlen, const char *key,
                               const char **vp, size_t *vl)
{
    size_t klen = strlen(key);
    const char *p = json;
    const char *end = json + jlen;
    while (p < end) {
        /* 找 "key" */
        const char *q = memchr(p, '"', end - p);
        if (!q) return false;
        if ((size_t)(end - q - 1) >= klen + 1
            && memcmp(q + 1, key, klen) == 0
            && q[1 + klen] == '"') {
            const char *r = q + 2 + klen;
            r = skip_ws(r, end);
            if (r >= end || *r != ':') { p = q + 1; continue; }
            r = skip_ws(r + 1, end);
            if (r >= end || *r != '"') { p = q + 1; continue; }
            const char *vs = r + 1;
            const char *ve = memchr(vs, '"', end - vs);
            if (!ve) return false;
            *vp = vs;
            *vl = (size_t)(ve - vs);
            return true;
        }
        p = q + 1;
    }
    return false;
}

esp_err_t dynapp_manifest_read(const char *app_id, dynapp_manifest_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    uint8_t *buf = NULL;
    size_t   len = 0;
    esp_err_t e = dynapp_app_file_read(app_id, DYNAPP_FILE_MANIFEST, &buf, &len);
    if (e != ESP_OK) return e;

    const char *vs; size_t vl;
    bool ok_id   = find_string_field((const char *)buf, len, "id",   &vs, &vl);
    if (ok_id) {
        size_t cp = vl < sizeof(out->id) - 1 ? vl : sizeof(out->id) - 1;
        memcpy(out->id, vs, cp);
        out->id[cp] = '\0';
    }
    bool ok_name = find_string_field((const char *)buf, len, "name", &vs, &vl);
    if (ok_name) {
        size_t cp = vl < sizeof(out->name) - 1 ? vl : sizeof(out->name) - 1;
        memcpy(out->name, vs, cp);
        out->name[cp] = '\0';
    }
    /* 可选字段：icon / iconColor。缺失不算错误，由调用方回退默认 */
    if (find_string_field((const char *)buf, len, "icon", &vs, &vl)) {
        size_t cp = vl < sizeof(out->icon) - 1 ? vl : sizeof(out->icon) - 1;
        memcpy(out->icon, vs, cp);
        out->icon[cp] = '\0';
    }
    if (find_string_field((const char *)buf, len, "iconColor", &vs, &vl)) {
        size_t cp = vl < sizeof(out->icon_color) - 1 ? vl : sizeof(out->icon_color) - 1;
        memcpy(out->icon_color, vs, cp);
        out->icon_color[cp] = '\0';
    }
    dynapp_script_store_release(buf);

    if (!ok_id || !ok_name) return ESP_ERR_INVALID_ARG;
    /* id 字段必须与目录名一致 */
    if (strcmp(out->id, app_id) != 0) {
        ESP_LOGW(TAG, "manifest id %s != dir %s", out->id, app_id);
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

/* ============================================================================
 * §9. sys.fs.* 用户数据沙箱
 * ========================================================================= */

esp_err_t dynapp_user_data_read(const char *app_id, const char *relpath,
                                uint8_t **out_buf, size_t *out_len)
{
    if (!out_buf || !out_len) return ESP_ERR_INVALID_ARG;
    *out_buf = NULL; *out_len = 0;
    if (!app_id_is_valid(app_id) || !user_relpath_is_valid(relpath))
        return ESP_ERR_INVALID_ARG;
    char path[PATH_BUFSZ];
    build_user_data_path(path, sizeof(path), app_id, relpath);
    return read_file(path, out_buf, out_len, DYNAPP_USER_DATA_MAX_BYTES);
}

esp_err_t dynapp_user_data_write(const char *app_id, const char *relpath,
                                 const uint8_t *buf, size_t len)
{
    if (!app_id_is_valid(app_id) || !user_relpath_is_valid(relpath) || !buf)
        return ESP_ERR_INVALID_ARG;
    if (len == 0 || len > DYNAPP_USER_DATA_MAX_BYTES)
        return ESP_ERR_INVALID_SIZE;

    esp_err_t e = ensure_data_dir(app_id);
    if (e != ESP_OK) return e;

    char path[PATH_BUFSZ], tmp[PATH_BUFSZ + 8];
    build_user_data_path(path, sizeof(path), app_id, relpath);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    return atomic_write(tmp, path, buf, len);
}

esp_err_t dynapp_user_data_remove(const char *app_id, const char *relpath)
{
    if (!app_id_is_valid(app_id) || !user_relpath_is_valid(relpath))
        return ESP_ERR_INVALID_ARG;
    char path[PATH_BUFSZ];
    build_user_data_path(path, sizeof(path), app_id, relpath);
    if (unlink(path) != 0) return (errno == ENOENT) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    return ESP_OK;
}

bool dynapp_user_data_exists(const char *app_id, const char *relpath)
{
    if (!app_id_is_valid(app_id) || !user_relpath_is_valid(relpath)) return false;
    char path[PATH_BUFSZ];
    build_user_data_path(path, sizeof(path), app_id, relpath);
    struct stat st;
    return stat(path, &st) == 0;
}

int dynapp_user_data_list(const char *app_id,
                          char out[][DYNAPP_USER_DATA_MAX_PATH + 1], int max)
{
    if (!app_id_is_valid(app_id) || !out || max <= 0) return 0;
    char dir[PATH_BUFSZ];
    snprintf(dir, sizeof(dir), "%s/%s/%s",
             APPS_DIR, app_id, DYNAPP_DATA_SUBDIR);
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    struct dirent *ent;
    while (n < max && (ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        size_t l = strlen(ent->d_name);
        if (l == 0 || l > DYNAPP_USER_DATA_MAX_PATH) continue;
        memcpy(out[n], ent->d_name, l);
        out[n][l] = '\0';
        n++;
    }
    closedir(d);
    return n;
}

/* ============================================================================
 * §10. 流式 writer（BLE 上传 worker 用）
 *
 *   open_writer 现在需要 (app_id, filename)：上传 main.js / manifest.json /
 *   未来 assets/icon.png 都走同一接口。
 * ========================================================================= */

struct dynapp_script_writer {
    char    app_id[DYNAPP_SCRIPT_STORE_MAX_NAME + 1];
    char    filename[DYNAPP_SCRIPT_STORE_MAX_FNAME + 1];
    char    tmp_path[PATH_BUFSZ];
    char    final_path[PATH_BUFSZ];
    FILE   *fp;
    size_t  written;
    bool    failed;
};

static dynapp_script_writer_t *s_active_writer = NULL;

dynapp_script_writer_t *dynapp_script_store_open_writer(const char *app_id,
                                                         const char *filename)
{
    if (!app_id_is_valid(app_id) || !filename_is_valid(filename)) return NULL;

    if (s_active_writer) {
        ESP_LOGW(TAG, "open_writer: prev not closed, aborting");
        dynapp_script_writer_abort(s_active_writer);
    }

    esp_err_t mke = filename_is_asset(filename)
                    ? ensure_assets_dir(app_id)
                    : ensure_app_dir(app_id);
    if (mke != ESP_OK) return NULL;

    dynapp_script_writer_t *w = (dynapp_script_writer_t *)calloc(1, sizeof(*w));
    if (!w) return NULL;

    strncpy(w->app_id,   app_id,   sizeof(w->app_id)   - 1);
    strncpy(w->filename, filename, sizeof(w->filename) - 1);
    build_app_file(w->tmp_path,   sizeof(w->tmp_path),   app_id, filename, true);
    build_app_file(w->final_path, sizeof(w->final_path), app_id, filename, false);

    w->fp = fopen(w->tmp_path, "wb");
    if (!w->fp) {
        ESP_LOGE(TAG, "open_writer fopen %s failed: errno=%d", w->tmp_path, errno);
        free(w);
        return NULL;
    }
    s_active_writer = w;
    ESP_LOGI(TAG, "writer opened: %s", w->tmp_path);
    return w;
}

esp_err_t dynapp_script_writer_append(dynapp_script_writer_t *w,
                                      const uint8_t *data, size_t len)
{
    if (!w || !data) return ESP_ERR_INVALID_ARG;
    if (w != s_active_writer) return ESP_ERR_INVALID_STATE;
    if (w->failed) return ESP_FAIL;
    if (len == 0) return ESP_OK;

    if (w->written + len > DYNAPP_SCRIPT_STORE_MAX_BYTES) {
        ESP_LOGW(TAG, "append: %s/%s exceeded max", w->app_id, w->filename);
        w->failed = true;
        return ESP_ERR_INVALID_SIZE;
    }
    size_t put = fwrite(data, 1, len, w->fp);
    if (put != len) {
        ESP_LOGE(TAG, "append: short %u/%u errno=%d",
                 (unsigned)put, (unsigned)len, errno);
        w->failed = true;
        return ESP_FAIL;
    }
    w->written += len;
    return ESP_OK;
}

esp_err_t dynapp_script_writer_commit(dynapp_script_writer_t *w)
{
    if (!w) return ESP_ERR_INVALID_ARG;
    if (w != s_active_writer) return ESP_ERR_INVALID_STATE;

    if (w->failed) {
        dynapp_script_writer_abort(w);
        return ESP_FAIL;
    }
    int rc1 = fflush(w->fp);
    int rc2 = fclose(w->fp);
    w->fp = NULL;
    if (rc1 != 0 || rc2 != 0) {
        ESP_LOGE(TAG, "commit: flush=%d close=%d", rc1, rc2);
        unlink(w->tmp_path);
        s_active_writer = NULL;
        free(w);
        return ESP_FAIL;
    }
    unlink(w->final_path);
    if (rename(w->tmp_path, w->final_path) != 0) {
        ESP_LOGE(TAG, "commit rename %s -> %s errno=%d",
                 w->tmp_path, w->final_path, errno);
        unlink(w->tmp_path);
        s_active_writer = NULL;
        free(w);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "saved %s/%s (%u B)",
             w->app_id, w->filename, (unsigned)w->written);
    s_active_writer = NULL;
    free(w);
    return ESP_OK;
}

void dynapp_script_writer_abort(dynapp_script_writer_t *w)
{
    if (!w) return;
    if (w->fp) { fclose(w->fp); w->fp = NULL; }
    unlink(w->tmp_path);
    if (w == s_active_writer) s_active_writer = NULL;
    ESP_LOGI(TAG, "writer aborted: %s/%s", w->app_id, w->filename);
    free(w);
}

bool dynapp_script_store_has_active_writer(void)
{
    return s_active_writer != NULL;
}
