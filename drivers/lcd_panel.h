#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef void (*lcd_panel_tx_done_cb_t)(void *user_ctx);

esp_err_t lcd_panel_init(void);
void lcd_panel_register_tx_done_cb(lcd_panel_tx_done_cb_t tx_done_cb, void *user_ctx);
esp_err_t lcd_panel_draw_bitmap(int x_start, int y_start, int x_end, int y_end, const void *color_data);

void lcd_panel_set_backlight(uint8_t duty);
uint8_t lcd_panel_get_backlight(void);
void lcd_panel_set_display_on(bool on);
void lcd_panel_set_screen_on(bool on);
