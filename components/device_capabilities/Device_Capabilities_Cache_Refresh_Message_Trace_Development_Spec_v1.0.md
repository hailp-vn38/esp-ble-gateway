# Device Capabilities Cache-First & Message Trace Development Specification

**Project:** ESP32 BLE Gateway  
**Repository:** `hailp-vn38/esp-ble-gateway`  
**Target:** ESP32-S3 / ESP-IDF / NimBLE Central  
**Document version:** 1.0  
**Date:** 27/08/2026  
**Baseline repository tree SHA reviewed:** `a26f45d1985f0af7b61848e1ac1ca89fac463f1b`

---

## 1. Purpose

Tài liệu này định nghĩa thay đổi tiếp theo cho `components/device_capabilities` theo hai yêu cầu:

1. **Capability phải là persistent metadata và không được discovery liên tục.**
   - Capability của một device thường cố định hoặc rất ít thay đổi.
   - Gateway chỉ discovery tự động khi **chưa từng có snapshot hợp lệ**.
   - Reconnect BLE, reboot gateway hoặc mất kết nối tạm thời **không được tự động refresh lại** nếu cache vẫn tồn tại.
   - Khi cần cập nhật, người dùng hoặc tầng quản trị gọi **manual refresh**.

2. **Có message trace đầy đủ để debug luồng gửi/nhận.**
   - Theo dõi được request từ Web/MCP tới dispatcher, executor, BLE TX, BLE RX, decode, capability manager/ACK và response cuối.
   - Có correlation bằng `request_id`.
   - Có thể xem cả **decoded fields** và **raw CBOR bytes** trong debug mode.
   - Tận dụng `ESP_LOGx` + `components/log_buffer` hiện có, không tạo một cơ chế log độc lập khó bảo trì.

Tài liệu này là specification triển khai. AI agent hoặc developer có thể dùng trực tiếp làm checklist sửa code và test.

---

## 2. Current implementation baseline

### 2.1. Capability manager hiện tại

Component hiện có:

```text
components/device_capabilities/
├── CMakeLists.txt
├── README.md
├── device_capabilities.c
├── include/device_capabilities.h
└── test/test_device_capabilities.c
```

Hiện tại lifecycle:

```text
BLE READY
   |
   v
device_capabilities_on_ready()
   |
   v
CAP_EVENT_READY
   |
   v
start_discovery(..., automatic=true)
   |
   v
describe_capabilities
```

`attempted_session` chỉ ngăn discovery nhiều lần trong cùng BLE session. Sau disconnect nó được reset, vì vậy reconnect sẽ discovery lại.

Snapshot persist trong NVS namespace:

```text
dev_caps
  cap00
  cap01
  ...
```

Snapshot load từ NVS hiện được gán:

```c
DEVICE_CAP_STATE_STALE
```

Do đó thiết kế cũ ngầm coi cache từ session trước là cần refresh lại.

### 2.2. Capability transport hiện tại

Wire protocol v3 dùng flow:

```text
Gateway                         Device
   |                               |
   | describe_capabilities         |
   |------------------------------>|
   |                               |
   | capabilities_begin            |
   |<------------------------------|
   | capability_item #0            |
   |<------------------------------|
   | capability_item #1            |
   |<------------------------------|
   | ...                           |
   | capabilities_end              |
   |<------------------------------|
   | device_ack                    |
   |<------------------------------|
```

Manager sử dụng staging snapshot và chỉ commit khi nhận đủ begin/items/end hợp lệ. Cách này cần giữ nguyên.

### 2.3. Logging hiện tại

`components/log_buffer` hiện:

- hook `esp_log_set_vprintf()`;
- forward log ra console như bình thường;
- copy mỗi dòng log vào RAM ring;
- `LOG_BUFFER_CAPACITY = 64`;
- `LOG_ENTRY_MAX_LEN = 192`;
- `/api/logs` trả `text` và `timestamp_ms`;
- nếu mutex đang bận, writer không block mà drop log entry;
- có metric `log_buffer_get_dropped_count()`.

Dispatcher đã có một số correlation log:

```text
[CMD_SEND]
[CMD_SEND_FAILED]
[CMD_TIMEOUT]
[CMD_ACK]
[ACK_PROTOCOL_ERROR]
[ACK_UNMATCHED]
```

Nhưng BLE transport chưa có full TX/RX message trace:

- TX encode CBOR rồi write GATT nhưng không dump message/raw bytes;
- RX notify lưu raw bytes vào queue rồi decode nhưng không dump message/raw bytes;
- capability manager chưa log đầy đủ begin/item/end/state transition;
- API/MCP ingress chưa có một trace format thống nhất với BLE layer.

---

## 3. Design decisions

Các quyết định bắt buộc cho phiên bản này:

### D1. Capability là persistent metadata

Snapshot capability hợp lệ phải được xem là có giá trị qua:

- BLE reconnect;
- gateway restart;
- temporary RF loss;
- device sleep/wakeup;
- reconnect supervisor retry.

Không tự động coi snapshot invalid chỉ vì session thay đổi.

### D2. Auto discovery chỉ khi cache chưa tồn tại

`device_capabilities_on_ready(device_id)` có semantics mới:

```text
if committed snapshot exists:
    do nothing
else:
    schedule initial discovery
```

### D3. Manual refresh là explicit operation

Chỉ các nguồn sau được phép ép refresh khi đã có cache:

- Web UI: nút Refresh;
- REST: `POST /api/capabilities/refresh`;
- internal administrative call;
- MCP refresh tool nếu được bổ sung sau này.

`GET /api/capabilities` và `list_device_capabilities` chỉ đọc cache.

### D4. Snapshot cũ không bị mất khi refresh lỗi

Nếu manual refresh lỗi:

```text
old snapshot exists -> keep old snapshot
new staging         -> discard
```

Lỗi refresh không được xóa command list đang dùng.

### D5. Chỉ ghi NVS khi nội dung thực sự thay đổi

Nếu refresh trả cùng revision và cùng capability data:

```text
RAM metadata may update
NVS write = skipped
```

### D6. Message trace phải có một format thống nhất

Không thêm các log ngẫu nhiên ở từng component. Tạo helper/component dùng chung để format:

```text
MSG_TRACE
```

với các direction/phase cố định.

### D7. Raw CBOR chỉ bật khi debug

Summary log có thể bật thường xuyên. Full raw CBOR là debug facility, phải điều khiển bằng Kconfig/log level vì có chi phí CPU, UART bandwidth và RAM log-buffer.

---

## 4. Target capability architecture

```text
                         +---------------------+
                         | Gateway boot        |
                         +----------+----------+
                                    |
                                    v
                           load dev_caps NVS
                                    |
                   +----------------+----------------+
                   |                                 |
           snapshot exists                    no snapshot
                   |                                 |
                   v                                 v
              READY/CACHED                        UNKNOWN
                   |                                 |
                   +----------------+----------------+
                                    |
                              BLE READY event
                                    |
                                    v
                         capability snapshot?
                            /             \
                          yes              no
                           |                |
                     DO NOTHING      initial discovery
                                           |
                                           v
                                describe_capabilities
                                           |
                                  begin/items/end
                                           |
                                           v
                                    validate staging
                                           |
                                           v
                                       commit
```

Manual refresh:

```text
Web UI / REST / Admin
        |
        v
POST /api/capabilities/refresh
        |
        v
device_capabilities_refresh(device_id)
        |
        v
CAP_EVENT_REFRESH
        |
        v
describe_capabilities
        |
        v
staging snapshot
        |
        v
validate complete snapshot
        |
        v
compare committed vs staging
        |
        +-------------------------+
        |                         |
      same                      changed
        |                         |
        |                    replace RAM
        |                    persist NVS
        |                         |
        +-----------+-------------+
                    |
                    v
                  READY
```

---

## 5. Capability state semantics

Giữ enum hiện tại để giảm scope refactor:

```c
typedef enum {
    DEVICE_CAP_STATE_UNKNOWN = 0,
    DEVICE_CAP_STATE_DISCOVERING,
    DEVICE_CAP_STATE_READY,
    DEVICE_CAP_STATE_STALE,
    DEVICE_CAP_STATE_UNSUPPORTED,
    DEVICE_CAP_STATE_ERROR,
} device_cap_state_t;
```

Nhưng đổi semantics như sau.

### 5.1. UNKNOWN

Không có committed snapshot.

Các trường hợp:

- device mới;
- capability cache đã bị forget;
- chưa từng discovery thành công.

### 5.2. DISCOVERING

Đang chạy initial discovery hoặc manual refresh.

Nếu đã có committed snapshot, snapshot vẫn phải tồn tại trong record trong khi staging chạy.

### 5.3. READY

Có committed snapshot hợp lệ và usable.

**Snapshot load từ NVS sau reboot phải chuyển trực tiếp thành READY.**

READY không có nghĩa “vừa xác nhận trong BLE session hiện tại”. READY có nghĩa “gateway có capability metadata hợp lệ để sử dụng”.

### 5.4. STALE

Chỉ sử dụng khi gateway có lý do thực sự để nghi ngờ snapshot không còn đồng bộ, ví dụ:

- future protocol báo `capability_revision` mới nhưng refresh chưa hoàn tất;
- refresh bắt đầu nhận snapshot mới nhưng phát hiện protocol inconsistency cho thấy device schema có thể đã đổi;
- future firmware-update event báo metadata changed.

**Không dùng STALE chỉ vì disconnect/reboot.**

### 5.5. UNSUPPORTED

Device chưa có committed snapshot và `describe_capabilities` bị reject/timeout theo policy xác định là firmware legacy.

Nếu đã có committed snapshot từ trước, một refresh timeout không được chuyển snapshot thành UNSUPPORTED.

### 5.6. ERROR

Không có snapshot usable và discovery gặp lỗi nội bộ/protocol.

Nếu đã có committed snapshot, lỗi refresh nên được ghi vào refresh result/diagnostic nhưng committed snapshot vẫn usable.

---

## 6. Record model changes

Current internal record:

```c
typedef struct {
    bool used;
    bool has_committed;
    bool attempted_session;
    bool discovery_pending;
    device_capability_snapshot_t committed;
    bool staging_active;
    uint16_t staging_expected;
    device_capability_snapshot_t staging;
} capability_record_t;
```

### 6.1. Remove session-driven behavior

`attempted_session` không còn cần để kiểm soát reconnect discovery.

Khuyến nghị remove:

```c
bool attempted_session;
```

Nếu muốn giảm diff ở phase đầu, có thể giữ field nhưng không dùng làm policy. Tuy nhiên clean implementation nên bỏ field để tránh agent sau hiểu nhầm semantics cũ.

### 6.2. Add refresh diagnostics

Khuyến nghị thêm metadata runtime, không nhất thiết persist:

```c
typedef enum {
    DEVICE_CAP_REFRESH_NONE = 0,
    DEVICE_CAP_REFRESH_OK,
    DEVICE_CAP_REFRESH_UNCHANGED,
    DEVICE_CAP_REFRESH_REJECTED,
    DEVICE_CAP_REFRESH_TIMEOUT,
    DEVICE_CAP_REFRESH_PROTOCOL_ERROR,
    DEVICE_CAP_REFRESH_INTERNAL_ERROR,
} device_cap_refresh_result_t;

typedef struct {
    bool used;
    bool has_committed;
    bool discovery_pending;

    device_capability_snapshot_t committed;

    bool staging_active;
    uint16_t staging_expected;
    device_capability_snapshot_t staging;

    bool refresh_requested;
    device_cap_refresh_result_t last_refresh_result;
    int64_t last_refresh_at_ms;
} capability_record_t;
```

`refresh_requested` giúp phân biệt:

```text
initial discovery
manual refresh
```

để log và error policy rõ ràng.

---

## 7. Persistence behavior

### 7.1. NVS schema

Giữ namespace và key hiện tại:

```text
namespace: dev_caps
keys: cap00 ... cap15
```

Không thay đổi schema blob nếu không cần thiết.

Persisted data tiếp tục gồm:

- schema version;
- device ID;
- revision;
- snapshot ID;
- capability items.

Runtime diagnostics không persist.

### 7.2. Boot load

Đổi:

```c
record->committed.state = DEVICE_CAP_STATE_STALE;
```

thành:

```c
record->committed.state = DEVICE_CAP_STATE_READY;
```

Sau load thành công phải log:

```text
[CAP_CACHE_LOAD] device=lamp-1 revision=7 count=2 source=nvs state=ready
```

### 7.3. Snapshot equality

Thêm helper:

```c
static bool capability_equal(const device_capability_t *a,
                             const device_capability_t *b);

static bool snapshot_content_equal(
    const device_capability_snapshot_t *a,
    const device_capability_snapshot_t *b);
```

So sánh ít nhất:

- count;
- revision;
- từng command;
- label;
- unit;
- value_type;
- flags;
- min;
- max;
- step.

Không dùng `memcmp()` trên toàn struct nếu struct có padding hoặc runtime-only fields.

### 7.4. Commit rule

`handle_end()` phải đổi thành:

```text
complete staging?
    no  -> discard staging / report protocol error
    yes -> compare committed vs staging
             |
             +-- no committed snapshot -> commit + persist
             |
             +-- changed -> commit + persist
             |
             +-- unchanged -> keep/refresh RAM state, skip NVS write
```

Pseudo-code:

```c
bool changed = !record->has_committed ||
               !snapshot_content_equal(&record->committed,
                                       &record->staging);

record->staging.state = DEVICE_CAP_STATE_READY;
record->staging.updated_at_ms = now_ms();
record->committed = record->staging;
record->has_committed = true;
record->staging_active = false;

if (changed) {
    persist_record(index, &record->committed);
}
```

Log:

```text
[CAP_COMMIT] device=lamp-1 revision=8 count=3 changed=true persisted=true
```

hoặc:

```text
[CAP_COMMIT] device=lamp-1 revision=8 count=3 changed=false persisted=false
```

---

## 8. Lifecycle behavior changes

### 8.1. `device_capabilities_on_ready()`

Public API giữ nguyên để không phá integration trong `main/main.c`.

Current:

```c
esp_err_t device_capabilities_on_ready(const char *device_id)
{
    return enqueue_device_event(CAP_EVENT_READY, device_id);
}
```

Worker behavior mới:

```c
case CAP_EVENT_READY:
    maybe_start_initial_discovery(event.device_id);
    break;
```

Implement:

```c
static void maybe_start_initial_discovery(const char *device_id)
{
    bool has_committed = false;

    if (!lock_records()) return;
    int index = find_or_create_record_locked(device_id);
    if (index >= 0) {
        has_committed = s_records[index].has_committed;
    }
    unlock_records();

    if (has_committed) {
        ESP_LOGI(TAG,
                 "[CAP_READY_CACHE_HIT] device=%s action=skip_discovery",
                 device_id);
        return;
    }

    ESP_LOGI(TAG,
             "[CAP_READY_CACHE_MISS] device=%s action=initial_discovery",
             device_id);
    start_discovery(device_id, CAP_DISCOVERY_INITIAL);
}
```

### 8.2. Reconnect

Expected:

```text
connect -> READY -> cache exists -> skip
 disconnect
 reconnect -> READY -> cache exists -> skip
 disconnect
 reconnect -> READY -> cache exists -> skip
```

Không có `describe_capabilities` mới.

### 8.3. Disconnect

Current disconnect handler reset attempted session và gọi failure state, có thể biến committed snapshot thành STALE.

Phải đổi thành:

```text
if discovery is active for this device:
    abort staging
    keep committed snapshot if exists
else:
    do not touch capability state
```

Pseudo-code:

```c
if (record->staging_active || record->committed.state == DEVICE_CAP_STATE_DISCOVERING) {
    record->staging_active = false;
    memset(&record->staging, 0, sizeof(record->staging));

    if (record->has_committed) {
        record->committed.state = DEVICE_CAP_STATE_READY;
        record->last_refresh_result = DEVICE_CAP_REFRESH_TIMEOUT;
    } else {
        record->committed.state = DEVICE_CAP_STATE_UNKNOWN;
    }
}
```

Nếu không discovery:

```text
capability state unchanged
```

### 8.4. Manual refresh

`device_capabilities_refresh(device_id)` phải luôn có quyền schedule discovery kể cả cache tồn tại.

Preconditions:

- device tồn tại trong `device_store`;
- device đang BLE READY;
- device không có capability discovery đang chạy;
- command path không busy theo policy hiện tại.

REST layer hiện đã check connected. Capability manager vẫn phải chống duplicate refresh.

---

## 9. Discovery serialization

Current component dùng global:

```c
static bool s_discovery_active;
```

và `discovery_pending` để serialize discovery giữa devices. Có thể giữ behavior này trong version đầu.

Policy:

```text
max simultaneous capability discovery = 1 gateway-wide
```

Lý do:

- giảm BLE traffic burst;
- giảm pressure lên command executor;
- giảm notify queue burst;
- capability refresh là low-frequency administrative operation.

Manual refresh khi một discovery khác đang chạy:

```text
queue pending
```

hoặc REST trả `409` nếu muốn explicit feedback. Trong version này ưu tiên giữ queue semantics hiện có để giảm thay đổi.

---

## 10. Web API contract

### 10.1. GET `/api/capabilities`

Semantics bắt buộc:

```text
READ CACHE ONLY
```

Không gọi BLE.

Request:

```http
GET /api/capabilities?device_id=lamp-1
```

Example response:

```json
{
  "success": true,
  "data": {
    "device_id": "lamp-1",
    "state": "ready",
    "revision": 8,
    "stale": false,
    "commands": [
      {
        "name": "set_power",
        "label": "Power",
        "value_type": "boolean",
        "idempotent": true,
        "destructive": false
      }
    ]
  }
}
```

### 10.2. POST `/api/capabilities/refresh`

Semantics:

```text
EXPLICIT DISCOVERY
```

Request:

```json
{
  "device_id": "lamp-1"
}
```

Accepted:

```http
HTTP 202
```

```json
{
  "success": true,
  "message": "Capability refresh queued"
}
```

Recommended errors:

| HTTP | Code/meaning |
|---|---|
| 400 | invalid device_id/body |
| 404 | device not registered |
| 409 | capability discovery already running for same device |
| 409 | device command busy, if preflight is implemented |
| 502 | device not BLE connected/READY |
| 503 | capability queue/executor unavailable |

### 10.3. UI refresh polling

Không dùng fixed delay `2500 ms` làm source of truth.

Sau POST refresh:

```text
poll GET /api/capabilities
```

với interval khoảng:

```text
500 ms
```

và stop khi:

```text
state != discovering
```

hoặc local UI timeout khoảng 5–8 giây.

UI states:

```text
Refreshing...
Ready
Refresh failed - showing cached commands
Unsupported
Unknown
```

Nếu refresh fail nhưng old cache tồn tại:

```text
commands vẫn render bình thường
```

---

## 11. MCP behavior

MCP registry hiện là static global registry. Giữ nguyên kiến trúc này.

### 11.1. `list_device_capabilities(device_id)`

Phải chỉ đọc cache:

```text
MCP call
   |
   v
list_device_capabilities
   |
   v
device_capabilities_get
   |
   v
RAM cache
```

Không được trigger BLE discovery âm thầm.

### 11.2. `device_command`

Giữ rule:

```text
MCP static allowlist
AND
device capability validation
```

### 11.3. Optional future MCP refresh tool

Nếu cần AI chủ động refresh, thêm tool riêng:

```text
refresh_device_capabilities(device_id)
```

Không overload `list_device_capabilities` thành read + network side effect.

Tool refresh nên là administrative tool và có thể không enable mặc định.

---

# Part II — Full Message Trace for Debugging

## 12. Logging goals

Message trace phải trả lời được các câu hỏi sau chỉ bằng log:

1. Request đến từ Web hay MCP?
2. Request device nào?
3. Command gì?
4. `request_id` nào được dispatcher gán?
5. Protocol version nào được gửi xuống device?
6. CBOR encode thành bao nhiêu byte?
7. Raw CBOR bytes chính xác là gì?
8. GATT write dùng connection/value handle nào?
9. Device notify về bao nhiêu byte?
10. Raw notify bytes là gì?
11. Decode thành message gì?
12. Message được capability manager consume hay dispatcher xử lý?
13. Nếu capability flow: begin/item/end nào đã nhận?
14. ACK có match `request_id` không?
15. Tổng latency từ send tới ACK là bao nhiêu?
16. Nếu fail, fail ở encode, MTU, GATT, notify queue, decode, protocol, validation, ACK timeout hay refresh state?

---

## 13. Architecture for message trace

Không đưa formatting logic vào `log_buffer`.

`log_buffer` chỉ tiếp tục làm generic sink/ring buffer.

Thêm component/helper mới:

```text
components/message_trace/
├── CMakeLists.txt
├── Kconfig
├── message_trace.c
├── include/message_trace.h
└── test/test_message_trace.c
```

Architecture:

```text
Web/MCP
   |
   v
Dispatcher / Executor
   |
   v
message_trace summary
   |
   v
BLE Central encode/write
   |
   +--> message_trace TX decoded
   +--> message_trace TX raw CBOR
   |
Device
   |
   v
BLE Notify enqueue
   |
   +--> message_trace RX raw CBOR
   |
   v
CBOR decode
   |
   +--> message_trace RX decoded
   |
   v
Capability manager / ACK matcher
   |
   +--> state/correlation logs
   |
   v
ESP_LOGx
   |
   +--> UART/monitor
   +--> log_buffer ring
              |
              v
          GET /api/logs
              |
              v
            Web UI
```

---

## 14. Message trace Kconfig

Tạo `components/message_trace/Kconfig`:

```text
menu "Message Trace"

config MESSAGE_TRACE_ENABLE
    bool "Enable gateway message tracing"
    default y

config MESSAGE_TRACE_DECODED
    bool "Log decoded gateway message fields"
    depends on MESSAGE_TRACE_ENABLE
    default y

config MESSAGE_TRACE_RAW_CBOR
    bool "Log raw CBOR bytes"
    depends on MESSAGE_TRACE_ENABLE
    default n
    help
        Debug only. Emits complete TX/RX CBOR as chunked hex lines.

config MESSAGE_TRACE_RAW_CHUNK_BYTES
    int "Raw CBOR bytes per log line"
    depends on MESSAGE_TRACE_RAW_CBOR
    range 16 64
    default 48

endmenu
```

Recommended profiles:

### Normal development

```text
MESSAGE_TRACE_ENABLE=y
MESSAGE_TRACE_DECODED=y
MESSAGE_TRACE_RAW_CBOR=n
```

### Protocol debugging

```text
MESSAGE_TRACE_ENABLE=y
MESSAGE_TRACE_DECODED=y
MESSAGE_TRACE_RAW_CBOR=y
MESSAGE_TRACE_RAW_CHUNK_BYTES=48
```

### Production optimization

```text
MESSAGE_TRACE_ENABLE=n
```

hoặc chỉ giữ summary tùy yêu cầu field diagnostics.

---

## 15. Trace levels

### INFO

Dùng cho lifecycle/correlation summary:

```text
MSG TX
MSG RX
CAP refresh start/result
ACK result
```

Example:

```text
[MSG_TX] device=lamp-1 req=42 pv=3 type=device_command command=set_power len=37
```

### DEBUG

Decoded fields và state transitions:

```text
[MSG_TX_DECODED]
[MSG_RX_DECODED]
[CAP_BEGIN]
[CAP_ITEM]
[CAP_END]
```

### VERBOSE hoặc DEBUG guarded by RAW config

Raw CBOR chunks:

```text
[MSG_TX_RAW]
[MSG_RX_RAW]
```

### WARN

Recoverable failures:

```text
queue full
unmatched ACK
refresh timeout
cached snapshot retained
```

### ERROR

Hard protocol/transport errors:

```text
CBOR encode/decode failure
invalid capability sequence
MTU overflow
GATT write failure
```

---

## 16. Public message trace API

Recommended header:

```c
#ifndef MESSAGE_TRACE_H
#define MESSAGE_TRACE_H

#include <stddef.h>
#include <stdint.h>
#include "cbor_codec.h"

typedef enum {
    MESSAGE_TRACE_DIR_TX = 0,
    MESSAGE_TRACE_DIR_RX,
} message_trace_direction_t;

typedef enum {
    MESSAGE_TRACE_STAGE_API = 0,
    MESSAGE_TRACE_STAGE_DISPATCH,
    MESSAGE_TRACE_STAGE_BLE,
    MESSAGE_TRACE_STAGE_CAPABILITY,
    MESSAGE_TRACE_STAGE_ACK,
} message_trace_stage_t;

void message_trace_log_message(message_trace_direction_t direction,
                               message_trace_stage_t stage,
                               const char *device_id,
                               const gw_message_t *message,
                               size_t encoded_len);

void message_trace_log_raw(message_trace_direction_t direction,
                           const char *device_id,
                           uint32_t request_id,
                           const uint8_t *data,
                           size_t len);

void message_trace_log_transport_result(const char *device_id,
                                        const gw_message_t *message,
                                        int result,
                                        uint16_t conn_handle,
                                        uint16_t value_handle,
                                        uint16_t mtu);

#endif
```

API phải:

- không malloc;
- không giữ pointer sau khi return;
- không block lâu;
- không decode raw buffer lần thứ hai nếu caller đã có decoded `gw_message_t`;
- compile thành no-op khi feature disable.

---

## 17. Standard trace fields

Mỗi summary line nên chứa tối đa các field liên quan:

| Field | Meaning |
|---|---|
| `dir` | TX / RX |
| `stage` | api / dispatch / ble / capability / ack |
| `device` | device_id |
| `req` | request_id, 0 hoặc `-` nếu message không có |
| `pv` | protocol_version |
| `type` | message.type |
| `command` | message.command |
| `len` | encoded/raw byte count |
| `int` | int_value nếu có |
| `bool` | bool_value nếu có |
| `snapshot` | capability snapshot_id nếu có |
| `seq` | capability sequence nếu có |
| `total` | capability total nếu có |
| `revision` | capability_revision nếu có |

Không cố nhét tất cả field vào một line nếu vượt `LOG_ENTRY_MAX_LEN`.

---

## 18. Full raw CBOR logging

`GW_MSG_MAX_LEN = 256`. Hex dump tối đa cần khoảng 512 ký tự, lớn hơn `LOG_ENTRY_MAX_LEN = 192`.

Vì vậy **không** dùng một line raw hex duy nhất.

Chunk theo 48 bytes:

```text
[MSG_TX_RAW] device=lamp-1 req=42 off=0 len=48 data=A60103...
[MSG_TX_RAW] device=lamp-1 req=42 off=48 len=17 data=...
```

48 bytes -> 96 hex chars, đủ chỗ cho metadata trong 192-byte entry.

Pseudo-code:

```c
static void trace_hex_chunks(const char *prefix,
                             const char *device_id,
                             uint32_t request_id,
                             const uint8_t *data,
                             size_t len)
{
    for (size_t offset = 0; offset < len; offset += CHUNK_BYTES) {
        size_t chunk_len = MIN(CHUNK_BYTES, len - offset);
        char hex[(CHUNK_BYTES * 2) + 1];

        for (size_t i = 0; i < chunk_len; i++) {
            snprintf(&hex[i * 2], 3, "%02X", data[offset + i]);
        }

        ESP_LOGD(TAG,
                 "[%s] device=%s req=%lu off=%u len=%u data=%s",
                 prefix,
                 device_id,
                 (unsigned long)request_id,
                 (unsigned)offset,
                 (unsigned)chunk_len,
                 hex);
    }
}
```

Không dùng ESP_LOG_BUFFER_HEX nếu output format không đảm bảo correlation metadata ở từng line.

---

## 19. Decoded message logging

Không bắt buộc convert sang JSON rồi log nguyên JSON. Việc đó tạo buffer lớn và dễ truncate.

Nên log structured key/value trực tiếp.

Example ordinary command:

```text
[MSG_TX_DECODED] device=lamp-1 req=42 pv=3 type=device_command command=set_power bool=true
```

Capability begin:

```text
[MSG_RX_DECODED] device=lamp-1 pv=3 type=capabilities_begin command=describe_capabilities snapshot=88 total=2 revision=7
```

Capability item:

```text
[MSG_RX_DECODED] device=lamp-1 pv=3 type=capability_item command=set_brightness snapshot=88 seq=1 value_type=int min=0 max=100 step=1 unit=%
```

ACK:

```text
[MSG_RX_DECODED] device=lamp-1 req=42 pv=3 type=device_ack command=set_power bool=true
```

---

## 20. Required trace points

### 20.1. Web API ingress

Files:

```text
components/web_server/web_gateway_api.c
```

Log after request validation and before command submission:

```text
[API_COMMAND] source=web device=lamp-1 command=set_power
```

Capability refresh:

```text
[CAP_REFRESH_REQUEST] source=web device=lamp-1
```

Không log request body chứa credential/token.

### 20.2. MCP ingress

Files:

```text
components/mcp_endpoint/mcp_tools.c
components/mcp_endpoint/mcp_endpoint.c
```

Example:

```text
[MCP_TOOL] tool=device_command device=lamp-1 command=set_power
```

### 20.3. Dispatcher allocation

File:

```text
components/command_dispatcher/device_command.c
```

Sau khi `device_request_allocate()` sinh request ID:

```text
[MSG_DISPATCH] device=lamp-1 req=42 command=set_power capability=valid
```

Current `[CMD_SEND]` có thể được migrate sang standard trace format hoặc giữ lại tạm thời. Không nên để hai bộ log trùng lặp lâu dài.

### 20.4. BLE TX before write

File:

```text
components/ble_central/ble_central.c
```

Trong `ble_central_send_command()` sau encode và MTU check, trước `ble_gattc_write_no_rsp_flat()`:

1. `message_trace_log_message(TX, BLE, ...)`;
2. `message_trace_log_raw(TX, ...)` nếu raw enabled;
3. log transport metadata.

Example:

```text
[MSG_TX] device=lamp-1 req=42 pv=3 type=device_command command=set_power len=37 handle=12 chr=31 mtu=247
```

Sau write:

```text
[MSG_TX_RESULT] device=lamp-1 req=42 rc=0
```

Nếu fail:

```text
[MSG_TX_RESULT] device=lamp-1 req=42 rc=14 result=gatt_error
```

### 20.5. BLE RX immediately after notification

File:

```text
components/ble_central/ble_central_notify.c
```

Trong `ble_central_notify_enqueue()` raw bytes phải được trace trước hoặc ngay sau enqueue thành công.

Recommended:

```text
[MSG_RX_RAW] ...
```

chỉ log nếu queue accept packet. Nếu queue full:

```text
[MSG_RX_DROP] device=lamp-1 len=63 reason=notify_queue_full
```

### 20.6. RX after CBOR decode

Trong notify worker, sau decode thành công:

```text
[MSG_RX] device=lamp-1 req=42 pv=3 type=device_ack command=set_power len=31
```

và decoded detail nếu enabled.

Decode fail phải log:

```text
[MSG_RX_DECODE_ERROR] device=lamp-1 len=31
```

Raw bytes đã được log ở ingress nếu raw enabled, nên developer có bytes để reproduce decode bug.

### 20.7. Capability manager

File:

```text
components/device_capabilities/device_capabilities.c
```

Required events:

```text
[CAP_READY_CACHE_HIT]
[CAP_READY_CACHE_MISS]
[CAP_DISCOVERY_START]
[CAP_BEGIN]
[CAP_ITEM]
[CAP_END]
[CAP_COMMIT]
[CAP_REFRESH_RESULT]
[CAP_PROTOCOL_ERROR]
[CAP_CACHE_LOAD]
[CAP_CACHE_FORGET]
```

Examples:

```text
[CAP_DISCOVERY_START] device=lamp-1 mode=initial
[CAP_DISCOVERY_START] device=lamp-1 mode=manual_refresh
[CAP_BEGIN] device=lamp-1 snapshot=88 total=2 revision=7
[CAP_ITEM] device=lamp-1 snapshot=88 seq=0 command=set_power type=bool
[CAP_ITEM] device=lamp-1 snapshot=88 seq=1 command=set_brightness type=int
[CAP_END] device=lamp-1 snapshot=88 received=2 expected=2 valid=true
[CAP_COMMIT] device=lamp-1 revision=7 count=2 changed=true persisted=true
```

Reconnect with cache:

```text
[CAP_READY_CACHE_HIT] device=lamp-1 revision=7 count=2 action=skip_discovery
```

### 20.8. ACK matcher

File:

```text
components/command_dispatcher/device_request_manager.c
```

Add matched ACK trace including latency.

Để tính latency, thêm vào `pending_request_t`:

```c
int64_t started_at_us;
```

Set khi allocate hoặc ngay trước send.

Matched:

```text
[ACK_MATCH] device=lamp-1 req=42 command=set_power latency_ms=27 result=ok
```

Unmatched:

```text
[ACK_UNMATCHED] device=lamp-1 req=42 command=set_power
```

Protocol mismatch:

```text
[ACK_PROTOCOL_ERROR] device=lamp-1 req=42 expected=set_power got=set_brightness
```

### 20.9. Completion back to Web/MCP

Log outcome:

```text
[COMMAND_RESULT] source=web device=lamp-1 command=set_power status=ok latency_ms=31
```

Optional in phase 1; transport/correlation path above is mandatory.

---

## 21. Example full trace — ordinary command

Request:

```text
Web UI -> set_power(true) -> lamp-1
```

Expected log sequence:

```text
[API_COMMAND] source=web device=lamp-1 command=set_power
[MSG_DISPATCH] device=lamp-1 req=42 command=set_power capability=valid
[MSG_TX] device=lamp-1 req=42 pv=3 type=device_command command=set_power len=37 handle=12 chr=31 mtu=247
[MSG_TX_DECODED] device=lamp-1 req=42 bool=true
[MSG_TX_RAW] device=lamp-1 req=42 off=0 len=37 data=A7...
[MSG_TX_RESULT] device=lamp-1 req=42 rc=0
[MSG_RX_RAW] device=lamp-1 req=42 off=0 len=29 data=A7...
[MSG_RX] device=lamp-1 req=42 pv=3 type=device_ack command=set_power len=29
[MSG_RX_DECODED] device=lamp-1 req=42 bool=true
[ACK_MATCH] device=lamp-1 req=42 command=set_power latency_ms=27 result=ok
[COMMAND_RESULT] source=web device=lamp-1 command=set_power status=ok latency_ms=31
```

Từ trace này có thể xác định rõ lỗi nằm ở layer nào.

---

## 22. Example full trace — initial capability discovery

Device chưa có cache:

```text
BLE connection READY
```

Expected:

```text
[CAP_READY_CACHE_MISS] device=lamp-1 action=initial_discovery
[CAP_DISCOVERY_START] device=lamp-1 mode=initial
[MSG_DISPATCH] device=lamp-1 req=50 command=describe_capabilities capability=reserved
[MSG_TX] device=lamp-1 req=50 pv=3 type=device_command command=describe_capabilities len=...
[MSG_TX_RAW] ...
[MSG_TX_RESULT] device=lamp-1 req=50 rc=0

[MSG_RX] device=lamp-1 pv=3 type=capabilities_begin snapshot=88 total=2 revision=7
[CAP_BEGIN] device=lamp-1 snapshot=88 total=2 revision=7

[MSG_RX] device=lamp-1 pv=3 type=capability_item snapshot=88 seq=0 command=set_power
[CAP_ITEM] device=lamp-1 snapshot=88 seq=0 command=set_power type=bool

[MSG_RX] device=lamp-1 pv=3 type=capability_item snapshot=88 seq=1 command=set_brightness
[CAP_ITEM] device=lamp-1 snapshot=88 seq=1 command=set_brightness type=int min=0 max=100 step=1

[MSG_RX] device=lamp-1 pv=3 type=capabilities_end snapshot=88 total=2
[CAP_END] device=lamp-1 snapshot=88 received=2 expected=2 valid=true
[CAP_COMMIT] device=lamp-1 revision=7 count=2 changed=true persisted=true

[MSG_RX] device=lamp-1 req=50 pv=3 type=device_ack command=describe_capabilities
[ACK_MATCH] device=lamp-1 req=50 command=describe_capabilities latency_ms=... result=ok
[CAP_REFRESH_RESULT] device=lamp-1 mode=initial result=ok state=ready
```

---

## 23. Example trace — reconnect with cache

Expected sequence:

```text
[BLE_READY] device=lamp-1 ...
[CAP_READY_CACHE_HIT] device=lamp-1 revision=7 count=2 action=skip_discovery
```

**Không được xuất hiện:**

```text
command=describe_capabilities
CAP_DISCOVERY_START
```

Đây là acceptance condition quan trọng nhất của feature cache-first.

---

## 24. Example trace — manual refresh unchanged

```text
[CAP_REFRESH_REQUEST] source=web device=lamp-1
[CAP_DISCOVERY_START] device=lamp-1 mode=manual_refresh
...
[CAP_END] device=lamp-1 snapshot=91 received=2 expected=2 valid=true
[CAP_COMMIT] device=lamp-1 revision=7 count=2 changed=false persisted=false
[CAP_REFRESH_RESULT] device=lamp-1 mode=manual_refresh result=unchanged state=ready
```

NVS write count không tăng.

---

## 25. Example trace — manual refresh fails but cache remains

Old cache:

```text
revision=7
count=2
state=ready
```

Refresh timeout:

```text
[CAP_REFRESH_REQUEST] source=web device=lamp-1
[CAP_DISCOVERY_START] device=lamp-1 mode=manual_refresh
[MSG_TX] ... command=describe_capabilities
[CMD_TIMEOUT] device=lamp-1 req=55 command=describe_capabilities timeout_ms=...
[CAP_REFRESH_RESULT] device=lamp-1 mode=manual_refresh result=timeout cache_retained=true revision=7 count=2 state=ready
```

GET capability sau đó vẫn phải trả 2 commands cũ.

---

## 26. Log buffer changes recommended for debug usability

Current capacity:

```text
64 entries x 192 bytes ≈ 12 KiB payload
```

Full protocol trace có thể tạo 10–30 entries cho một transaction capability.

64 entries dễ overwrite quá nhanh.

### 26.1. Make capacity configurable

Đổi hardcoded macro thành Kconfig-backed value.

Suggested:

```text
config LOG_BUFFER_CAPACITY
    int "RAM log entry capacity"
    range 32 512
    default 128
```

Debug profile:

```text
LOG_BUFFER_CAPACITY=256
```

Approx raw text storage:

```text
256 x 192 ≈ 48 KiB
```

Cần benchmark free heap với BLE max connections trước khi chốt production default.

### 26.2. Keep entry length 192 initially

Không cần tăng `LOG_ENTRY_MAX_LEN` nếu raw CBOR được chunk đúng cách.

Ưu điểm:

- tránh tăng RAM quá mạnh;
- giữ behavior Web logs hiện tại;
- mỗi raw chunk vẫn đầy đủ và reconstructable.

### 26.3. Expose dropped count

`/api/status` hoặc `/api/logs` metadata nên expose:

```json
{
  "log_dropped": 3
}
```

Nếu debug protocol mà dropped > 0 thì developer biết trace không đầy đủ.

---

## 27. Web UI log debug UX

Không bắt buộc đổi backend schema ngay.

Existing `/api/logs` có thể tiếp tục trả:

```json
[
  {
    "text": "...",
    "timestamp_ms": 12345
  }
]
```

Dashboard nên bổ sung filter client-side:

```text
All
Message TX/RX
Capabilities
BLE
Errors
```

Có ô filter:

```text
device_id / request_id / command
```

Ví dụ search:

```text
req=42
```

sẽ gom toàn transaction.

Nếu raw mode bật, UI không được bỏ/reformat line raw vì developer cần reconstruct chính xác CBOR.

---

## 28. Security and privacy logging rules

Message trace chỉ dùng cho gateway-device protocol.

Không log:

- Wi-Fi password;
- Authorization header;
- MCP secret/token;
- pairing keys;
- NVS security material.

Nếu sau này `gw_message_t` có field sensitive, `message_trace` phải có central redaction rule thay vì để từng caller tự nhớ redact.

Raw CBOR có thể chứa future sensitive payload. Vì vậy:

```text
MESSAGE_TRACE_RAW_CBOR = debug-only
```

và production build nên disable.

---

## 29. Detailed file change plan

### 29.1. `components/device_capabilities/device_capabilities.c`

Required:

1. Load NVS snapshot as READY.
2. Replace session-auto discovery policy with cache-miss discovery.
3. Remove/reset logic based on `attempted_session`.
4. Disconnect must not mark normal cached snapshot stale.
5. Manual refresh remains explicit.
6. Add snapshot equality helper.
7. Skip NVS write when unchanged.
8. Add refresh diagnostics.
9. Add standardized capability logs.
10. Preserve staging atomicity.

### 29.2. `components/device_capabilities/include/device_capabilities.h`

Optional public additions:

```c
device_cap_refresh_result_t last_refresh_result;
int64_t last_refresh_at_ms;
```

Only expose if Web/UI needs them. Otherwise keep runtime diagnostics private in v1.

### 29.3. `components/device_capabilities/test/test_device_capabilities.c`

Add tests listed in section 31.

### 29.4. `components/ble_central/ble_central.c`

Add:

- TX summary;
- decoded TX;
- raw CBOR TX chunks;
- connection/value handle/MTU;
- write result.

### 29.5. `components/ble_central/ble_central_notify.c`

Add:

- raw RX trace;
- decoded RX trace;
- decode error trace;
- notify queue drop trace.

### 29.6. `components/command_dispatcher/device_command.c`

Migrate current `[CMD_SEND]` logging into standardized correlation logging.

Keep behavior unchanged.

### 29.7. `components/command_dispatcher/device_request_manager.c/.h`

Add request start timestamp and matched ACK latency.

### 29.8. `components/message_trace/*`

New reusable logging helper.

### 29.9. `components/log_buffer/Kconfig`

Add configurable capacity if RAM budget permits.

### 29.10. `components/web_server/web_gateway_api.c`

Ensure GET is cache-only.

Ensure POST refresh is explicit.

Improve conflict/error handling if possible.

### 29.11. `components/web_server/www/dashboard.html`

- replace fixed 2.5s refresh delay with polling;
- show refresh state;
- keep cached controls visible on refresh failure;
- add log filter by message/capability/request ID if debug panel exists.

### 29.12. `main/main.c`

No architecture change required.

Keep:

```c
ble_central_set_lifecycle_callbacks(on_device_ready,
                                    device_capabilities_on_disconnect);
```

`on_device_ready()` still forwards event. Capability component decides whether discovery is necessary.

---

## 30. Implementation order

Recommended sequence:

### Phase A — capability policy

1. Change persisted snapshot boot state to READY.
2. Implement cache-hit/cache-miss READY behavior.
3. Fix disconnect semantics.
4. Add snapshot equality.
5. Skip unchanged NVS write.
6. Add capability unit tests.

Do not touch Web UI until component tests pass.

### Phase B — message trace helper

1. Create `message_trace` component.
2. Implement decoded summary.
3. Implement raw chunk dump.
4. Unit test chunking/format safety.
5. Integrate TX path.
6. Integrate RX path.
7. Integrate capability events.
8. Add request latency.

### Phase C — REST/UI

1. Validate GET cache-only behavior.
2. Validate manual refresh.
3. Replace fixed-delay UI refresh with polling.
4. Add debug log filters.
5. Expose log dropped metric.

### Phase D — integration/stress

1. reboot cache test;
2. reconnect no-refresh test;
3. manual refresh unchanged test;
4. manual refresh changed test;
5. refresh timeout retains cache test;
6. 5–10 device reconnect stress;
7. raw trace load test;
8. heap/stack/log-drop measurement.

---

## 31. Required tests

### 31.1. Capability component unit tests

#### TC-CAP-001 — first READY discovers

Given:

```text
no committed snapshot
```

When:

```text
on_ready(device)
```

Then exactly one `describe_capabilities` submit occurs.

#### TC-CAP-002 — second READY with cache does not discover

Given committed READY snapshot.

When READY event occurs.

Then submit count remains zero.

#### TC-CAP-003 — disconnect/reconnect does not discover with cache

Sequence:

```text
READY snapshot exists
DISCONNECT
READY
```

Expected:

```text
no discovery submit
snapshot remains READY
```

#### TC-CAP-004 — NVS restore is immediately usable

After load persisted:

```text
state == READY
count preserved
revision preserved
```

READY event must not submit discovery.

#### TC-CAP-005 — manual refresh always discovers

Given READY cached snapshot.

Call:

```c
device_capabilities_refresh(device_id)
```

Expected exactly one discovery submit.

#### TC-CAP-006 — unchanged refresh skips persist

Given revision/content same.

Expected:

```text
state READY
content unchanged
persist call count = 0
refresh result = UNCHANGED
```

Persistence should be hookable/mocked for deterministic test if current code cannot observe call count.

#### TC-CAP-007 — changed refresh persists

Change one field or revision.

Expected:

```text
commit new snapshot
persist call count = 1
```

#### TC-CAP-008 — failed refresh retains old cache

Given old snapshot.

Trigger timeout/reject/protocol error.

Expected:

```text
old count preserved
old revision preserved
commands still validate
```

#### TC-CAP-009 — incomplete snapshot never replaces old cache

Begin + partial items + end invalid.

Old snapshot must remain.

#### TC-CAP-010 — forget removes RAM and NVS

After forget:

```text
get -> UNKNOWN
next READY -> discovery occurs
```

### 31.2. Message trace tests

#### TC-TRACE-001 — TX summary contains correlation fields

Expected fields:

```text
device
request_id
protocol_version
type
command
```

#### TC-TRACE-002 — RX ACK summary contains request_id

#### TC-TRACE-003 — raw 256-byte message is fully chunked

Reconstruct hex chunks and compare byte-for-byte with original input.

#### TC-TRACE-004 — each raw log line fits entry size

```text
strlen(line) < LOG_ENTRY_MAX_LEN
```

#### TC-TRACE-005 — trace disabled is no-op

No allocation, no output-specific side effect.

#### TC-TRACE-006 — no out-of-bounds for max GW message

Test exactly `GW_MSG_MAX_LEN`.

#### TC-TRACE-007 — decode failure still leaves raw trace available

#### TC-TRACE-008 — ACK latency non-negative and correlated

### 31.3. REST integration tests

#### TC-REST-CAP-001

GET cached capabilities does not invoke refresh/submitter.

#### TC-REST-CAP-002

POST refresh returns 202 for connected READY device.

#### TC-REST-CAP-003

POST refresh returns conflict/busy when appropriate.

### 31.4. Hardware/integration tests

#### TC-HW-001 — repeated reconnect

Perform 20 reconnects with existing cache.

Expected:

```text
describe_capabilities count = 0 after initial snapshot
```

#### TC-HW-002 — gateway reboot

After reboot:

```text
capability controls available before manual refresh
```

#### TC-HW-003 — manual refresh

Button refresh must generate exactly one capability transaction.

#### TC-HW-004 — raw protocol trace

Enable raw trace, execute capability refresh, reconstruct all CBOR frames from `/api/logs` or serial monitor and decode offline.

#### TC-HW-005 — log pressure

Run 10 devices with normal telemetry/commands while raw trace enabled.

Measure:

```text
free heap
log_buffer dropped_count
notify dropped metrics
worker stack watermark
command latency
```

---

## 32. Acceptance criteria

Feature hoàn thành khi tất cả điều kiện sau đúng.

### Capability behavior

- [ ] First connection of an unknown device triggers discovery.
- [ ] Existing cached device does not discovery on BLE reconnect.
- [ ] Existing cached device does not discovery after gateway reboot.
- [ ] `GET /api/capabilities` never talks to BLE.
- [ ] Manual Refresh explicitly triggers one discovery.
- [ ] Failed refresh never deletes a previously valid snapshot.
- [ ] Unchanged refresh does not write NVS.
- [ ] Changed refresh updates RAM + NVS atomically after complete snapshot.
- [ ] Device delete/forget removes capability cache.

### Logging/debug

- [ ] Every BLE TX command has summary log.
- [ ] Every accepted BLE RX notification has summary or decode-error log.
- [ ] TX/RX raw bytes are available when raw debug config is enabled.
- [ ] Raw bytes are complete and reconstructable.
- [ ] `request_id` links send and ACK.
- [ ] Capability begin/item/end is visible in trace.
- [ ] GATT send failure is visible.
- [ ] MTU rejection is visible.
- [ ] Notify queue drop is visible.
- [ ] CBOR decode failure is visible.
- [ ] ACK unmatched/protocol mismatch is visible.
- [ ] ACK latency is visible.
- [ ] `log_buffer` dropped count can be checked.

### UI

- [ ] Capability refresh uses polling, not fixed 2.5-second assumption.
- [ ] Cached commands remain visible if refresh fails.
- [ ] Debug log can be filtered by `device_id`, `request_id` or trace prefix.

---

## 33. Non-goals for this version

Không triển khai trong scope này:

- periodic capability polling;
- dynamic MCP tools per device command;
- capability TTL auto-expiration;
- automatic refresh every N hours/days;
- capability refresh on every reconnect;
- application-level BLE fragmentation;
- persistent flash storage for runtime log history;
- remote cloud log upload;
- binary PCAP format;
- Wireshark plugin;
- protocol v4.

---

## 34. Future optional capability revision optimization

Sau này device có thể advertise hoặc handshake một lightweight revision:

```text
capability_revision=8
```

Gateway cache:

```text
revision=7
```

Then:

```text
7 != 8 -> auto refresh once
```

Nếu revision giống nhau:

```text
skip
```

Đây là future optimization. Không cần cho phiên bản hiện tại vì manual refresh đã đáp ứng mục tiêu và đơn giản hơn.

---

## 35. Final target behavior

Sau implementation, capability subsystem phải có tính chất:

```text
CAPABILITY = PERSISTENT DEVICE METADATA
```

không phải:

```text
CAPABILITY = PER-CONNECTION SESSION DATA
```

Và debugging phải có chuỗi quan sát được:

```text
Web/MCP request
   -> dispatcher
   -> request_id
   -> encoded CBOR
   -> BLE TX raw
   -> device
   -> BLE RX raw
   -> decoded message
   -> capability/ACK handling
   -> result + latency
```

Hai thay đổi này kết hợp giúp hệ thống giảm BLE traffic không cần thiết, giảm contention với command thật, giảm write NVS, đồng thời làm cho lỗi protocol/transport có thể tái hiện và phân tích chính xác bằng log.

---

## 36. Source files reviewed for this specification

Repository paths reviewed:

```text
components/device_capabilities/device_capabilities.c
components/device_capabilities/include/device_capabilities.h
components/device_capabilities/test/test_device_capabilities.c
components/ble_central/ble_central.c
components/ble_central/ble_central_notify.c
components/ble_central/ble_central_gatt.c
components/cbor_codec/include/cbor_codec.h
components/command_dispatcher/device_command.c
components/command_dispatcher/device_request_manager.c
components/log_buffer/include/log_buffer.h
components/log_buffer/log_buffer.c
components/log_buffer/Kconfig
components/web_server/web_system_api.c
main/main.c
docs/Thiet_Ke_Device_Capability_Discovery.md
```

Project architecture basis:

```text
docs/ESP32_BLE_Gateway_Project_Framework.md
```

