idf_component_register(
    SRCS
        # === JS 侧三层：控制 / 引擎 / API ===
        "dynamic_app.c"               # 控制层：生命周期 + script_task 主循环
        "dynamic_app_runtime.c"       # 引擎层：JSContext 创建/销毁/eval
        "dynamic_app_natives.c"       # API 层：sys.* 所有 native fn
        "dynamic_app_registry.c"      # 注册表：app 名 → 脚本 buffer

        # === UI 侧三层：调度 / 注册表 / 样式 ===
        "dynamic_app_ui.c"            # 调度层：drain 分发 + 入队 API
        "dynamic_app_ui_registry.c"   # 注册表层：UTF-8 + id↔obj 映射
        "dynamic_app_ui_styles.c"     # 样式层：apply_style 20-key 分发
    INCLUDE_DIRS
        "."
    EMBED_TXTFILES
        # 标准库 prelude，runtime 在每次启动 app 前自动 eval 一次
        # 这是动态 app 系统唯一内嵌的脚本：runtime 启动不依赖 FS。
        # 业务 app 一律走 LittleFS（storage/littlefs/dynapp_script_store），
        # 通过 BLE 上传写入 /littlefs/apps/<name>.js。
        "scripts/prelude.js"
    REQUIRES
        freertos
        esp_timer
        lvgl
        esp-mquickjs
        storage
        services
)
