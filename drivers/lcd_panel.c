#include "lcd_panel.h"

#include <assert.h>

#include "board_config.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"

typedef struct {
    lcd_panel_tx_done_cb_t tx_done_cb;
    void *tx_done_user_ctx;
} lcd_panel_callback_ctx_t;

static const char *TAG = "lcd_panel";

static esp_lcd_panel_handle_t s_panel_handle = NULL;
static uint8_t s_backlight_duty = BOARD_LCD_BACKLIGHT_DEFAULT;
static uint8_t s_backlight_saved = BOARD_LCD_BACKLIGHT_DEFAULT;
static bool s_display_on = true;
static lcd_panel_callback_ctx_t s_callback_ctx = {0};

static bool lcd_panel_io_color_trans_done(esp_lcd_panel_io_handle_t panel_io,
                                          esp_lcd_panel_io_event_data_t *edata,
                                          void *user_ctx)
{
    (void)panel_io;
    (void)edata;

    lcd_panel_callback_ctx_t *ctx = (lcd_panel_callback_ctx_t *)user_ctx;
    if (ctx && ctx->tx_done_cb) {
        ctx->tx_done_cb(ctx->tx_done_user_ctx);
    }
    return false;
}

static void lcd_panel_backlight_init(void)
{
    ledc_timer_config_t timer_conf = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_conf));

    ledc_channel_config_t channel_conf = {
        .gpio_num = BOARD_LCD_PIN_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = s_backlight_duty,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel_conf));
}

void lcd_panel_register_tx_done_cb(lcd_panel_tx_done_cb_t tx_done_cb, void *user_ctx)
{
    s_callback_ctx.tx_done_cb = tx_done_cb;
    s_callback_ctx.tx_done_user_ctx = user_ctx;
}

esp_err_t lcd_panel_init(void)
{
    spi_bus_config_t buscfg = {
        .mosi_io_num = BOARD_LCD_PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = BOARD_LCD_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BOARD_LCD_H_RES * BOARD_LCD_V_RES * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BOARD_LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO), TAG, "spi init failed");

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = BOARD_LCD_PIN_CS,
        .dc_gpio_num = BOARD_LCD_PIN_DC,
        .spi_mode = 0,
        .pclk_hz = BOARD_LCD_SPI_FREQ_HZ,
        .trans_queue_depth = 10,
        .on_color_trans_done = lcd_panel_io_color_trans_done,
        .user_ctx = &s_callback_ctx,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_LCD_SPI_HOST, &io_config, &io_handle), TAG, "panel io init failed");

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BOARD_LCD_PIN_RST,
        .rgb_endian = LCD_RGB_ENDIAN_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io_handle, &panel_config, &s_panel_handle), TAG, "panel create failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel_handle), TAG, "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel_handle), TAG, "panel init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel_handle, BOARD_LCD_COLOR_INVERT), TAG, "panel invert failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel_handle, BOARD_LCD_MIRROR_X, BOARD_LCD_MIRROR_Y), TAG, "panel mirror failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel_handle, BOARD_LCD_SWAP_XY), TAG, "panel swap failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel_handle, true), TAG, "panel on failed");

    lcd_panel_backlight_init();
    s_display_on = true;

    ESP_LOGI(TAG, "LCD initialized (%dx%d)", BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    return ESP_OK;
}

esp_err_t lcd_panel_draw_bitmap(int x_start, int y_start, int x_end, int y_end, const void *color_data)
{
    if (!s_panel_handle) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_lcd_panel_draw_bitmap(s_panel_handle, x_start, y_start, x_end, y_end, color_data);
}

void lcd_panel_set_backlight(uint8_t duty)
{
    s_backlight_duty = duty;
    if (duty > 0) {
        s_backlight_saved = duty;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

uint8_t lcd_panel_get_backlight(void)
{
    return s_backlight_duty;
}

void lcd_panel_set_display_on(bool on)
{
    s_display_on = on;
    if (s_panel_handle) {
        esp_lcd_panel_disp_on_off(s_panel_handle, on);
    }
}

void lcd_panel_set_screen_on(bool on)
{
    if (on) {
        lcd_panel_set_display_on(true);
        lcd_panel_set_backlight(s_backlight_saved == 0 ? BOARD_LCD_BACKLIGHT_DEFAULT : s_backlight_saved);
    } else {
        if (s_backlight_duty > 0) {
            s_backlight_saved = s_backlight_duty;
        }
        lcd_panel_set_backlight(0);
        lcd_panel_set_display_on(false);
    }
}
