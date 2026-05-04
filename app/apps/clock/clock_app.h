#pragma once
#include "app_router.h"

extern const app_descriptor_t CLOCK_APP;

typedef enum {
    CLOCK_PAGE_ALARMS = 0,
    CLOCK_PAGE_WORLD,
    CLOCK_PAGE_STOPWATCH,
    CLOCK_PAGE_TIMER,
} clock_page_id_t;

void              clock_app_switch_to(clock_page_id_t id);
void              clock_app_exit(void);
clock_page_id_t   clock_app_current_page(void);

/* alarm edit 子页用 push/pop（不在 tab 栏）*/
void clock_app_open_alarm_edit(void);
void clock_app_close_alarm_edit(void);
