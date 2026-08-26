# Command Executor

Execution path chung cho mọi lệnh từ REST/MCP. HTTPD task không chờ BLE ACK.
Flow: `submit(msg, completion, ctx)` → queue → worker → `command_dispatcher_handle()` → completion callback trả response.

## API

```c
esp_err_t command_executor_init(void);            // single-shot; gọi sau ble_central_init(), trước web_server_start()
esp_err_t command_executor_submit(const gw_message_t *msg,
                                  command_completion_fn completion, void *ctx);
void      command_executor_deinit(void);          // drain backlog qua poison pill rồi mới free
void      command_executor_get_stats(command_executor_stats_t *stats, uint32_t *worker_stack_min_bytes);
```

Submit chỉ enqueue: `ESP_OK` | `ESP_ERR_NO_MEM` (queue full) | `ESP_ERR_INVALID_STATE` | `ESP_ERR_INVALID_ARG`.

## Contract bắt buộc

- `result` trong completion **chỉ valid lúc callback chạy** — không lưu/free, cần giữ phải copy.
- Queue full → HTTP **503** ngay · job quá deadline → `DISPATCH_STATUS_TIMEOUT`, không gửi BLE → **504** · device busy (per-device pending) → **409**.
- Deadline e2e đo từ submit, khác `DISPATCHER_ACK_TIMEOUT_MS` (chỉ send→ACK).

## Kconfig (default)

| Symbol | Default |
|---|---|
| `CONFIG_CMD_EXEC_WORKER_COUNT` | 2 (mỗi worker = 1 task + result 4.1 KB BSS) |
| `CONFIG_CMD_EXEC_QUEUE_LEN` | 2 |
| `CONFIG_CMD_EXEC_WORKER_STACK` | 4096 |
| `CONFIG_CMD_EXEC_JOB_TIMEOUT_MS` | 3000 |

Tuning theo metrics `/api/status` → `"executor"` (`max_queue_depth`, `max_queue_wait_ms`, `worker_stack_min_bytes`). Stats RAM-only.

Tests: `test/` project, Unity trên target (đã có trong `TEST_COMPONENTS`).
