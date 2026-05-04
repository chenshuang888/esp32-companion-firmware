#include "persist.h"

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "persist";

static bool s_initialized = false;

esp_err_t persist_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase (err=0x%x), erasing and re-init", ret);
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: 0x%x", ret);
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "NVS initialized");
    return ESP_OK;
}

/* ------------------------------------------------------------------
 * 内部工具：短事务 open -> op -> commit -> close
 * ------------------------------------------------------------------ */

static esp_err_t open_for_read(const char *ns, nvs_handle_t *out_handle)
{
    esp_err_t err = nvs_open(ns, NVS_READONLY, out_handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* namespace 不存在 — 首次使用，调用方按 NOT_FOUND 处理 */
        return err;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open(ns=%s, RO) failed: 0x%x", ns, err);
    }
    return err;
}

static esp_err_t open_for_write(const char *ns, nvs_handle_t *out_handle)
{
    esp_err_t err = nvs_open(ns, NVS_READWRITE, out_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open(ns=%s, RW) failed: 0x%x", ns, err);
    }
    return err;
}

/* ------------------------------------------------------------------
 * u8
 * ------------------------------------------------------------------ */

esp_err_t persist_get_u8(const char *ns, const char *key, uint8_t *out)
{
    nvs_handle_t h;
    esp_err_t err = open_for_read(ns, &h);
    if (err != ESP_OK) return err;

    err = nvs_get_u8(h, key, out);
    nvs_close(h);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "get_u8(%s/%s) err=0x%x", ns, key, err);
    }
    return err;
}

esp_err_t persist_set_u8(const char *ns, const char *key, uint8_t val)
{
    nvs_handle_t h;
    esp_err_t err = open_for_write(ns, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_u8(h, key, val);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_u8(%s/%s) err=0x%x", ns, key, err);
    }
    return err;
}

/* ------------------------------------------------------------------
 * i64
 * ------------------------------------------------------------------ */

esp_err_t persist_get_i64(const char *ns, const char *key, int64_t *out)
{
    nvs_handle_t h;
    esp_err_t err = open_for_read(ns, &h);
    if (err != ESP_OK) return err;

    err = nvs_get_i64(h, key, out);
    nvs_close(h);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "get_i64(%s/%s) err=0x%x", ns, key, err);
    }
    return err;
}

esp_err_t persist_set_i64(const char *ns, const char *key, int64_t val)
{
    nvs_handle_t h;
    esp_err_t err = open_for_write(ns, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_i64(h, key, val);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_i64(%s/%s) err=0x%x", ns, key, err);
    }
    return err;
}

/* ------------------------------------------------------------------
 * blob
 * ------------------------------------------------------------------ */

esp_err_t persist_get_blob(const char *ns, const char *key,
                           void *buf, size_t *len)
{
    nvs_handle_t h;
    esp_err_t err = open_for_read(ns, &h);
    if (err != ESP_OK) return err;

    err = nvs_get_blob(h, key, buf, len);
    nvs_close(h);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "get_blob(%s/%s) err=0x%x", ns, key, err);
    }
    return err;
}

esp_err_t persist_set_blob(const char *ns, const char *key,
                           const void *buf, size_t len)
{
    nvs_handle_t h;
    esp_err_t err = open_for_write(ns, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_blob(h, key, buf, len);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_blob(%s/%s, %u B) err=0x%x",
                 ns, key, (unsigned)len, err);
    }
    return err;
}

/* ------------------------------------------------------------------
 * erase
 * ------------------------------------------------------------------ */

esp_err_t persist_erase_namespace(const char *ns)
{
    nvs_handle_t h;
    esp_err_t err = open_for_write(ns, &h);
    if (err != ESP_OK) return err;

    err = nvs_erase_all(h);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "erase_namespace(%s) err=0x%x", ns, err);
    return err;
}
