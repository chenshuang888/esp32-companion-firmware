#include "touch_ft5x06.h"

#include <string.h>

#include "board_config.h"
#include "esp_check.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_log.h"

static const char *TAG = "touch_ft5x06";

static esp_lcd_touch_handle_t s_touch_handle = NULL;

esp_err_t touch_ft5x06_init(void)
{
    i2c_master_bus_handle_t i2c_handle;
    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = BOARD_TOUCH_I2C_PORT,
        .sda_io_num = BOARD_TOUCH_PIN_SDA,
        .scl_io_num = BOARD_TOUCH_PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = true },
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &i2c_handle), TAG, "i2c init failed");

    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    tp_io_config.scl_speed_hz = BOARD_TOUCH_I2C_FREQ_HZ;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(i2c_handle, &tp_io_config, &tp_io_handle), TAG, "touch io init failed");

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = BOARD_LCD_H_RES,
        .y_max = BOARD_LCD_V_RES,
        .rst_gpio_num = BOARD_TOUCH_PIN_RST,
        .int_gpio_num = BOARD_TOUCH_PIN_INT,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_ft5x06(tp_io_handle, &tp_cfg, &s_touch_handle), TAG, "touch init failed");

    ESP_LOGI(TAG, "Touch initialized");
    return ESP_OK;
}

bool touch_ft5x06_read(touch_ft5x06_point_t *point)
{
    if (!s_touch_handle || !point) {
        return false;
    }

    memset(point, 0, sizeof(*point));
    esp_lcd_touch_read_data(s_touch_handle);

    esp_lcd_touch_point_data_t points[1] = {0};
    uint8_t count = 0;
    if (esp_lcd_touch_get_data(s_touch_handle, points, &count, 1) != ESP_OK || count == 0) {
        point->pressed = false;
        return false;
    }

    point->pressed = true;
    point->x = points[0].x;
    point->y = points[0].y;
    point->strength = points[0].strength;
    point->count = count;
    return true;
}
