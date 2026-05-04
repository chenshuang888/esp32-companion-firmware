#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * dynapp_upload_manager —— BLE 上传协议状态机（IDLE / RECEIVING）
 *
 * 角色：
 *   - 解 BLE rx 帧 → 跑 START/CHUNK/END/DELETE/LIST 状态机
 *   - 把真正的文件 IO 委派给 dynapp_fs_worker（独立 task 串行执行）
 *   - 通过 status_cb 把结果反馈给 service 层（BLE notify）
 *
 * 自身不持有任何 task/queue：状态字段只在 BLE host task 上下文访问，
 * 无锁；fs_worker 的 done callback 只携带预先 packed 的 (op,seq)，
 * 不读状态字段，所以两个执行流互不干扰。
 *
 * 不依赖 BLE / NimBLE 头文件（service 层负责拆帧后调 submit_*）。
 */

#define DYNAPP_UPLOAD_NAME_MAX     15
#define DYNAPP_UPLOAD_FNAME_MAX    31

typedef enum {
    UPL_RESULT_OK              = 0,
    UPL_RESULT_BAD_FRAME       = 1,
    UPL_RESULT_NO_SESSION      = 2,
    UPL_RESULT_TOO_LARGE       = 3,
    UPL_RESULT_CRC_MISMATCH    = 4,
    UPL_RESULT_FS_ERROR        = 5,
    UPL_RESULT_BUSY            = 6,
} upload_result_t;

typedef enum {
    UPL_OP_START   = 0x01,
    UPL_OP_CHUNK   = 0x02,
    UPL_OP_END     = 0x03,
    UPL_OP_DELETE  = 0x10,
    UPL_OP_LIST    = 0x11,
} upload_op_t;

/**
 * manager → 上层的状态回调。
 * - op:        触发本次状态的 op
 * - result:    UPL_RESULT_*
 * - seq:       触发本次状态的请求 seq（service 用它做 BLE notify echo）
 * - name:      START/END/DELETE 时为 app_id，CHUNK/LIST 时可能 NULL
 * - extra:     CHUNK 成功时为 next_expected_offset，LIST 时为条数，否则 0
 * - list_buf:  仅 LIST OK 时非 NULL，指向 NUL 分隔的 app_id 列表
 * - list_len:  list_buf 字节数
 *
 * 实现要求：不阻塞、可调 NimBLE notify（线程安全）。
 * 调用线程：CHUNK ack 在 BLE host task；END/DELETE/LIST 完成在 fs_worker task。
 */
typedef void (*upload_status_cb_t)(upload_op_t op,
                                    upload_result_t result,
                                    uint8_t seq,
                                    const char *name,
                                    uint32_t extra,
                                    const uint8_t *list_buf,
                                    size_t list_len);

esp_err_t dynapp_upload_manager_init(upload_status_cb_t status_cb);

/**
 * 追加注册一个 status 观察者（多消费者）。
 *
 * - 容量上限 DYNAPP_UPLOAD_MAX_STATUS_CBS（含 init 时注册的那一个）
 * - 同一个函数指针重复注册：no-op，返回 ESP_OK（idempotent）
 * - 调用线程契约同 init cb（manager / fs_worker，禁阻塞 / 禁碰 LVGL）
 *
 * 典型用例：菜单页在 create 时注册一个观察者（仅 set 一个 dirty flag），
 * 在 UI 线程的 update tick 里再做真正的列表重建。
 */
#define DYNAPP_UPLOAD_MAX_STATUS_CBS  4
esp_err_t dynapp_upload_manager_register_status_cb(upload_status_cb_t cb);

/* ---- 提交接口（service 用，全部非阻塞）----
 *
 * START.app_slash_file = "<app_id>/<filename>"，例 "alarm/main.js"。
 */
bool dynapp_upload_submit_start (const char *app_slash_file,
                                 uint32_t total_len, uint32_t crc32,
                                 uint8_t seq);
bool dynapp_upload_submit_chunk (uint32_t offset, const uint8_t *data, uint16_t len,
                                 uint8_t seq);
bool dynapp_upload_submit_end   (uint8_t seq);
bool dynapp_upload_submit_delete(const char *app_id, uint8_t seq);
bool dynapp_upload_submit_list  (uint8_t seq);

#ifdef __cplusplus
}
#endif
