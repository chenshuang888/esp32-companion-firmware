#pragma once

#include "app_router.h"

/* 设置 app —— 内部 sub_router 串起多 page */
extern const app_descriptor_t SETTINGS_APP;

typedef enum {
    SETTINGS_PAGE_HOME = 0,
    SETTINGS_PAGE_BLUETOOTH,
    SETTINGS_PAGE_DISPLAY,
    SETTINGS_PAGE_TIME,
    SETTINGS_PAGE_ABOUT,
} settings_page_id_t;

void settings_app_push(settings_page_id_t id);
void settings_app_pop_or_exit(void);   /* 栈空则 exit_to_launcher */
