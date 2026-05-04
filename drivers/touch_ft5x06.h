#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool pressed;
    uint16_t x;
    uint16_t y;
    uint16_t strength;
    uint8_t count;
} touch_ft5x06_point_t;

esp_err_t touch_ft5x06_init(void);
bool touch_ft5x06_read(touch_ft5x06_point_t *point);
