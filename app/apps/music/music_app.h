#pragma once
#include "app_router.h"

extern const app_descriptor_t MUSIC_APP;

typedef enum {
    MUSIC_PAGE_LIST   = 0,
    MUSIC_PAGE_DETAIL = 1,
} music_page_id_t;

/* 子页跳转 API（页面内部调用） */
void music_app_push(music_page_id_t id);
void music_app_pop_or_exit(void);
