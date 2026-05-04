#pragma once
#include "app_router.h"
extern const app_descriptor_t LAUNCHER_APP;

/* 启动时一次性初始化（注册 upload_manager 观察者等）。
 * 在 app_router_register 之后、app_router_enter 之前调一次。 */
void launcher_app_module_init(void);
