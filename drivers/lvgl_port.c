#include "lvgl_port.h"

#include <assert.h>

#include "board_config.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "lcd_panel.h"
#include "lvgl.h"
#include "touch_ft5x06.h"

static lv_display_t *s_display = NULL;

static uint32_t lvgl_tick_get_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void lvgl_flush_ready_cb(void *user_ctx)
{
    lv_display_t *display = (lv_display_t *)user_ctx;
    lv_display_flush_ready(display);
}

static void lvgl_flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *px_map)
{
    (void)display;
    ESP_ERROR_CHECK(lcd_panel_draw_bitmap(area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map));
}

static void lvgl_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;

    touch_ft5x06_point_t point;
    if (!touch_ft5x06_read(&point) || !point.pressed) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    int px = point.x;
    int py = point.y;

    if (BOARD_LCD_MIRROR_X) {
        px = BOARD_LCD_H_RES - 1 - px;
    }
    if (BOARD_LCD_MIRROR_Y) {
        py = BOARD_LCD_V_RES - 1 - py;
    }

    if (px < 0) px = 0;
    if (py < 0) py = 0;
    if (px >= BOARD_LCD_H_RES) px = BOARD_LCD_H_RES - 1;
    if (py >= BOARD_LCD_V_RES) py = BOARD_LCD_V_RES - 1;

    data->point.x = (lv_coord_t)px;
    data->point.y = (lv_coord_t)py;
    data->state = LV_INDEV_STATE_PRESSED;
}

esp_err_t lvgl_port_init(void)
{
    lv_init();
    lv_tick_set_cb(lvgl_tick_get_ms);

    s_display = lv_display_create(BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    if (!s_display) {
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(lcd_panel_init(), "lvgl_port", "lcd init failed");
    lcd_panel_register_tx_done_cb(lvgl_flush_ready_cb, s_display);
    ESP_RETURN_ON_ERROR(touch_ft5x06_init(), "lvgl_port", "touch init failed");

    lv_display_set_color_format(s_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_display, lvgl_flush_cb);

    size_t buf_size = BOARD_LCD_H_RES * BOARD_LVGL_BUFFER_LINES * sizeof(uint16_t);
    void *buf1 = heap_caps_malloc(buf_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    void *buf2 = heap_caps_malloc(buf_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    assert(buf1 && buf2);
    lv_display_set_buffers(s_display, buf1, buf2, buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, lvgl_touch_read_cb);

    return ESP_OK;
}

void lvgl_port_task_handler(void)
{
    lv_timer_handler();
}
