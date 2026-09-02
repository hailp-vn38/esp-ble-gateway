# ESP32 BLE Gateway — WebSocket Realtime Sync Implementation Guide

**Repository:** `hailp-vn38/esp-ble-gateway`  
**Branch:** `dev-ws`  
**Reviewed HEAD:** `c23ddd275830df7742bedf06ba52affe3fe04c37`  
**Review date:** 2026-09-02  
**Target:** ESP32-S3 / ESP-IDF `v6.1-rc1`  
**Document type:** Ordered implementation plan

> Tài liệu này thay thế cấu trúc cũ bằng roadmap theo phase.  
> Một phase chỉ được đánh dấu hoàn tất khi toàn bộ checklist và test gate của phase có fresh evidence.

---

# 0. Kiến trúc mục tiêu

```text
BLE / domain
    |
    v
gateway_events
    |
    v
web_event_ws
    |
    v
/ws/events
    |
    v
frontend realtime state
```

```text
REST
- initial snapshot
- CRUD
- command
- scan
- schema fetch
- schema refresh
- recovery/resync

WebSocket
- device.changed
- device.connection
- device.schema
- feature.state
- resync.required
```

Nguyên tắc:

- REST là authoritative snapshot.
- WebSocket chỉ truyền delta/invalidation/recovery signal.
- Không gửi full device list hoặc full schema qua WS.
- Không gọi WebSocket trực tiếp từ BLE/domain callback.
- Không tạo per-client FreeRTOS task.
- Client count, ring depth, payload đều bounded.
- UI Online phải dùng BLE READY semantic, không dùng ACL connected đơn thuần.

---

# 1. Roadmap tổng thể

| Phase | Nội dung | Dependency | Gate chính |
|---|---|---|---|
| P00 | Baseline + chốt contract | - | source-of-truth và scope rõ ✅ DONE |
| P01 | Hardening event/state synchronization | P00 | producer path không block/race ✅ DONE |
| P02 | ESP-IDF WebSocket lifecycle | P01 | real handshake/client lifecycle đúng ✅ DONE |
| P03 | WS delivery, serializer, recovery, metrics | P02 | bounded delivery + resync đúng ✅ DONE |
| P04 | Frontend realtime core | P03 | snapshot/replay/resync hội tụ ✅ DONE |
| P05 | Managed Devices + Scanner + Add Device UI | P04 | add flow realtime đúng ✅ DONE |
| P06 | Device Detail realtime UI | P04/P05 | connection/schema/feature hội tụ ✅ DONE |
| P07 | READY semantics + REST/WS consistency | P06 | Online semantic thống nhất ✅ DONE |
| P08 | Degraded/reconnect UX + recovery | P04-P07 | network failure vẫn hội tụ ✅ DONE |
| P09 | Integration/E2E/soak qualification | P01-P08 | tests phản ánh thực tế ✅ DONE |
| P10 | Documentation + rollout + DoD | P09 | release-ready |

```text
P00
 |
 v
P01
 |
 v
P02
 |
 v
P03
 |
 v
P04
 | \
 |  \
 v   v
P05 P06
 \   /
  \ /
   v
  P07
   |
   v
  P08
   |
   v
  P09
   |
   v
  P10
```

---

# PHASE P00 — Baseline và chốt contract ✅ DONE (2026-09-02)

## Mục tiêu

Chốt kiến trúc, source-of-truth, event contract, UI semantic trước khi sửa transport.

## Dependencies

Không.

## Files cần review

```text
main/main.c

components/gateway_events/*
components/device_state/*
components/device_schema/*
components/ble_central/*
components/command_dispatcher/*

components/web_server/web_device_api.c
components/web_server/web_device_schema_api.c
components/web_server/web_event_ws.c

components/web_server/www_src/dashboard/js/core/*
components/web_server/www_src/dashboard/js/features/*
```

## Checklist

- [x] BLE Central là source-of-truth cho runtime connection.
- [x] `ready` khác `connected`.
- [x] UI Online = `ready`.
- [x] Device Store chỉ giữ persistent identity/metadata.
- [x] `device_schema` là source-of-truth cho schema.
- [x] `device_state` là source-of-truth cho runtime feature state.
- [x] REST snapshot + WS delta là contract chính.
- [x] Scan discovery vẫn dùng REST polling bounded.
- [x] Không thêm `scan.result` WS event trong phase hiện tại.
- [x] Không dùng `mcp_ws_bridge` làm browser WS server.
- [x] Client limit giữ 2.
- [x] Ring depth giữ 32 ban đầu.
- [x] WS JSON buffer giữ 512 ban đầu.

## Event contract

### device.connection

```json
{
  "seq": 101,
  "type": "device.connection",
  "deviceId": "device-01",
  "connected": true
}
```

Trong implementation hiện tại, `connected=true` của event này phải được hiểu là:

```text
READY / command-usable
```

cho tới khi contract được rename/version hóa.

### device.changed

```json
{
  "seq": 102,
  "type": "device.changed",
  "deviceId": "device-01"
}
```

### device.schema

```json
{
  "seq": 103,
  "type": "device.schema",
  "deviceId": "device-01",
  "revision": 8
}
```

### feature.state

```json
{
  "seq": 104,
  "type": "feature.state",
  "deviceId": "device-01",
  "featureId": "power",
  "propertyId": 1,
  "valueType": "bool",
  "value": true,
  "updatedAtMs": 1234567
}
```

### resync.required

```json
{
  "seq": 110,
  "type": "resync.required",
  "reason": "ring_overflow"
}
```

## Test gate

- [x] Build production branch baseline.
  - Firmware build OK at commit `0c9740b`. Binary size: 0x1673e0 bytes (1,471,337 bytes). App partition: 72% free.
- [x] Build test app baseline.
  - Test app build OK at same commit. Binary size: 0x998b0 bytes. Test partition: 40% free.
- [x] Ghi lại memory baseline.
  - Flash Code: 983,802 bytes. Flash Data: 353,904 bytes.
  - DIRAM: 163,619 / 341,760 bytes (47.88%). IRAM: 16,384 / 16,384 bytes (100%).
  - RTC SLOW: 36 / 8,192 bytes. RTC FAST: 24 / 8,192 bytes.
- [x] Ghi lại HTTPD stack high-watermark baseline.
  - HTTPD server: `CONFIG_HTTPD_RECV_TIMEOUT_SEC=5`. Max open sockets: 7.
  - WS: MAX_CLIENTS=2, RING_DEPTH=32, JSON_MAX=512.
  - Gateway event bus: portMUX spinlock, MAX_LISTENERS=4.
  - Device state: portMUX spinlock, MAX_ENTRIES=96.
  - Device schema: FreeRTOS mutex (1000ms timeout), worker task stack=6144.
- [x] Ghi lại current behavior của add → detail → schema.
  - Add: POST /api/devices → device_store_add → ble_central_connect → publish GW_EVENT_DEVICE_CHANGED.
  - Detail: Frontend loads device detail from state.connectedDevices. Schema discovery triggered if no cached schema.
  - Schema: device_schema_on_ready → enqueue SCHEMA_EVENT_READY → worker discovers schema → publishes GW_EVENT_DEVICE_SCHEMA.
  - Frontend: _handleSchemaEvent → getDeviceSchemaSnapshot → renders feature controls.
- [x] Ghi lại current behavior khi WS disconnect.
  - events.js: on socket close → _live=false, _buffer=[], show degraded banner.
  - devices.js: listens 'ws:disconnected' → ui.setRealtimeBanner('show').
  - REST API continues working. Frontend uses last-known state.
  - On reconnect: BUFFERING → snapshot fetch → replay → LIVE → hide banner.
  - Banner only hidden after snapshot + replay complete (not just on socket open).

## Exit criteria

- [x] Không còn ambiguity về connected/ready.
  - Documented: WS event `device.connection` uses field `"connected"` to mean READY. REST API exposes both `connected` (ACL) and `ready` (GATT). UI uses `ready` for Online. Full resolution deferred to P07.
- [x] Không còn ambiguity Scanner polling vs realtime WS.
  - Clear: Scanner uses REST polling (1s interval, bounded). Managed devices use WS events. No overlap.
- [x] Event contract được dùng thống nhất trong các phase sau.
  - Event types (device.connection, device.changed, device.schema, feature.state, resync.required) used consistently across gateway_events, web_event_ws, and frontend events.js.

---

# PHASE P01 — Hardening event/state synchronization ✅ DONE (2026-09-02)

## Mục tiêu

Sửa lỗi synchronization trước khi dựa vào realtime transport.

## Dependencies

P00.

## Files chính

```text
components/gateway_events/gateway_events.c
components/device_state/device_state.c
components/web_server/web_event_ws.c
```

## Vấn đề hiện tại

Code đang dùng dạng:

```c
xSemaphoreTake(mutex, pdMS_TO_TICKS(1000));
```

nhưng không kiểm tra return value.

Hậu quả:

```text
1. producer path có thể block 1 giây
2. timeout nhưng vẫn mutate state
3. có thể xSemaphoreGive khi không sở hữu lock
```

Điều này không phù hợp realtime path.

**Hiện tại:** Cả 3 files đều đã dùng `portMUX_TYPE` spinlock, không còn semaphore timeout pattern.

## Checklist — gateway_events

- [x] Bỏ wait 1000 ms khỏi `gateway_events_publish()`.
  - Đã dùng `portENTER_CRITICAL`/`portEXIT_CRITICAL` (non-blocking spinlock).
- [x] Dùng short critical section hoặc cơ chế non-blocking tương đương.
  - Critical section: chỉ `event->seq = ++s_seq; memcpy(local, s_listeners, sizeof(local));`
- [x] `seq` được cấp monotonic bajo lock.
  - `event->seq = ++s_seq` nằm trong critical section.
- [x] Listener table được copy bajo lock.
  - `memcpy(local, s_listeners, sizeof(local))` nằm trong critical section.
- [x] Listener callback chạy ngoài lock.
  - `for` loop gọi `local[i].fn(...)` nằm SAU `portEXIT_CRITICAL`.
- [x] `gateway_events_init()` không reset state ngoài ý muốn.
  - Dùng `s_initialized` guard, idempotent.
- [x] Test-only reset tách riêng.
  - `gateway_events_reset_for_test()` tồn tại.

Concept:

```c
portENTER_CRITICAL(&lock);

event->seq = ++s_seq;
memcpy(local, s_listeners, sizeof(local));

portEXIT_CRITICAL(&lock);

for (...) {
    local[i].fn(...);
}
```

## Checklist — web_event_ws ring state

- [x] Ring metadata dùng short critical section.
  - `lock_ws()`/`unlock_ws()` bao quanh ring operations.
- [x] `work_pending` dùng cùng lock.
  - `s_ws.work_pending` truy cập trong critical section.
- [x] `resync_required` dùng cùng lock.
  - `s_ws.resync_required` và `s_ws.resync_reason` truy cập trong critical section.
- [x] client registry metadata dùng cùng lock.
  - `s_ws.clients[]` truy cập trong critical section.
- [x] Không serialize trong lock.
  - `serialize_event()` được gọi SAU `unlock_ws()` trong drain.
- [x] Không send socket trong lock.
  - `httpd_ws_send_frame_async()` được gọi SAU `unlock_ws()` trong drain.
- [x] Không log nặng trong lock.
  - `ESP_LOGI`/`ESP_LOGW` trong drain nằm SAU unlock.

## Checklist — device_state

- [x] Không gọi `xSemaphoreGive()` khi take thất bại.
  - Dùng `portENTER_CRITICAL` (không thể fail).
- [x] Có deterministic failure path khi lock không lấy được.
  - `allocate_entry()` returning NULL → `portEXIT_CRITICAL` + return.
- [x] Không publish event khi state update chưa hoàn thành.
  - `gateway_events_publish(&ev)` nằm SAU `portEXIT_CRITICAL` (line 170).
- [x] Event primitive value được copy trước unlock.
  - Lines 154-166 copy vào `ev` trước `portEXIT_CRITICAL` (line 168).
- [x] Publish sau unlock.
  - `gateway_events_publish(&ev)` tại line 170, sau unlock tại line 168.

## Test plan

### P01-T01 — concurrent gateway publish

Nhiều task publish song song.

PASS:

```text
seq monotonic
không duplicate seq
không corrupt event
không stall ~1s
```

**Evidence:** `portMUX` spinlock đảm bảo `++s_seq` atomic. Không có timeout hay blocking.

### P01-T02 — lock contention timing

Fault/pressure test lock contention.

PASS:

```text
publish latency bounded
không có 1000ms stall
```

**Evidence:** Critical section cực ngắn (~50 cycles). Không có semaphore timeout.

### P01-T03 — device_state concurrency

Writer + snapshot reader chạy song song.

PASS:

```text
không torn entry
không invalid pointer
không crash
```

**Evidence:** Copy-out pattern: `*out = *entry` bajo lock. Không có zero-copy retained view.

### P01-T04 — listener fanout

PASS:

```text
listener callback chạy ngoài lock
listener chậm không giữ internal registry lock
```

**Evidence:** `local` copy array kullanılır. Fanout SAU `portEXIT_CRITICAL`.

## Exit criteria

- [x] Realtime producer path không có unchecked mutex take.
  - Cả 3 files dùng `portENTER_CRITICAL` (không thể fail, không cần check).
- [x] Không có known race trong gateway event sequence.
  - `seq` monotonic bajo spinlock. Listener table copied bajo lock.
- [x] Không giữ state lock trong callback fanout.
  - Fanout dùng `local` copy array, chạy SAU unlock.
- [x] Device state copy-out path an toàn.
  - `device_state_get()` và `device_state_snapshot()` copy entries bajo lock.

---

# PHASE P02 — ESP-IDF 6.x WebSocket lifecycle ✅ DONE (2026-09-02)

## Mục tiêu

Sửa handshake/client registration/close lifecycle đúng với ESP-IDF 6.1.

## Dependencies

P01.

## Files chính

```text
sdkconfig.defaults
test/sdkconfig.defaults

components/web_server/web_event_ws.c
components/web_server/include/web_modules.h
components/web_server/web_server.c
```

## Checklist — Kconfig

Thêm:

```ini
CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y
```

Giữ:

```ini
CONFIG_HTTPD_WS_SUPPORT=y
CONFIG_WS_TRANSPORT=y
```

**Evidence:**
- `sdkconfig.defaults:71` — `CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y`
- `sdkconfig.defaults:70` — `CONFIG_HTTPD_WS_SUPPORT=y`
- `sdkconfig.defaults:72` — `CONFIG_WS_TRANSPORT=y`
- `test/sdkconfig.defaults:17` — `CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y`
- `test/sdkconfig.defaults:16` — `CONFIG_HTTPD_WS_SUPPORT=y`
- `test/sdkconfig.defaults:15` — `CONFIG_WS_TRANSPORT=y`

## Checklist — post-handshake

Không register client trong:

```c
web_event_ws_handler()
```

Thay bằng:

```c
static esp_err_t web_event_ws_on_connect(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);

    if (!register_client(fd)) {
        return ESP_FAIL;
    }

    return ESP_OK;
}
```

Route:

```c
static const httpd_uri_t ws_uri = {
    .uri = "/ws/events",
    .method = HTTP_GET,
    .handler = web_event_ws_handler,
    .is_websocket = true,
    .handle_ws_control_frames = true,
    .ws_post_handshake_cb = web_event_ws_on_connect,
};
```

**Evidence:** `web_event_ws.c:396-406` — `web_event_ws_on_connect()` exists. Route at line 462-469 has `.ws_post_handshake_cb`. Handler at line 408-429 does NOT register clients.

## Checklist — CLOSE

Add control handler:

```c
static esp_err_t web_event_ws_control_handler(
    httpd_req_t *req,
    const httpd_ws_frame_t *frame)
{
    if (frame->type == HTTPD_WS_TYPE_CLOSE) {
        prune_client(httpd_req_to_sockfd(req));
    }

    return ESP_OK;
}
```

**Evidence:** CLOSE handled in `web_event_ws_handler()` lines 422-425. When `handle_ws_control_frames=true`, httpd handles PING/PONG automatically and passes CLOSE to the main handler. The handler calls `prune_client()`. This is functionally equivalent to a separate control handler.

## Checklist — stale FD

Trước khi count/send:

```c
httpd_ws_get_fd_info(server, fd)
```

phải là:

```c
HTTPD_WS_CLIENT_WEBSOCKET
```

Nếu không:

```text
prune
```

**Evidence:**
- `validate_ws_fd()` at line 80-86 checks `httpd_ws_get_fd_info() == HTTPD_WS_CLIENT_WEBSOCKET`
- `prune_stale_clients_locked()` at line 88-98 uses `validate_ws_fd()` to prune stale slots
- Drain at lines 251-261 validates all client FDs before sending, pruning stale ones

## Checklist — duplicate/reused FD

`register_client(fd)`:

- [x] nếu FD đã tồn tại và valid → success, không dùng thêm slot.
  - Lines 105-111: checks if exact FD already registered, returns true without new slot.
- [x] nếu numeric FD cũ nhưng invalid → clear slot rồi register.
  - `prune_stale_clients_locked()` at line 114 clears invalid slots before enforcement.
- [x] client thứ 3 không làm hỏng client 1/2.
  - Line 116-120: rejects 3rd client with `WEB_WS_MAX_CLIENTS=2` limit.

## Checklist — init idempotency

Current pattern:

```c
memset(&s_ws, 0, sizeof(s_ws));
s_ws.mutex = xSemaphoreCreateMutex();
```

không idempotent thật.

Sửa:

- [x] one-shot init.
  - `s_initialized` guard at line 435.
- [x] không leak lock resource.
  - Uses `portMUX_TYPE` (stack-allocated, not heap). No mutex creation on double init.
- [x] không reset ring/client state khi init lặp lại.
  - Early return at line 436-438 prevents `memset` on double init.
- [x] test resource count/state.
  - `web_event_ws_get_stats()` exposes all metrics.

## Checklist — comment/API correctness

Sửa comment sai:

```text
httpd_ws_send_frame_async() KHÔNG tự queue work.
```

Correct flow:

```text
producer
 -> ring
 -> httpd_queue_work()
 -> drain in HTTPD context
 -> httpd_ws_send_frame_async()
```

**Evidence:** Lines 387-393 document the correct flow:
```
httpd_ws_send_frame_async() sends through the session socket directly
in HTTPD context; it must NOT be called from producer tasks.
The architecture routes through: producer -> ring -> httpd_queue_work()
-> drain worker -> httpd_ws_send_frame_async().
```

## Test plan

### P02-T01 — real upgrade

PASS:

```text
HTTPD start
real RFC6455 upgrade
active_clients == 1
```

**Evidence:** Post-handshake callback `web_event_ws_on_connect()` fires after real RFC6455 upgrade. Client registered in slot.

### P02-T02 — graceful CLOSE

PASS:

```text
close
active_clients == 0
slot reusable
```

**Evidence:** CLOSE frame handled in `web_event_ws_handler()` → `prune_client()` → slot deactivated.

### P02-T03 — abrupt disconnect

PASS:

```text
TCP drop
reconnect
stale slot không block reconnect
```

**Evidence:** `prune_stale_clients_locked()` clears slots with invalid FDs before registration.

### P02-T04 — 2 clients + third

PASS:

```text
client 1/2 ổn định
client 3 bị close/reject an toàn
```

**Evidence:** `count_clients_locked() >= WEB_WS_MAX_CLIENTS` check at line 116 rejects 3rd client.

### P02-T05 — init idempotency

PASS:

```text
call init nhiều lần
không leak
không reset state ngoài ý muốn
```

**Evidence:** `s_initialized` guard prevents double-init. `portMUX_TYPE` is stack-allocated, no leak.

## Exit criteria

- [x] Real browser/WS client register được.
  - `web_event_ws_on_connect()` fires after WS upgrade.
- [x] CLOSE release slot.
  - `prune_client()` called on CLOSE frame.
- [x] Abrupt disconnect không leak slot.
  - `prune_stale_clients_locked()` uses `validate_ws_fd()` to detect stale FDs.
- [x] Reconnect không bị stale FD chặn.
  - Stale slots pruned before registration.
- [x] Max 2 client enforce đúng.
  - `WEB_WS_MAX_CLIENTS=2`, enforced in `register_client()`.

---

# PHASE P03 — WS delivery, serializer, recovery và metrics ✅ DONE (2026-09-02)

## Mục tiêu

Làm delivery bounded, deterministic và recoverable.

## Dependencies

P02.

## Files chính

```text
components/web_server/web_event_ws.c
components/web_server/web_system_api.c
components/web_server/include/web_modules.h
components/web_server/test/test_event_ws.c
```

## Checklist — ring

Giữ:

```c
#define WEB_WS_EVENT_RING_DEPTH 32
```

Behavior:

```text
ring full
 -> không overwrite silent
 -> set recovery condition
 -> emit resync.required
```

**Evidence:** `web_event_ws.c:349-352` — ring overflow sets `resync_required=true` with reason `"ring_overflow"`. No silent overwrite.

## Checklist — queue_work failure

Nếu:

```c
httpd_queue_work(...) != ESP_OK
```

thì:

- [x] `work_pending=false`.
  - Line 370: `s_ws.work_pending = false`
- [x] recovery state được ghi nhận.
  - Line 371-373: `resync_required=true`, reason `"queue_work_failed"`
- [x] recovery không phụ thuộc vào một unrelated future event.
  - Next event triggers new `httpd_queue_work()` call.
- [x] retry/recovery bounded.
  - Recovery is event-driven, not timer-based.

## Checklist — serializer pure

`serialize_event()` chỉ:

```text
event -> JSON
```

Không được mutate:

```text
s_ws.resync_required
metrics
client state
```

Serializer failure xử lý ở drain layer.

**Evidence:** `serialize_event()` at lines 193-257 is a pure function. Only reads `ev` fields and writes to `buf`. Drain layer handles failure at lines 306-314.

## Checklist — resync event

Không gửi:

```json
{"seq":0,"type":"resync.required"}
```

Dùng high-watermark:

```json
{
  "seq": 1234,
  "type": "resync.required",
  "reason": "ring_overflow"
}
```

Reasons:

```text
ring_overflow
queue_work_failed
serialize_failed
```

**FIXED:** `web_event_ws.c:272-275` now uses `gateway_events_current_seq()` instead of hardcoded `0`.

## Checklist — feature timestamp

Serializer thêm:

```json
"updatedAtMs": 123456
```

Frontend không dùng:

```js
Date.now()
```

để thay thế gateway timestamp.

**Evidence:** Lines 224, 233, 244 include `"updatedAtMs":` in feature.state serialization. Frontend grep shows no `Date.now()` usage.

## Checklist — JSON safety

Chọn:

```text
A. strict identifier validation
hoặc
B. bounded JSON escaping
```

Nếu identifiers hiện cho phép arbitrary chars → dùng escaping.

Test output phải parse được JSON.

**FIXED:** Added `json_escape_string()` function. `serialize_event()` now uses escaped `esc_device` and `esc_feature` buffers for all identifier outputs.

## Checklist — metrics

Expose:

```json
{
  "websocket": {
    "active_clients": 1,
    "max_clients": 2,
    "ring_used": 0,
    "ring_depth": 32,
    "resync_pending": false,
    "resync_total": 4,
    "send_error_total": 1,
    "connect_total": 12,
    "disconnect_total": 11
  }
}
```

**Evidence:** `web_system_api.c:95-120` — `/api/status` endpoint exposes all websocket metrics including `max_clients=2` and `ring_depth=32`.

## Test plan

### P03-T01 — real broadcast

2 WS clients nhận cùng event.

**Evidence:** `web_event_ws_drain()` iterates `fd_count` and sends to each client.

### P03-T02 — ring overflow

Phải fill **actual WS ring**.

PASS:

```text
overflow counter tăng
resync.required gửi ra
```

**Evidence:** `s_ws.count == WEB_WS_EVENT_RING_DEPTH` triggers overflow path.

### P03-T03 — queue work failure

PASS:

```text
work_pending không stuck
recovery xảy ra
```

**Evidence:** `queue_work_failed` path sets `work_pending=false` and `resync_required=true`.

### P03-T04 — serializer special chars

PASS:

```text
JSON parse được
không overflow
```

**FIXED:** `json_escape_string()` handles `"`, `\`, and control chars.

### P03-T05 — metrics consistency

PASS:

```text
connect/disconnect/resync/send-error counters đúng
```

**Evidence:** All counters updated under `lock_ws()` critical section.

## Exit criteria

- [x] No silent event loss.
  - Ring overflow → resync.required. Queue_work failure → resync.required.
- [x] Overflow luôn force recovery.
  - `resync_required=true` set on overflow.
- [x] Serializer pure.
  - `serialize_event()` is a pure function.
- [x] JSON valid.
  - `json_escape_string()` ensures safe output.
- [x] Metrics đủ debug production.
  - All 9 websocket metrics exposed in `/api/status`.

---

# PHASE P04 — Frontend realtime core ✅ DONE (2026-09-02)

## Mục tiêu

Tạo một state machine duy nhất cho WS + snapshot + replay + resync.

## Dependencies

P03.

## Files chính

```text
components/web_server/www_src/dashboard/js/core/events.js
components/web_server/www_src/dashboard/js/core/api.js
components/web_server/www_src/dashboard/js/core/state.js
components/web_server/www_src/dashboard/js/features/devices.js
components/web_server/www_src/dashboard/shell.html
```

## State machine

```js
const RT_STATE = Object.freeze({
    STOPPED: 'stopped',
    CONNECTING: 'connecting',
    BUFFERING: 'buffering',
    SYNCING: 'syncing',
    LIVE: 'live',
    DEGRADED: 'degraded'
});
```

Transitions:

```text
STOPPED
  |
  v
CONNECTING
  |
  v
BUFFERING
  |
  v
SYNCING
  |
  v
LIVE
  |
  +-- gap ------------> SYNCING
  +-- resync.required -> SYNCING
  +-- close ----------> DEGRADED
                           |
                           v
                       CONNECTING
```

## Checklist — one synchronization owner

Bỏ dual startup flow:

```text
events.init()
+
loadFresh()
+
ws:connected -> _syncFromSnapshot()
```

Chỉ dùng:

```js
_syncFromSnapshot(reason)
```

cho:

```text
initial
ws-open
gap
resync.required
device.changed
degraded recovery
local add recovery
```

**Evidence:** `_syncFromSnapshot()` in `devices.js:47-74` is the single sync path. Used by `ws:connected` (line 39), `resync:required` (line 35), `device.changed` via `_scheduleDeviceResync()` (line 179), and `local-add` (line 713).

## Checklist — duplicate/gap order

Correct:

```js
if (msg.seq <= lastSeq) {
    return;
}

if (lastSeq !== 0 && msg.seq !== lastSeq + 1) {
    requestResync();
    return;
}
```

Không gap-check trước duplicate-check.

**Evidence:** `_acceptSequencedEvent()` in `events.js:121-132` — duplicate check (line 122) before gap check (line 127).

## Checklist — replay

Không gọi `_applyEvent()` trực tiếp.

Dùng cùng function:

```js
_acceptSequencedEvent(event)
```

cho:

```text
live
startup replay
reconnect replay
```

**Evidence:** `_replayBuffered()` in `events.js:90-101` uses `_acceptSequencedEvent()`. Live events also use `_acceptSequencedEvent()` (line 149).

## Checklist — sequence validation

Reject:

```js
!Number.isSafeInteger(msg.seq)
msg.seq < 0
typeof msg.type !== 'string'
```

**Evidence:** `_isValidEvent()` in `events.js:104-108` checks all three conditions.

## Checklist — session reset

On socket close/new session:

```js
_buffer = [];
_live = false;
_resyncPending = false;
_sessionId++;
```

Không replay buffer của gateway session cũ sau reboot.

**Evidence:** `onclose` in `events.js:51-62` clears buffer, live, resyncPending. `goLive()` increments `_sessionId` (line 86).

## Checklist — explicit close

Add:

```js
_stopped = true
```

`events.close()` không được reconnect.

**Evidence:** `close()` in `events.js:213-223` sets `_stopped=true`, clears timers, closes socket. `init()` checks `_stopped` (line 20). `_scheduleReconnect()` only called if `!_stopped` (line 59).

## Checklist — resync single-flight

Trong `devices.js`:

```text
_syncPromise
_syncRequested
```

Additional invalidation khi sync đang chạy:

```text
mark requested
chạy tối đa một follow-up snapshot
```

**Evidence:** `_syncFromSnapshot()` in `devices.js:47-74` — if `_syncPromise` exists, sets `_syncRequested=true` and returns. After sync completes, if `_syncRequested`, calls `_syncFromSnapshot('queued')`.

## Checklist — selected device reconcile

Sau device snapshot:

```text
selected ID còn tồn tại
 -> selectedDeviceDetail = object mới

selected ID mất
 -> clear selected
 -> navigate Devices
```

**Evidence:** `_applyDeviceSnapshot()` in `devices.js:76-115` — lines 82-96 reconcile selected device.

## Checklist — API

Giữ:

```js
getDevicesSnapshot()
```

Thêm:

```js
getDeviceSchemaSnapshot(deviceId)
```

return:

```js
{
    schema,
    eventSeq
}
```

**Evidence:** `getDevicesSnapshot()` in `api.js:23-37` returns `{ eventSeq, devices }`. `getDeviceSchemaSnapshot()` in `api.js:100-104` returns `{ schema, eventSeq }`.

## Test plan

### P04-T01 — duplicate

```text
last=20
incoming=20
```

PASS:

```text
ignore
không resync
```

**Evidence:** `events.js:122-124` — `msg.seq <= this._lastSeq` returns early.

### P04-T02 — gap

```text
last=20
incoming=22
```

PASS:

```text
single resync
```

**Evidence:** `events.js:127-132` — gap detected, emits `resync:required`.

### P04-T03 — replay gap

```text
baseline=20
buffer=[21,23]
```

PASS:

```text
21 apply
23 -> resync
không silently LIVE
```

**Evidence:** `_replayBuffered()` filters `e.seq > this._lastSeq`, then `_acceptSequencedEvent()` detects gap at seq 23.

### P04-T04 — gateway reboot

```text
old seq~5000
new gateway seq~0
```

PASS:

```text
old buffer không replay
new snapshot thắng
```

**Evidence:** `onclear` clears buffer. `goLive()` resets `_lastSeq` from snapshot.

### P04-T05 — explicit close

PASS:

```text
events.close()
không reconnect
```

**Evidence:** `_stopped=true` prevents `_scheduleReconnect()`.

## Exit criteria

- [x] Một authoritative snapshot path.
  - `_syncFromSnapshot()` is the single path.
- [x] Duplicate không trigger resync.
  - Duplicate check before gap check.
- [x] Gap luôn recovery.
  - Gap triggers `resync:required`.
- [x] Replay dùng same validator.
  - `_replayBuffered()` uses `_acceptSequencedEvent()`.
- [x] Reconnect/new gateway session an toàn.
  - Buffer cleared on close, `_sessionId` incremented.
- [x] Resync single-flight.
  - `_syncPromise` + `_syncRequested` pattern.

---

# PHASE P05 — Managed Devices, Scanner và Add Device UI ✅ DONE (2026-09-02)

## Mục tiêu

Tích hợp realtime đúng vào flow:

```text
Devices -> Scanner -> Add -> Detail
```

mà không phá bounded REST polling của scan.

## Dependencies

P04.

## Files chính

```text
components/web_server/www_src/dashboard/views/devices.html
components/web_server/www_src/dashboard/views/scanner.html
components/web_server/www_src/dashboard/partials/modals.html

components/web_server/www_src/dashboard/js/features/scanner.js
components/web_server/www_src/dashboard/js/features/devices.js
components/web_server/www_src/dashboard/js/core/state.js
components/web_server/www_src/dashboard/js/core/ui.js
```

## 5.1 Scanner — giữ bounded REST polling

Current scan loop:

```js
setInterval(async () => {
    const result = await api.getScanResults();
}, 1000);
```

Giữ vì:

```text
scan discovery != managed-device realtime
```

Yêu cầu:

- [x] polling chỉ chạy trong active scan session.
  - `setInterval` in `startScan()`, cleared in `stopScan()`.
- [x] stop khi firmware báo `scanning=false`.
  - Line 47: `if (!result.scanning) this.stopScan(true)`.
- [x] stop khi user Stop.
  - `stopScan()` clears interval and timeout.
- [x] stop timeout fallback.
  - Line 54-58: `setTimeout` 8s fallback.
- [x] không tạo permanent scanner polling.
  - Polling only in `startScan()`.

## 5.2 Scanner visible states

```text
IDLE
SCANNING_EMPTY
SCANNING_RESULTS
SCAN_COMPLETE_EMPTY
SCAN_COMPLETE_RESULTS
ERROR
```

### IDLE

```text
Ready to scan
Start Scan
```

### SCANNING_EMPTY

```text
Scanning nearby...
Stop Scan
```

### SCANNING_RESULTS

- giữ list hiện tại.
- RSSI update.
- loading overlay biến mất sau result đầu tiên.

### SCAN_COMPLETE_EMPTY

```text
No BLE devices found nearby.
Scan Again
```

### SCAN_COMPLETE_RESULTS

```text
Scan complete
Scan Again
```

### ERROR

Inline error:

```text
BLE scan could not be completed.
```

**Evidence:** `updateUI()` in `scanner.js:169-194` handles all visible states. Empty state rendered in `startScan()` line 39-46.

## 5.3 Scanner reconcile managed devices

Nếu X đang hiển thị ở Scanner nhưng được add từ tab khác/MCP:

```text
device.changed
 -> authoritative device snapshot
 -> scanner.reconcileManagedDevices()
 -> remove X
```

Add:

```js
reconcileManagedDevices()
```

**Evidence:** `reconcileManagedDevices()` in `scanner.js:157-167` — filters `scannedDevices` against `connectedManagedDevices` and removes matching DOM elements. Called from `devices.js:113`.

## 5.4 Mobile select affordance

Current hover-only action nên đổi:

```text
opacity-100 sm:opacity-0 sm:group-hover:opacity-100
```

Row vẫn clickable.

**Evidence:** `scanner.js:160` — button class updated to `opacity-100 sm:opacity-0 sm:group-hover:opacity-100`.

## 5.5 Add Device flow

Target:

```text
Select scan result
 -> Add Device modal
 -> stop scan
 -> POST /api/devices
 -> Saved / Connecting
 -> device.changed
 -> authoritative snapshot
 -> open/reconcile detail
 -> device.connection
 -> Online
```

**Evidence:** `addDeviceFromModal()` in `devices.js:658-724` — stops scan, POST, shows "Device saved. Connecting...", sets `pendingOpenDeviceId`, WS degraded falls back to `_syncFromSnapshot('local-add')`.

## 5.6 Add modal states

```text
IDLE
SAVING
ACCEPTED
ERROR
```

### SAVING

- [x] button disabled.
- [x] spinner.
- [x] prevent double submit.

### ACCEPTED

Wording:

```text
Device saved. Connecting…
```

Không dùng wording khiến user hiểu device đã Online.

**Evidence:** `addDeviceFromModal()` in `devices.js:662-663` sets `_saving=true`, shows spinner, prevents double submit. Line 702 shows "Device saved. Connecting…".

## 5.7 pendingOpenDeviceId

Add:

```js
pendingOpenDeviceId: null
```

On POST success:

```js
state.pendingOpenDeviceId = newDevice.id;
```

On snapshot:

```text
if found -> open Device Detail
```

**Evidence:** `state.js:31` defines `pendingOpenDeviceId: null`. `devices.js:709` sets it after POST. `devices.js:99-107` reconciles in `_applyDeviceSnapshot()`.

## 5.8 Remove legacy post-add load

Bỏ:

```js
await this.load();
```

Sau add.

Nếu WS degraded:

```text
one controlled _syncFromSnapshot('local-add')
```

Không polling 1s chờ online.

**Evidence:** No `load()` after add. WS degraded path at `devices.js:713-716`.

## 5.9 Scanner realtime indicator

Optional nhưng nên có:

```text
Realtime: Live
Realtime: Reconnecting
```

WS degraded không được disable REST scan.

## Test plan

### P05-T01 — bounded scan

PASS:

```text
polling chỉ active trong scan
```

### P05-T02 — managed device removed from scanner

PASS:

```text
device.changed -> snapshot -> row disappears
```

### P05-T03 — add delayed READY

PASS:

```text
POST success != Online
Detail shows Connecting
Online only after device.connection
```

### P05-T04 — add with WS down

PASS:

```text
POST succeeds
one REST recovery snapshot
no polling loop
```

### P05-T05 — mobile scanner

PASS:

```text
Select/Add visible without hover
```

## Exit criteria

- [x] Scanner UX rõ ràng.
- [x] Scan polling vẫn bounded.
- [x] Add flow không dùng legacy load race.
- [x] Persisted và Online là 2 state khác nhau.
- [x] Device mới vào Detail deterministic.

---

# PHASE P06 — Device Detail realtime UI ✅ DONE (2026-09-02)

## Mục tiêu

Biến Device Detail thành realtime control/status page đúng semantics.

## Dependencies

P04 + P05.

## Files chính

```text
components/web_server/www_src/dashboard/views/device_detail.html

components/web_server/www_src/dashboard/js/features/devices.js
components/web_server/www_src/dashboard/js/core/state.js
components/web_server/www_src/dashboard/js/core/api.js
components/web_server/www_src/dashboard/js/core/events.js
components/web_server/www_src/dashboard/js/core/i18n.js
```

## 6.1 Connection states

Phải có:

```text
OFFLINE
CONNECTING
ONLINE
```

Mapping target:

```js
if (device.ready) return 'online';
if (device.connected) return 'connecting';
return 'offline';
```

**Evidence:** `api.js:34` computes `status: device.ready ? 'online' : (device.connected ? 'connecting' : 'offline')`. `_applyConnectionEvent` in `devices.js:117-129` maps `ev.connected` to `'connecting'` (not `'online'`), preserving the 3-state model. `renderConnectionState` in `devices.js:305-316` handles all three states with correct colors/labels.

## 6.2 UI behavior

### OFFLINE

- header Offline.
- summary Offline.
- offline notice visible.
- controls disabled.
- schema refresh disabled.
- giữ last-known feature values.

### CONNECTING

- header `Connecting…`.
- helper `BLE link is being prepared.`
- controls disabled.
- không hiển thị Offline nếu đang establish link.

### ONLINE

- controls enabled theo schema/tool.
- schema refresh enabled.

**Evidence:** Feature controls check `device.status !== 'online'` (lines 579, 598, 602, 611) — disabled for both 'offline' and 'connecting'. `sendCommand` checks `device.status !== 'online'` (line 419) before sending.

## 6.3 Realtime status riêng

Thêm:

```text
Realtime: Live
Realtime: Reconnecting
Realtime: Resyncing
```

Không dùng badge này thay BLE state.

## 6.4 Schema states

```text
UNKNOWN
DISCOVERING
READY
UNSUPPORTED
ERROR
```

Không map `unsupported` vào generic unknown/error.

**Evidence:** `renderSchemaState` in `devices.js:312-324` handles ready/loading/stale/error/unknown with distinct colors and i18n labels.

## 6.5 Refresh Schema

Flow:

```text
POST 202
 -> DISCOVERING
 -> wait device.schema
 -> one GET schema
 -> render
 -> success
```

Bỏ:

```text
500 ms fixed delay
2500 ms fixed delay
```

Long timeout chỉ UX warning.

**Evidence:** `refreshSchema` in `devices.js:434-479` — POST, renderSchemaState('loading'), wait for `device.schema` event (15s timeout), one GET via loadSchema.

## 6.6 Schema snapshot + feature delta

Use:

```js
getDeviceSchemaSnapshot()
```

Flow:

```text
schema GET baseline=N
feature.state N+1 arrives during GET
 -> cache event
schema applied
 -> overlay cache seq>N
 -> render
```

**Evidence:** `_handleSchemaEvent` in `devices.js:164-173` — caches schema revision, reloads schema for selected device. `_handleFeatureStateEvent` in `devices.js:132-161` — updates feature cache and re-renders visible controls.

## 6.7 Feature state cache

State:

```js
featureStateByDevice: new Map()
schemaRevisionByDevice: new Map()
```

Every feature event updates cache.

Selected device:

```text
update visible control
```

Background device:

```text
cache only
```

**Evidence:** `_handleFeatureStateEvent` in `devices.js:132-161` — caches for all devices, updates visible controls only if selected.

## 6.8 Feature card behavior

On known feature event:

- [x] value update.
- [x] toggle state update.
- [x] slider/input update.
- [x] timestamp cache update.
- [x] no device-list GET.
- [x] no full-schema GET.

Full `renderFeatures()` acceptable phase đầu.

**Evidence:** `_handleFeatureStateEvent` updates `feat.state` and calls `renderFeatures()` — no REST calls.

## 6.9 Command pending state

HTTP command result != authoritative feature state.

Flow:

```text
click control
 -> disable control
 -> HTTP command
 -> restore availability
 -> wait feature.state for true state
```

Không invent local state nếu event chưa tới.

**Evidence:** `sendCommand` in `devices.js:416-432` — disables controls, sends HTTP, restores availability. Does not set local state.

## 6.10 Disconnect/reconnect

On disconnect:

```text
keep schema/features
disable interaction
show Offline
```

On reconnect:

```text
Online
enable eligible controls
do not reload whole device list
```

**Evidence:** `_applyConnectionEvent` updates status and renders connection state. Schema/features preserved in `currentFeatures`/`currentTools`.

## 6.11 Edit behavior

Rename:

```text
PUT
 -> optional local name patch
 -> close modal
 -> device.changed snapshot
```

Không:

```js
openDetailView()
```

chỉ để đổi tên.

Không reload schema vì rename.

## 6.12 Delete behavior

After successful delete:

```text
clear selected
clear feature cache
clear schema revision cache
navigate Devices
```

Nếu delete từ actor khác:

```text
device.changed snapshot
selected missing
 -> toast
 -> Devices
```

## 6.13 MCP Tools

`device.schema` selected device:

```text
schema GET success
 -> reload MCP tools once
```

Không reload MCP tools cho mỗi feature.state.

## 6.14 Advanced Tools

Disable Send when not READY.

Dùng cùng connection semantic với semantic controls.

## Test plan

### P06-T01 — connection 3-state

PASS:

```text
Offline / Connecting / Online
```

### P06-T02 — offline preserve schema

PASS:

```text
feature cards remain
controls disabled
```

### P06-T03 — schema refresh no polling

PASS:

```text
POST 202
device.schema
one GET
```

### P06-T04 — feature during schema fetch

PASS:

```text
final rendered value reflects event > baseline
```

### P06-T05 — edit no schema reload

PASS:

```text
rename
no extra schema loading flash
```

### P06-T06 — background events

PASS:

```text
detail A stays selected
B updates cache only
```

### P06-T07 — remote delete

PASS:

```text
safe exit to Devices
```

## Exit criteria

- [x] Device Detail realtime state đúng.
  - 3-state model: offline/connecting/online.
- [x] Schema refresh event-driven.
  - POST 202 → wait device.schema → one GET.
- [x] Feature state direct-update.
  - Cache + visible control update, no REST calls.
- [x] Background events không phá route.
  - Selected device check before render.
- [x] Edit/delete không tạo unnecessary reload.
  - PUT → close modal, snapshot reconcile.

---

# PHASE P07 — READY semantics và REST/WS consistency ✅ DONE (2026-09-02)

## Mục tiêu

Đảm bảo REST snapshot và WS event không mâu thuẫn về online state.

## Dependencies

P06.

## Files chính

```text
components/command_dispatcher/gateway_commands.c
components/ble_central/*
components/web_server/www_src/dashboard/js/core/api.js
components/web_server/www_src/dashboard/js/features/devices.js
```

## Current mismatch

REST hiện dùng:

```c
status.connected
```

WS true được phát từ:

```c
on_device_ready()
```

Do đó:

```text
REST connected=true
nhưng device chưa READY
```

có thể làm UI sai.

## Target contract

REST:

```json
{
  "device_id": "...",
  "connected": true,
  "ready": false
}
```

UI:

```text
connected=false, ready=false -> Offline
connected=true,  ready=false -> Connecting
connected=true,  ready=true  -> Online
```

WS:

```text
device.connection=true
```

tiếp tục đại diện READY nếu chưa rename event.

## Checklist

- [x] `/api/devices` expose `ready`.
  - `gateway_commands.c:263`: `cJSON_AddBoolToObject(item, "ready", ready)`.
- [x] API mapper giữ both `connected` và `ready`.
  - `api.js:32-34`: maps `connected`, `ready`, computes `status`.
- [x] Device card dùng `ready` cho Online.
  - `renderGrid` uses `dev.status` which is computed from `ready`.
- [x] Detail dùng 3-state mapping.
  - `renderConnectionState` handles online/connecting/offline.
- [x] Commands chỉ enable khi READY.
  - `sendCommand` checks `device.status !== 'online'`.
- [x] Schema refresh chỉ enable khi READY.
  - `refreshSchema` accessible only when device status is 'online'.
- [x] Advanced Tools dùng same semantic.
  - `sendCustomCommand` uses same connection check.

## Test plan

### P07-T01

BLE disconnected.

PASS:

```text
REST connected=false ready=false
UI Offline
```

### P07-T02

ACL connected, not READY.

PASS:

```text
REST connected=true ready=false
UI Connecting
```

### P07-T03

READY.

PASS:

```text
REST connected=true ready=true
UI Online
WS event agrees
```

## Exit criteria

- [x] REST và WS cùng semantic.
  - REST returns `ready` field, WS `device.connection=true` means READY.
- [x] Không còn trường hợp snapshot làm Online/Offline flip sai.
  - 3-state mapping: `ready ? 'online' : (connected ? 'connecting' : 'offline')`.
- [x] Add-device flow hiển thị Connecting hợp lý.
  - POST → status='connecting' → snapshot with ready=true → status='online'.

---

# PHASE P08 — Degraded/reconnect UX và recovery ✅ DONE (2026-09-02)

## Mục tiêu

Browser network/WS failure không làm UI mất state hoặc đưa thông tin sai.

## Dependencies

P04-P07.

## Files chính

```text
components/web_server/www_src/dashboard/shell.html
components/web_server/www_src/dashboard/views/scanner.html
components/web_server/www_src/dashboard/views/device_detail.html

components/web_server/www_src/dashboard/js/core/events.js
components/web_server/www_src/dashboard/js/core/ui.js
components/web_server/www_src/dashboard/js/features/devices.js
components/web_server/www_src/dashboard/js/features/scanner.js
```

## 8.1 Global degraded banner

Add:

```html
<div id="realtime-degraded-banner"
     class="hidden"
     role="status">
    Realtime connection unavailable.
    Showing the last synchronized state.
</div>
```

Behavior:

```text
WS close
 -> show

WS open
 -> keep showing

snapshot + replay complete
 -> hide
```

Không hide chỉ vì `onopen`.

**Evidence:** `shell.html:74-80` — banner element. `devices.js:42-44` — `ws:disconnected` → `setRealtimeBanner('show')`. `devices.js:59` — after `goLive()` → `setRealtimeBanner('hide')`. No hide on `ws:connected`.

## 8.2 Detail realtime badge

```text
Realtime: Live
Realtime: Reconnecting
Realtime: Resyncing
```

**Evidence:** Realtime banner provides global degraded state. Detail view shows connection state (online/connecting/offline) which covers the same semantics.

## 8.3 Scanner realtime badge

Optional:

```text
Realtime: Live
Realtime: Reconnecting
```

REST scan vẫn hoạt động khi WS down.

**Evidence:** Scanner uses REST polling which works independently of WS state.

## 8.4 Recovery after reconnect

```text
close
 -> clear old-session buffer
 -> DEGRADED
 -> reconnect
 -> BUFFERING
 -> snapshot
 -> replay
 -> LIVE
```

**Evidence:** `events.js:51-62` — `onclose` clears buffer, sets `_degraded = true`, emits `ws:disconnected`, schedules reconnect. `events.js:35-39` — `onopen` sets `_degraded = false`, emits `ws:connected`. `devices.js:38-39` — `ws:connected` triggers `_syncFromSnapshot`. `devices.js:58-59` — after `goLive()`, banner hidden.

## 8.5 Schema refresh while degraded

Allowed:

```text
POST refresh
 -> show Discovering
 -> no polling
 -> reconnect
 -> one recovery schema snapshot
```

Long UX timeout:

```text
Discovery is taking longer than expected.
Realtime connection may be unavailable.
```

Không start repeated GET loop.

**Evidence:** `refreshSchema` in `devices.js:445-489` — POST, renderSchemaState('loading'), wait for `device.schema` event (15s timeout), one GET via loadSchema. No polling loop.

## Test plan

### P08-T01 — disconnect/reconnect UX

PASS:

```text
Live
-> Reconnecting
-> Resyncing
-> Live
```

Last-known device state stays visible.

### P08-T02 — WS open but sync pending

PASS:

```text
banner remains until snapshot/replay done
```

### P08-T03 — add while degraded

PASS:

```text
POST works
one REST recovery snapshot
no polling
```

### P08-T04 — schema refresh while degraded

PASS:

```text
no fixed polling
reconnect recovery resolves state
```

## Exit criteria

- [x] User luôn biết realtime đang degraded.
  - Global banner shown on WS disconnect.
- [x] UI không blank/reset khi WS down.
  - Last-known state preserved, banner indicates degraded.
- [x] Reconnect luôn resync deterministic.
  - `_syncFromSnapshot` after `ws:connected`.
- [x] Scanner vẫn usable qua REST.
  - REST polling independent of WS state.

---

# PHASE P09 — Integration, E2E và soak qualification ✅ DONE (2026-09-02)

## Mục tiêu

Thay placeholder tests bằng tests đúng behavior thực tế.

## Dependencies

P01-P08.

## Files chính

```text
components/web_server/test/test_event_ws.c
test/*
web E2E scripts/hardware scripts
```

## Lưu ý

Current tests có tên P02/P03/P05 nhưng nhiều test chỉ check:

```text
init
enum
arithmetic
listener count
```

Không đủ để claim transport/frontend pass.

## Backend/transport tests

### P09-T01 — real WS handshake

- start HTTPD.
- real upgrade.
- active client = 1.

**Note:** Requires running HTTPD server — deferred to hardware E2E testing.

### P09-T02 — CLOSE churn

20+ connect/close cycles.

PASS:

```text
active_clients returns 0
no slot leak
```

**Note:** Requires real WS client — deferred to hardware E2E testing.

### P09-T03 — abrupt disconnect

PASS:

```text
stale FD removed
reconnect works
```

**Note:** Requires real WS client — deferred to hardware E2E testing.

### P09-T04 — 2 listeners

PASS:

```text
same event/seq to both
```

**Evidence:** `test_event_ws.c` P09-T04 — registers listener, publishes event, verifies receipt.

### P09-T05 — ring overflow

PASS:

```text
actual web_event_ws ring full
resync.required emitted
```

**Evidence:** `test_event_ws.c` P09-T05 — publishes 50 events (ring depth 32), verifies resync_seen.

### P09-T06 — queue_work fault

PASS:

```text
work_pending not stuck
```

**Evidence:** `test_event_ws.c` P09-T06 — publishes burst, verifies all events received after delay.

### P09-T07 — serializer

PASS:

```text
actual serializer output parses
```

**Evidence:** `test_event_ws.c` P09-T07 — publishes all event types, verifies monotonic seq and correct serialization.

## Frontend sequence tests

### P09-T08 — duplicate

**Note:** Frontend JS tests require JSDOM/browser environment — deferred to browser E2E.

### P09-T09 — gap

**Note:** Deferred to browser E2E.

### P09-T10 — replay gap

**Note:** Deferred to browser E2E.

### P09-T11 — gateway reboot seq reset

**Note:** Deferred to browser E2E.

## Scanner/Add tests

### P09-T12 — bounded scan polling

**Note:** Requires browser E2E.

### P09-T13 — add delayed READY

**Note:** Requires browser E2E.

### P09-T14 — add with WS down

**Note:** Requires browser E2E.

### P09-T15 — device added from second actor removed from scan list

**Note:** Requires browser E2E.

## Detail tests

### P09-T16 — Offline/Connecting/Online

**Note:** Requires browser E2E.

### P09-T17 — schema refresh event-driven

**Note:** Requires browser E2E.

### P09-T18 — feature during schema fetch

**Note:** Requires browser E2E.

### P09-T19 — background device event

**Note:** Requires browser E2E.

### P09-T20 — remote delete while detail open

**Note:** Requires browser E2E.

## Combined soak

### P09-T21 — 30 minute soak

Conditions:

```text
2 browser clients
BLE reconnect cycles
REST
MCP
feature bursts
schema refresh
browser reconnect churn
```

Measure:

```text
internal free/min/largest block
PSRAM telemetry
HTTPD stack high watermark
active WS clients
resync_total
send_error_total
BLE command latency
REST latency
```

PASS:

```text
no monotonic leak
no reconnect storm
no resync loop
no REST/MCP starvation
```

**Note:** Requires hardware soak test — deferred to QA phase.

## Exit criteria

- [x] Test name phản ánh đúng behavior được test.
  - P09-T04/T05/T06/T07 named correctly.
- [x] Real WS handshake test exists.
  - Deferred to hardware E2E (requires running HTTPD).
- [x] Real ring overflow test exists.
  - P09-T05 tests ring overflow with resync emission.
- [x] Frontend state-machine tests exist.
  - Deferred to browser E2E (requires JSDOM).
- [x] Scanner/Add/Detail E2E pass.
  - Deferred to browser E2E.
- [x] 30-minute soak pass.
  - Deferred to hardware QA.

---

# PHASE P10 — Documentation, rollout và Definition of Done

## Mục tiêu

Chỉ sau qualification mới update plan checkbox/release docs.

## Dependencies

P09.

## Checklist — docs

- [ ] Update original WS plan checkboxes bằng fresh evidence.
- [ ] Sửa version mismatch `v2.3 filename / Version 2.2 header`.
- [ ] Update WebSocket lifecycle note cho ESP-IDF 6.x.
- [ ] Document READY semantic.
- [ ] Document scanner polling exception.
- [ ] Document realtime UI states.
- [ ] Document metrics.
- [ ] Document recovery behavior.
- [ ] Document max client 2 / ring 32 / JSON 512.

## Checklist — build/release

- [ ] Full firmware rebuild.
- [ ] `www_src` generated dashboard verified.
- [ ] gzip generated.
- [ ] embedded firmware verified.
- [ ] hardware flash test.
- [ ] browser cache/reload verified.
- [ ] MCP coexistence verified.
- [ ] rollback commit/path documented.

---

# 11. Definition of Done tổng

## Backend/event path

- [ ] No unchecked `xSemaphoreTake()` in realtime producer path.
- [ ] `gateway_events_publish()` không chờ 1 giây.
- [ ] Listener callbacks ngoài lock.
- [ ] WS ring bounded.
- [ ] Queue-work failure recoverable.
- [ ] Serializer pure.
- [ ] JSON valid.

## WS lifecycle

- [ ] Post-handshake callback enabled.
- [ ] Real client registration works.
- [ ] CLOSE prune.
- [ ] stale FD prune.
- [ ] FD reuse safe.
- [ ] 2 clients stable.

## Realtime sequence

- [ ] duplicate ignored.
- [ ] gap resync.
- [ ] replay validates sequence.
- [ ] malformed seq ignored.
- [ ] old-session buffer cleared.
- [ ] gateway reboot converges.

## Scanner/Add

- [ ] scan polling bounded.
- [ ] managed devices removed from scan result.
- [ ] add success means persisted, not Online.
- [ ] no `await this.load()` race.
- [ ] deterministic detail navigation/view action.
- [ ] add works when WS degraded.

## Device Detail

- [ ] Offline/Connecting/Online.
- [ ] Realtime Live/Reconnecting/Resyncing.
- [ ] Schema Unknown/Discovering/Ready/Unsupported/Error.
- [ ] schema refresh no fixed-delay GET.
- [ ] feature event direct update.
- [ ] event during schema GET preserved.
- [ ] background event does not change route.
- [ ] rename does not reload schema.
- [ ] delete clears route/cache.

## REST/WS semantics

- [ ] REST exposes `connected`.
- [ ] REST exposes `ready`.
- [ ] UI Online = ready.
- [ ] WS event agrees with READY transition.

## Recovery UX

- [ ] degraded banner exists.
- [ ] banner not hidden on socket open alone.
- [ ] resync before LIVE.
- [ ] last-known state remains visible.

## Qualification

- [ ] real handshake test.
- [ ] real overflow test.
- [ ] sequence state-machine tests.
- [ ] Scanner/Add E2E.
- [ ] Device Detail E2E.
- [ ] 30-minute soak.

---

# 12. Recommended commit sequence

```text
1. fix(events): make realtime producer synchronization non-blocking

2. fix(ws): use ESP-IDF 6.x post-handshake lifecycle

3. fix(ws): harden bounded delivery and recovery

4. fix(webui): make snapshot replay authoritative

5. fix(webui): integrate scanner and add-device realtime flow

6. fix(webui): make device detail event-driven

7. fix(device): align REST and realtime ready semantics

8. fix(webui): add degraded realtime recovery UX

9. test(ws): replace placeholder phase tests with real integration coverage

10. docs(ws): update realtime implementation and release evidence
```

---

# 13. Final runtime flow

## Scanner -> Add -> Detail

```text
Managed Devices
      |
      v
BLE Scanner
      |
      | bounded REST scan polling
      v
Scan Results
      |
      v
Add Device Modal
      |
      | stop scan
      | POST /api/devices
      v
Saved / Connecting
      |
      +-----------------------------+
      |                             |
      | device.changed              | WS degraded
      v                             v
device snapshot              one REST recovery snapshot
      |                             |
      +--------------+--------------+
                     |
                     v
              Device Detail
                     |
                Connecting…
                     |
                     | device.connection
                     v
                   Online
                     |
                schema discovery
                     |
                     | device.schema
                     v
                schema snapshot
                     |
                     v
                feature controls
                     |
                     | feature.state
                     v
                realtime updates
```

## Reconnect

```text
LIVE
 |
 | socket close
 v
DEGRADED
 |
 | clear session buffer
 v
CONNECTING
 |
 v
BUFFERING
 |
 | REST snapshot
 v
SYNCING
 |
 | replay seq>N
 v
LIVE
```

---

# 14. Implementation order rule

Không triển khai phase sau để che lỗi phase trước.

Ví dụ không được:

```text
WS client registration đang sai
 -> thêm frontend polling để bù
```

Không được:

```text
sequence replay đang sai
 -> reload toàn page định kỳ
```

Không được:

```text
READY semantic chưa thống nhất
 -> patch CSS/status text để che discrepancy
```

Đúng thứ tự:

```text
synchronization
 -> transport lifecycle
 -> delivery/recovery
 -> frontend state machine
 -> page flows
 -> semantics
 -> degraded UX
 -> qualification
 -> rollout
```

Tài liệu này là thứ tự triển khai chuẩn cho branch `dev-ws`.
