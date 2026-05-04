#include "backlight_storage.h"
#include "persist.h"

#include "esp_log.h"

static const char *TAG = "bl_storage";

#define NS_BACKLIGHT      "backlight"
#define KEY_BACKLIGHT     "bl"
#define DEFAULT_BACKLIGHT 128  /* 与 lcd_panel 的默认亮度一致（约 50%） */

static uint8_t s_backlight = DEFAULT_BACKLIGHT;

esp_err_t backlight_storage_init(void)
{
    uint8_t bl = DEFAULT_BACKLIGHT;
    esp_err_t err = persist_get_u8(NS_BACKLIGHT, KEY_BACKLIGHT, &bl);
    if (err == ESP_OK) {
        s_backlight = bl;
        ESP_LOGI(TAG, "loaded backlight=%u", bl);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "backlight not stored, using default=%u", s_backlight);
    } else {
        ESP_LOGW(TAG, "failed to load backlight: 0x%x (using default)", err);
    }
    return ESP_OK;
}

uint8_t backlight_storage_get(void)
{
    return s_backlight;
}

esp_err_t backlight_storage_set(uint8_t duty)
{
    if (duty == s_backlight) {
        return ESP_OK;  /* 无变化不写 NVS */
    }
    s_backlight = duty;
    esp_err_t err = persist_set_u8(NS_BACKLIGHT, KEY_BACKLIGHT, duty);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "persist backlight failed: 0x%x", err);
    } else {
        ESP_LOGI(TAG, "backlight saved=%u", duty);
    }
    return err;
}
