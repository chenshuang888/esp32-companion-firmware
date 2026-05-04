#pragma once

#include "esp_err.h"

esp_err_t lvgl_port_init(void);
void lvgl_port_task_handler(void);
