#pragma once
#include "app_router.h"

extern const app_descriptor_t SYSTEM_APP;

typedef enum {
    SYSTEM_PAGE_PC     = 0,
    SYSTEM_PAGE_DEVICE = 1,
} system_page_id_t;

/* 子页内部调用 */
void system_app_switch_to(system_page_id_t id);
void system_app_exit(void);   /* 上滑退出回 launcher */

/* 当前页（让 tabbar 高亮 + 子页判断方向）*/
system_page_id_t system_app_current_page(void);
