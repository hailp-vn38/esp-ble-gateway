# Kế hoạch triển khai Web Server Refactor v2

> Thực thi `components/web_server/Web Server Refactor & Development Plan v2.md`.
> Tổng thời lượng ước tính: ~15 ngày làm việc (đã gồm buffer).
> Mỗi giai đoạn là một PR riêng, firmware luôn buildable (§95).

---

## Điều kiện tiên quyết

- Hardware: ESP32-S3 + thiết bị peripheral quảng bá service `0xABF0` (bắt buộc cho test BLE/ACK).
- Env: `idf.py` từ `/Users/lamphuchai/.espressif/v5.4.4/esp-idf`,
  `git submodule update --init --recursive`.
- Nhớ: project `test/` phải `set-target esp32s3` + build riêng; `MINIMAL_BUILD` →
  module mới phải nằm trong chuỗi `REQUIRES`.

---

## Giai đoạn 0 — Chuẩn bị + quick wins (0.5 ngày)

**Branch:** `refactor/p0-executor`

Chốt 5 quyết định trước khi code (decision record):

| # | Quyết định | Chọn |
|---|---|---|
| D1 | Worker count / queue length | 2 workers / queue 2, Kconfig `CONFIG_CMD_EXEC_*` để benchmark sau (§13-14) |
| D2 | Vị trí executor | Component riêng `components/command_executor` |
| D3 | Deadline semantics | Queue-full → 503; accepted nhưng expired → 504 (§50) |
| D4 | `mcp_async` | Phương án A — xóa hẳn (§8) |
| D5 | `error.code` machine-readable | Để P2, giữ response schema ổn định (§92) |

**Quick wins:**

1. Fix race `s_log_snapshot`: thêm mutex quanh `log_buffer_get_recent` +
   đọc snapshot trong `web_system_api.c` (esp_http_server chạy handler đồng thời
   trên nhiều session → 2 GET `/api/logs` song song ghi chung buffer tĩnh).
2. Đặt tên constant route capacity trong `web_server.c` kèm comment headroom,
   thay magic numbers `18/12288/12/8192` (§40).

## Giai đoạn 1 — BLE scan `esp_timer` (P0, 1.5 ngày) — làm TRƯỚC executor

Lý do đảo thứ tự so với §98: bug use-after-free trong `web_ble_api.c` (handler
detach handle dưới mutex rồi `vTaskDelete()` ngoài lock; worker có thể tự delete
trước đó) là memory-unsafe; file nhỏ, độc lập hoàn toàn.

Sửa `web_ble_api.c`:

- **Xóa:** `s_scan_stop_task`, `detach_stop_task()`, `ble_scan_stop_worker()`,
  mọi `vTaskDelete()` từ handler.
- **Thêm:** `s_scan_generation` (uint32) + `s_scan_active` + esp_timer one-shot
  6000 ms.
- POST scan: `generation++`, arm timer. DELETE: `generation++`, disarm.
- Timer callback: chỉ check generation + active rồi `ble_central_scan_stop()` —
  không malloc/JSON/lock dài (§24).
- NimBLE host reset giữa chừng → sync `s_scan_active` (mục cuối §26).

**Test (§79, trên hardware):** POST → GET giữa chừng → auto-stop ~6s · DELETE
trước timeout · POST ngay sau DELETE · stale callback · host reset khi đang scan.
Xác nhận không còn task `ble_scan_stop`.

## Giai đoạn 2 — Command Executor (P0 core, 4 ngày)

Theo §72 Step 1-3, chưa đụng Web/MCP:

1. `command_executor.h`: `init()` / `submit(msg, completion, ctx)` / `get_stats()` (§9).
2. `command_job_t`: message + completion + context + `submitted_at_us`/`deadline_us`
   — không chứa `dispatch_result_t` (§10).
3. Persistent workers, mỗi worker sở hữu `dispatch_result_t` riêng trong BSS (§11)
   → stack worker nhỏ (~3 KB).
4. Queue bounded; dequeue xong check deadline — expired trả completion
   `EXECUTOR_EXPIRED` mà không gửi BLE (§16).
5. Stats struct §18 (RAM only); shutdown sequence §69.
6. `main.c`: init sau `ble_central`, trước `web_server` (§70); fail → không đăng
   ký route command.
7. CMakeLists: thêm `REQUIRES command_executor` cho web_server + mcp_endpoint.

**Unit test** (fake dispatcher hook, case §76): thêm `"command_executor"` vào
`TEST_COMPONENTS` trong `test/CMakeLists.txt`.

## Giai đoạn 3 — Migrate REST `/api/command` (1.5 ngày)

Sửa `web_gateway_api.c`:

- Xóa: `command_http_worker`, `s_command_slots`, calloc context, semaphore logic.
- Handler mới: `async_handler_begin` → `submit()` với completion = map status qua
  `http_status_for()` → send JSON → `async_handler_complete`.
- Submit fail (queue full) → 503 ngay tại handler.

**Stress (§82/84/85):** vài trăm đến 1000 lệnh liên tục — heap không giảm
monotonic; 2 lệnh không-ACK đồng thời + poll status/logs vẫn responsive.

## Giai đoạn 4 — Migrate MCP, xóa `mcp_async` (2.5 ngày)

- `mcp_endpoint`: `tools/call` device_command → async begin → executor submit →
  completion format JSON-RPC → complete (§67).
- Xóa `mcp_async.c`, dọn `mcp_endpoint_internal.h` + `CONFIG_MCP_ASYNC_STACK`.
- Cross-transport tests (§77):
  - REST device A ∥ MCP device B → cả hai qua cùng executor.
  - REST device A ∥ MCP device A → một bên nhận 409 busy.
- Socket pressure (§78): dashboard + polling + queue đầy → UI không mất socket.

Gate: chạy hết Definition of Done P0 (§88) trước khi merge.

## Giai đoạn 5 — HTTP body correctness (P1, 2.5 ngày)

Sửa `web_http.c`:

1. `read_request_body`: absolute deadline `WEB_BODY_RECEIVE_TIMEOUT_MS` thay
   vòng `continue` vô hạn.
2. `web_body_status_t` (§29) + mapping 400/408/413/500 (§31).
3. Oversized → 413 + `Connection: close` (§33).
4. Limits per endpoint (§34): device 512 / command 1024 / wifi 256; MCP giữ 4096.

**Test (§80):** empty · malformed · đúng max · max+1 · slow client · repeated
timeout · disconnect · oversize + keep-alive (request kế tiếp không parse sai).

## Giai đoạn 6 — Services + route headroom (P1, 2.5 ngày)

1. `gateway_status_get()` — single source; `/api/status` và dispatcher
   `get_status` cùng gọi (§45-47).
2. Device mutations POST/PUT/DELETE qua executor, GET giữ sync (§43).
3. Route headroom: Gateway hiện 17 routes → capacity ≥ 21; Provisioning 12 → 14.
4. (Tuỳ chọn) `error.code` machine-readable §51-52.

Gate: Definition of Done P1 (§89).

## Giai đoạn 7 — P2 (theo nhu cầu, ~2 ngày)

Metrics §53-54 (queue_wait/dispatch/total) · Stack HWM toàn bộ task §55 ·
Heap soak §56 · Security headers: `nosniff`, `Referrer-Policy`, mở rộng CSP sang
dashboard.

---

## Việc cần làm NGAY

```
1. git checkout -b refactor/p0-executor
2. Chốt D1–D5
3. PR #1: quick wins (log mutex + route constants)
4. PR #2: BLE esp_timer + generation
5. Bắt tay viết command_executor.h (chưa migrate gì)
```

---

## Trạng thái thực thi

| Giai đoạn | Trạng thái | PR/Ghi chú |
|---|---|---|
| GĐ0 Quick wins | ✅ Code xong | Mutex `s_log_snapshot`; constants `WEB_GATEWAY_*` / `WEB_PROVISIONING_*` (headroom 17→21, 12→14) |
| GĐ1 BLE esp_timer | ✅ Đã test trên board | POST/GET/DELETE/auto-stop ~6s ✓; DELETE→POST ngay: scan mới sống sót (stale callback hết hiệu lực) |
| GĐ2 command_executor | ✅ Đã test trên board | 4/4 unit test PASS; fix bug stats decrement trong quá trình test |
| GĐ3 Migrate REST | ✅ Đã test trên board | 400 sync / 502 async NOT_CONNECTED đúng mapping; burst 90 lệnh, heap ổn định (94868→94324→94508); status/logs responsive |
| GĐ4 Migrate MCP + xóa mcp_async | ⏳ | |
| GĐ5 HTTP body correctness | ⏳ | |
| GĐ6 Services + headroom | ⏳ | Route headroom đã nâng sẵn ở GĐ0 |
| GĐ7 P2 metrics/tuning | ⏳ | |

### Việc cần làm tay (cần hardware)

```sh
# Chạy unit test executor trên board
cd test && idf.py -p <PORT> flash monitor   # chọn command_executor tests

# Test BLE scan (§79) — flash firmware chính rồi:
# POST /api/ble/scan → GET giữa chừng → auto-stop ~6s
# DELETE trước timeout → POST ngay sau DELETE → xác nhận scan mới không bị stop oan
```

### Ghi chú phát hiện thêm khi thực hiện

- `sdkconfig.defaults` có dòng `HTTPD_RECV_TIMEOUT_SEC` không tồn tại trong IDF 5.4.4
  (Kconfig warning lúc build) — symbol đúng là `HTTPD_RECV_WAIT_TIMEOUT`? Cần dọn ở GĐ5.
- `components/web_server/README.md` còn mô tả kiến trúc cũ (task-per-request,
  `COMMAND_WORKER_COUNT`) — cần cập nhật sau GĐ4.
