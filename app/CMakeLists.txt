idf_component_register(
    SRCS
        "app_main.c"
        "app_fonts.c"
        "weather_icons.c"
        # UI 设计系统（token + widgets + anim + shell helper）
        "ui/ui_widgets.c"
        "ui/ui_anim.c"
        "ui/ui_statusbar.c"
        "ui/ui_modal_card.c"
        "ui/app_shell_ui.c"
        # ---- Apps（每个目录 = 一个用户感知的应用）----
        "apps/lockscreen/lockscreen_app.c"
        "apps/lockscreen/pages/page_lockscreen.c"

        "apps/launcher/launcher_app.c"
        "apps/launcher/pages/page_launcher.c"
        "apps/launcher/pages/launcher_modal.c"

        "apps/weather/weather_app.c"
        "apps/weather/pages/page_weather.c"

        "apps/notifications/notifications_app.c"
        "apps/notifications/pages/page_notifications.c"

        "apps/music/music_app.c"
        "apps/music/pages/page_music_list.c"
        "apps/music/pages/page_music_detail.c"

        "apps/system/system_app.c"
        "apps/system/pages/system_ui_common.c"
        "apps/system/pages/page_system_pc.c"
        "apps/system/pages/page_system_device.c"

        "apps/settings/settings_app.c"
        "apps/settings/pages/settings_home.c"
        "apps/settings/pages/settings_bluetooth.c"
        "apps/settings/pages/settings_display.c"
        "apps/settings/pages/settings_time.c"
        "apps/settings/pages/settings_about.c"

        "apps/dynapp_host/dynapp_host_app.c"
        "apps/dynapp_host/pages/page_dynapp_host.c"

        "apps/clock/clock_app.c"
        "apps/clock/pages/clock_ui_common.c"
        "apps/clock/pages/page_alarms.c"
        "apps/clock/pages/page_world.c"
        "apps/clock/pages/page_stopwatch.c"
        "apps/clock/pages/page_timer.c"
        "apps/clock/pages/page_alarm_edit.c"
    INCLUDE_DIRS
        "."
        "ui"
        "apps/lockscreen" "apps/lockscreen/pages"
        "apps/launcher"   "apps/launcher/pages"
        "apps/weather"    "apps/weather/pages"
        "apps/notifications" "apps/notifications/pages"
        "apps/music"      "apps/music/pages"
        "apps/system"     "apps/system/pages"
        "apps/settings"   "apps/settings/pages"
        "apps/dynapp_host" "apps/dynapp_host/pages"
        "apps/clock"      "apps/clock/pages"
    EMBED_FILES
        "fonts/srhs_sc_subset.ttf"
        "fonts/material_icons_subset.ttf"
        "icons/ic_clear.bin"
        "icons/ic_cloudy.bin"
        "icons/ic_overcast.bin"
        "icons/ic_rain.bin"
        "icons/ic_snow.bin"
        "icons/ic_fog.bin"
        "icons/ic_thunder.bin"
        "icons/ic_unknown.bin"
    REQUIRES
        framework
        drivers
        services
        storage
        dynamic_app
        lvgl
        app_update
        spi_flash
        esp_hw_support
)
