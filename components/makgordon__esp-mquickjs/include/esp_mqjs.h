#pragma once

#include <stddef.h>
#include "mquickjs.h"

#ifdef __cplusplus
extern "C" {
#endif

void esp_mqjs_run_script(const char *script);

/* 返回 esp-mquickjs 内置 stdlib 定义（只读使用）。 */
const JSSTDLibraryDef *esp_mqjs_get_stdlib_def(void);

/* 返回 stdlib 的 C function table 项数，用于运行时追加 native API。 */
size_t esp_mqjs_get_stdlib_c_function_count(void);

#ifdef __cplusplus
}
#endif
