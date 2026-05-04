idf_component_register(
    SRCS
        # NVS 子层（小 KV，settings/notifications/saveState）
        "nvs/persist.c"
        "nvs/backlight_storage.c"
        "nvs/time_storage.c"
        "nvs/notify_storage.c"
        # LittleFS 子层（KB-MB 级文件，动态 App 脚本仓 + 未来日志/缓存）
        "littlefs/fs_littlefs.c"
        "littlefs/dynapp_script_store.c"
        "littlefs/dynapp_fs_worker.c"
    INCLUDE_DIRS
        # 保持调用方 `#include "persist.h"` / `#include "fs_littlefs.h"` 不变
        "nvs"
        "littlefs"
    REQUIRES
        nvs_flash
        esp_timer
        joltwallet__littlefs
        freertos
)
