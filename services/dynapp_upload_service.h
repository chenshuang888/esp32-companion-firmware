#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * dynapp_upload_service —— 动态 App 上传 / 运维 BLE service
 *
 * GATT 表：
 *   Service: a3a40001-0000-4aef-b87e-4fa1e0c7e0f6
 *     ├─ rx     a3a40002 WRITE     PC → ESP，上传/控制帧
 *     └─ status a3a40003 NOTIFY    ESP → PC，进度/完成/错误
 *
 * 帧格式（rx, ≤200B）：
 *   [1B op][1B seq][2B payload_len LE][payload...]
 *
 *   op 0x01 START   payload = path(31B,NUL pad)+total_len(4B LE)+crc32(4B LE) = 39B
 *                   path = "<app_id>/<filename>" 例 "alarm/main.js"
 *   op 0x02 CHUNK   payload = offset(4B LE) + data(...)
 *   op 0x03 END     payload = (空)
 *   op 0x10 DELETE  payload = app_id(15B,NUL pad) = 15B  （删整个 app 目录）
 *   op 0x11 LIST    payload = (空)
 *
 * 帧格式（status notify, ≤200B）：
 *   [1B op_echo][1B result][1B seq_echo][1B reserved][payload...]
 *   payload 因 op 而异：CHUNK → next_offset(4B LE); LIST → NUL 分隔的 name 列表
 *
 * 跟其它 service 一样：access_cb 不阻塞、不写 FS、不碰 LVGL；
 * 实际工作交给 dynapp_upload_manager 在自己的 consumer task 里做。
 */

esp_err_t dynapp_upload_service_init(void);

#ifdef __cplusplus
}
#endif
