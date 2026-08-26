# `components/ble_central`

BLE Central / GATT Client của ESP32 BLE Gateway: scan, connect, secure, discover,
gửi/nhận message CBOR, tự động reconnect và quản lý vòng đời thiết bị.

Public API: `include/ble_central.h`. Thiết kế chi tiết: `refactor-components-ble_central_v3.md`.

## Kiến trúc

```text
Application (dispatcher/web/mcp)
        │  ble_central.h
        ▼
┌─────────────────────────────────────────┐
│ ble_central.c          facade/public API│
│ ble_central_runtime.c  device runtime   │  ← DEVICE_STORE_MAX_DEVICES (16)
│ ble_central_state.c    connection pool  │  ← BLE_CENTRAL_MAX_CONN (9)
│                        + generation/ref │
├─────────────────────────────────────────┤
│ ble_central_gap.c      GAP events       │
│ ble_central_gatt.c     discovery/CCCD   │
│ ble_central_scan.c     scan 0xABF0      │
│ ble_central_supervisor.c reconnect     │
│ ble_central_notify.c   queue + worker   │
└─────────────────────────────────────────┘
        ▼
     NimBLE host
```

Hai lớp trạng thái tách biệt — đây là bất biến quan trọng nhất của component:

- **Device runtime** tồn tại độc lập với connection slot (thiết bị offline không giữ slot).
- **Connection slot** chỉ tồn tại khi có BLE lifetime đang chạy, được đánh dấu bằng
  `generation` tăng dần mỗi lifetime để vô hiệu hóa stale callback.

## Data model

| Pool | Kích thước | Nội dung |
|---|---|---|
| `g_ble_devices` | `DEVICE_STORE_MAX_DEVICES` | `device_id`, `peer_addr`, state, `connection_slot`, retry/backoff |
| `g_ble_connections` | `BLE_CENTRAL_MAX_CONN` | conn_handle, GATT handles, MTU, `device_index`, `generation` |
| `s_callback_ctxs` | `BLE_CENTRAL_MAX_CONN` | callback context truyền cho NimBLE (`{slot_index, generation}`) |

Toàn bộ ba pool được bảo vệ bởi **một** `s_state_mutex`. Callback identity là
`ble_conn_ref_t {slot_index, generation}` — mọi GAP/GATT callback validate ref +
conn_handle trước khi mutate; mismatch → bỏ qua + tăng `stale_callbacks`.

## State machines

Device (`ble_device_state_t`):

```text
OFFLINE ──reserve──► CONNECTING ──success──► CONNECTED(READY)
   ▲                    │ fail/cancel           │ disconnect
   │◄── host reset ─────┘                       ▼
   └──────────────────────────────────── BACKOFF (2→4→8→16→30s)
OFFLINE/CONNECTING/CONNECTED ──forget──► REMOVING ──link chết──► removed
```

Connection (`ble_conn_state_t`):

```text
FREE → CONNECTING → SECURING → DISCOVERING → READY
                │        │            │
                └────────┴────────────┴──► FREE (qua DISCONNECT event)
```

Mapping device ↔ slot được cập nhật nguyên tử trong
`ble_state_reserve_connection()` (validate + allocate + generation++ + mapping
trong một lần giữ lock). Policy hiện tại: **tối đa 1 slot CONNECTING toàn cục**
— request thứ hai nhận `BLE_CENTRAL_ERR_CONNECT_IN_PROGRESS`.

## BLE protocol

```text
Service 0xABF0
├── COMMAND 0xABF1 (Write Without Response)  gateway → device, CBOR
└── STATUS  0xABF2 (Notify)                  device → gateway, CBOR
```

- GATT discovery validate properties: COMMAND phải có WRITE_NO_RSP, STATUS phải
  có NOTIFY — sai property → terminate ngay thay vì fail muộn.
- `send_command()`: snapshot handle + MTU dưới lock, encode sau khi unlock,
  reject `MESSAGE_TOO_LARGE` nếu length > MTU-3 trước khi write.
- Security: bonding + LE Secure Connections, IO cap NO_IO. Timeout tách riêng:
  security 10s, discovery 10s (supervisor terminate khi quá hạn).

## Notification pipeline

NimBLE host task không decode CBOR và không gọi application callback:

```text
NOTIFY_RX → validate ref/handle → copy mbuf → xQueueSend(timeout=0)
        → worker task → CBOR decode → notify_cb(device_id, msg)
```

Queue depth 8 (~2,3 KB). Queue đầy → drop newest + metric `notify_dropped`;
ACK bị drop sẽ do dispatcher timeout xử lý — chấp nhận mất thông báo hơn block
BLE host.

## Reconnect supervisor

FreeRTOS task `ble_reconnect` (stack 4096, prio 4), tick 1000ms:

1. Thu thập + terminate các connection quá timeout security/discovery.
2. Bỏ qua nếu host chưa synced hoặc scan đang active.
3. `ble_scheduler_next_device()`: round-robin qua device đủ điều kiện
   (`in_use`, `reconnect_enabled`, OFFLINE/BACKOFF, không giữ slot, có addr,
   đã tới `next_retry_ms`) → `ble_connection_start()`.

Backoff theo số lần fail liên tục, reset về 0 khi READY. Lifecycle
STOPPED/RUNNING/STOPPING chặn duplicate task khi stop/start nhanh.

## Forget / removal

`ble_central_forget_peer(device_id, addr, type, has_addr)` — caller (command
dispatcher) snapshot peer identity từ `device_store` TRƯỚC khi xóa entry;
BLE layer không lookup lại store sau khi hàm return:

- Không có link: finalize runtime + delete bond ngay.
- Đang CONNECTING: mark `REMOVING` → `ble_gap_conn_cancel()` → callback failure
  hoàn tất (free slot + delete bond + remove runtime).
- Đang connected: mark `REMOVING` → terminate → callback disconnect hoàn tất.

Entry `REMOVING` không thể bị reuse hoặc chọn reconnect trong lúc chờ async.

## Host reset & sync

- `on_ble_host_reset`: bump generation từng slot → reset FREE, clear mapping +
  ctx, device về OFFLINE retry-later, entry REMOVING bị finalize (bond có thể
  orphan — log warning), mirror `connected=0` cho các device mất link.
- Host synced flag dùng EventGroup (`ble_host_is_ready()`); mọi API connect/scan
  đều gate bởi flag này.

## Quy tắc concurrency

```text
State mutex KHÔNG bao quanh:  NimBLE API calls
                              device_store API
                              application callback
NimBLE host KHÔNG:            decode CBOR, gọi app callback, wait ACK
```

## Public API

Signature giữ tương thích với dispatcher; giá trị trả về là `ble_central_err_t`
(0 = OK, <0 = lỗi cụ thể):

```c
ble_central_init(notify_cb)                  // NimBLE + pools + notify worker
ble_central_connect(device_id, addr, type)   // NOT_READY/NOT_FOUND/NO_SLOT/
                                             // CONNECT_IN_PROGRESS/STACK...
ble_central_disconnect(device_id)            // NOT_CONNECTED nếu chưa có link
ble_central_forget_peer(id, addr[6], type, has_addr)
ble_central_send_command(device_id, msg)     // MESSAGE_TOO_LARGE nếu vượt MTU
ble_central_is_connected(device_id)          // true chỉ khi READY
ble_central_active_count()
ble_central_scan_start(cb) / stop() / is_scanning()
ble_central_start_reconnect_supervisor() / stop_reconnect_supervisor()
```

`is_connected()` nghĩa là GATT session READY (đã subscribe STATUS), không chỉ
physical link. Metrics nội bộ (`stale_callbacks`, `notify_dropped`,
`connect_failures`, ...) xem `ble_central_metrics_t` trong internal header.

## Config đáng nhớ

```c
BLE_CONNECT_TIMEOUT_MS         10000   // ble_gap_connect timeout
BLE_SECURITY_TIMEOUT_MS        10000   // riêng cho SECURING
BLE_GATT_DISCOVERY_TIMEOUT_MS  10000   // riêng cho DISCOVERING
BLE_RETRY_INITIAL_MS            2000   // backoff 2→4→8→16→30s (max 30s)
BLE_NOTIFY_QUEUE_DEPTH             8
CONFIG_BT_NIMBLE_MAX_CONNECTIONS   9   // = số connection slot
```

Connection interval thích ứng theo số link: `<3` → 15ms, `<6` → 30ms, còn lại 50ms.

## Tests

Unit tests nằm tại `test/` trong component, đăng ký qua `TEST_COMPONENTS`
trong `test/CMakeLists.txt` của app kiểm thử:

```sh
cd test
idf.py build
idf.py -p <PORT> flash monitor   # tự chạy lúc boot, 23 case [ble_*]
```

Phạm vi: pool/generation/stale-ref, reserve/rollback, scheduler round-robin +
backoff, removal lifecycle, host reset, notify queue. Các kịch bản cần radio
(reconnect thật, forget thiết bị online, notification burst) thuộc integration
test trên hardware với peripheral quảng bá service `0xABF0`.
