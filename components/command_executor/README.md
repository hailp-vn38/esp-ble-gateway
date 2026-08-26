# Command Executor

## 1. Tổng quan

`command_executor` là **execution path chung duy nhất** cho mọi lệnh điều khiển từ transport (REST `/api/command`, `/api/devices` mutations, MCP `device_command`).

```text
REST / MCP (transport)
      │  async_handler_begin
      ▼
command_executor_submit(msg, completion, ctx)
      │  bounded queue
      ▼
persistent workers ──► command_dispatcher_handle() ──► BLE / registry
      │
      ▼
completion(result, ctx)  ──► format response ──► async_handler_complete()
```

Nguyên tắc (Plan v2):

* HTTPD task **không bao giờ** chờ BLE ACK — chỉ parse + enqueue.
* Không tạo FreeRTOS task per request; workers persistent, memory deterministic.
* Mỗi worker sở hữu `dispatch_result_t` riêng trong BSS (~4.1 KB), stack nhỏ.
* Queue bounded; job quá hạn bị expire mà **không gửi BLE**.

---

## 2. Các file chính

```text
components/command_executor/
├── command_executor.c            # Queue, workers, deadline, stats
├── include/command_executor.h    # Public API
├── Kconfig.projbuild             # Worker count / queue len / stack / deadline
└── test/test_command_executor.c  # Unity tests (4 cases, chạy trên target)
```

---

## 3. Public API

| Hàm | Mô tả |
|---|---|
| `command_executor_init()` | Single-shot. Tạo queue + workers. Gọi sau `ble_central_init()`, trước `web_server_start()` |
| `command_executor_submit(msg, completion, ctx)` | Chỉ enqueue. Trả `ESP_OK` / `ESP_ERR_NO_MEM` (queue full) / `ESP_ERR_INVALID_STATE` / `ESP_ERR_INVALID_ARG` |
| `command_executor_deinit()` | Dừng nhận job → poison pill (backlog chạy nốt) → đợi workers thoát → giải phóng |
| `command_executor_get_stats(&stats, &stack_min)` | Copy metrics + HWM nhỏ nhất của workers |

**Result lifetime:** pointer `dispatch_result_t *result` chỉ valid **trong lúc callback chạy**. Không lưu, không free, cần giữ lâu hơn phải copy.

---

## 4. Deadline & saturation semantics

| Trường hợp | Kết quả | HTTP |
|---|---|---|
| Queue full khi submit | `ESP_ERR_NO_MEM` ngay tại handler | **503** |
| Job nằm queue quá `CMD_EXEC_JOB_TIMEOUT_MS` | Completion với `DISPATCH_STATUS_TIMEOUT`, không dispatch | **504** |
| Dispatcher trả `DISPATCH_STATUS_BUSY` (per-device pending) | Completion bình thường | **409** |

Deadline đo từ lúc submit (e2e), khác với `DISPATCHER_ACK_TIMEOUT_MS` (chỉ đo send→ACK).

---

## 5. Cấu hình (Kconfig)

| Symbol | Default | Ý nghĩa |
|---|---|---|
| `CONFIG_CMD_EXEC_WORKER_COUNT` | 2 | Số worker persistent (1–4). Mỗi worker = 1 task + 4.1 KB BSS result |
| `CONFIG_CMD_EXEC_QUEUE_LEN` | 2 | Độ dài queue admission (1–8). Tính theo socket budget (§14–15) |
| `CONFIG_CMD_EXEC_WORKER_STACK` | 4096 | Stack mỗi worker (bytes) |
| `CONFIG_CMD_EXEC_JOB_TIMEOUT_MS` | 3000 | Deadline e2e của job (ms) |

Tuning dựa trên `/api/status` → `"executor"` metrics (`max_queue_depth`, `max_queue_wait_ms`, `worker_stack_min_bytes`), không chỉnh theo cảm tính.

---

## 6. Metrics

```c
typedef struct {
    uint32_t submitted, completed;
    uint32_t queue_full;        // số lần submit bị từ chối
    uint32_t queue_timeout;     // job expired trước khi dequeue
    uint32_t dispatch_timeout;  // dispatcher tự trả TIMEOUT (ACK)
    uint32_t max_queue_depth;   // depth lớn nhất từng đạt được
    uint32_t max_queue_wait_ms; // latency chờ queue lớn nhất (§54)
    uint32_t active_workers;
} command_executor_stats_t;
```

RAM only, không ghi NVS. Expose qua `/api/status`.

---

## 7. Tests

Unity, chạy trên target (project `test/`, đã thêm vào `TEST_COMPONENTS`): submit hợp lệ + stats · queue-full đồng bộ · job expired bỏ qua dispatch · invalid usage / double init / deinit safety.
