# Refactor Plan — `components/command_dispatcher`

**Project:** ESP32 BLE Gateway  
**Target:** ESP32-S3 / ESP-IDF / NimBLE  
**Component:** `components/command_dispatcher`  
**Document status:** Proposed refactor plan  
**Scope:** Phase 1 stability and maintainability

---

## 1. Mục tiêu

Tài liệu này đề xuất kế hoạch refactor `components/command_dispatcher` dựa trên implementation hiện tại và kiến trúc tổng thể của ESP32 BLE Gateway.

Mục tiêu chính:

1. Loại bỏ khả năng **nhận nhầm ACK** giữa device notification và command response.
2. Làm rõ **lifecycle của device** khi add/delete/forget BLE bond.
3. Chuẩn hóa **result contract** giữa dispatcher và các transport layer như Web UI / MCP.
4. Xác định rõ **concurrency model** của dispatcher.
5. Làm rõ contract của **command registry**.
6. Tăng test coverage cho các case concurrency, timeout và lifecycle.
7. Giữ dispatcher đơn giản, không biến gateway thành business-logic server.
8. Chuẩn bị nền tảng cho mở rộng nhiều transport và multi-gateway trong tương lai.

---

## 2. Nguyên tắc refactor

Refactor phải tuân theo các nguyên tắc sau:

- Không thay đổi kiến trúc cốt lõi: mọi command vẫn đi qua một dispatcher chung.
- `gateway_command` và `device_command` vẫn là hai nhóm xử lý độc lập.
- Gateway command vẫn sử dụng registry pattern.
- BLE transport không chứa business logic.
- Dispatcher không phụ thuộc trực tiếp vào HTTP/MCP transport semantics.
- Tránh over-engineering; ưu tiên deterministic behavior và khả năng test.
- Mọi thay đổi protocol phải có migration path rõ ràng.
- Không đưa dynamic allocation không cần thiết vào hot path.
- Không giữ mutex trong thời gian chờ BLE I/O.
- Không cho phép notification bất đồng bộ hoàn tất nhầm một command đang pending.

---

## 3. Kiến trúc hiện tại

Luồng tổng quát:

```text
Web UI / MCP
      |
      v
gw_message_t
      |
      v
command_dispatcher_handle()
      |
      +---------------------------+
      |                           |
      v                           v
gateway_command              device_command
      |                           |
      v                           v
command registry              BLE Central
      |                           |
      v                           v
handler                       peripheral
                                  |
                                  v
                              notification
                                  |
                                  v
command_dispatcher_on_device_notify()
```

Các file chính:

```text
components/command_dispatcher/
|
|-- command_dispatcher.c
|-- command_registry.c
|-- gateway_commands.c
|-- device_command.c
|-- command_dispatcher_internal.h
|-- include/
|   `-- command_dispatcher.h
`-- test/
    `-- test_command_dispatcher.c
```

---

## 4. Các vấn đề cần refactor

### 4.1. P0 — ACK correlation chưa đủ an toàn

#### Hiện trạng

Pending command hiện được correlate chủ yếu bằng:

```text
device_id
command
```

Notification có `command` rỗng cũng có thể được xem là ACK cho command đang pending trên cùng device.

Điều này tạo hai failure mode:

#### False ACK

```text
Gateway                  Device
   |                        |
   | set_relay              |
   |----------------------->|
   |                        |
   | telemetry              |
   |<-----------------------|
   |                        |
   | telemetry bị match     |
   | thành ACK              |
```

#### Stale ACK

```text
request A: set_relay
        |
        +-- timeout

request B: set_relay
        |
        +-- ACK cũ của A đến muộn
        |
        +-- bị match với B
```

#### Mức độ

**Priority: P0 / Critical correctness**

Đây là vấn đề phải sửa trước khi mở rộng MCP/Web concurrency.

---

### 4.2. P0 — Delete device có thể để lại BLE bond orphan

#### Hiện trạng

Luồng hiện tại:

```text
device_store_delete(device_id)
        |
        v
ble_central_forget_device(device_id)
```

Nếu BLE Central không còn runtime connection slot, `ble_central_forget_device()` có thể cần đọc BLE address từ `device_store`.

Nhưng store entry đã bị xóa trước đó.

Kết quả:

```text
device disconnected
        |
delete device
        |
store entry removed
        |
BLE layer không còn peer address
        |
bond có thể không bị xóa
```

#### Mức độ

**Priority: P0 / Lifecycle correctness**

---

### 4.3. P1 — Dispatcher synchronous-block tới ACK timeout

`device_command_handle()` chờ semaphore tối đa:

```c
DISPATCHER_ACK_TIMEOUT_MS
```

Hiện là 2000 ms.

Nếu HTTP/MCP handler gọi dispatcher trực tiếp:

```text
HTTP request task
      |
      v
command_dispatcher_handle()
      |
      v
wait BLE ACK
      |
      +-- block <= 2 s
```

Điều này làm transport layer phụ thuộc latency BLE và có thể trở thành bottleneck khi tăng số client.

#### Mức độ

**Priority: P1 / Scalability and responsiveness**

---

### 4.4. P1 — Result contract đang trộn text và JSON

`dispatch_result_t` hiện mang:

```c
success
message[]
```

Nhưng `message` có thể chứa:

```text
Device plug-1 added
```

hoặc:

```json
[
  {
    "device_id": "plug-1"
  }
]
```

Transport layer phải tự đoán payload là text hay JSON.

#### Mức độ

**Priority: P1 / API contract**

---

### 4.5. P1 — Registry API có lifetime contract chưa rõ

`command_dispatcher_get_registered_names()` trả pointer tới internal storage.

Sau khi mutex được release, caller vẫn giữ pointer đó.

Điều này chỉ an toàn nếu registry được xem là immutable sau init.

Trong khi API hiện tại lại cho phép runtime registration.

#### Mức độ

**Priority: P1 / API correctness**

---

### 4.6. P2 — `command_dispatcher_init()` reset dynamic registry

Mỗi lần init:

```text
registry reset
default command register
```

Nếu caller đã register custom command rồi init lại, custom command biến mất.

Cần chọn một contract rõ ràng:

- init chỉ được gọi một lần; hoặc
- init có semantics reset; hoặc
- init idempotent.

#### Mức độ

**Priority: P2 / API clarity**

---

### 4.7. P2 — `add_device` không phản ánh lỗi connect

Persistent add và runtime BLE connect là hai operation khác nhau.

Hiện tại device có thể được add thành công nhưng BLE connect request thất bại, trong khi result vẫn chỉ trả:

```text
Device <id> added
```

Điều này làm upper layer khó phân biệt:

```text
persisted successfully
connect started successfully
```

#### Mức độ

**Priority: P2 / Result accuracy**

---

## 5. Kiến trúc mục tiêu

Kiến trúc mục tiêu giữ dispatcher đơn giản nhưng làm rõ protocol và ownership:

```text
                 Web / MCP
                    |
                    v
             transport adapter
                    |
                    v
               gw_message_t
                    |
                    v
          command_dispatcher_handle()
                    |
          +---------+---------+
          |                   |
          v                   v
  gateway_command       device_command
          |                   |
          v                   v
     registry             request manager
                              |
                              v
                         BLE Central
                              |
                              v
                         peripheral
                              |
                              v
                         notification
                              |
                              v
                      request_id match
```

Điểm quan trọng:

- Dispatcher quyết định routing.
- Device request manager quản lý pending request.
- BLE Central chỉ gửi/nhận transport data.
- Notification chỉ complete request khi correlation metadata khớp chính xác.

---

## 6. Refactor 1 — Thêm correlation ID

### 6.1. Thay đổi protocol

Bổ sung `request_id` vào `gw_message_t`.

Đề xuất:

```c
typedef struct {
    uint8_t protocol_version;

    char type[GW_MSG_TYPE_LEN];
    char device_id[GW_MSG_DEVICE_ID_LEN];
    char command[GW_MSG_COMMAND_LEN];

    uint32_t request_id;
    int has_request_id;

    int int_value;
    int bool_value;

    int has_device_id;

    char name[GW_MSG_NAME_LEN];
    char device_type[GW_MSG_DEVICE_TYPE_LEN];

    uint8_t ble_addr[6];
    uint8_t ble_addr_type;
    int has_ble_addr;
} gw_message_t;
```

#### Lý do dùng `uint32_t`

- đủ cho local monotonic request sequence;
- chi phí nhỏ;
- dễ encode CBOR;
- không cần UUID;
- phù hợp MCU.

---

### 6.2. Phân biệt message type

Đề xuất protocol message type rõ ràng:

```text
gateway_command
device_command
device_ack
device_event
```

Không dùng notification có `command` rỗng như ACK fallback.

#### Command

```json
{
  "type": "device_command",
  "device_id": "relay-1",
  "command": "set_power",
  "request_id": 1042,
  "bool_value": true
}
```

#### ACK

```json
{
  "type": "device_ack",
  "device_id": "relay-1",
  "command": "set_power",
  "request_id": 1042,
  "bool_value": true
}
```

#### Event

```json
{
  "type": "device_event",
  "device_id": "relay-1",
  "command": "state_changed",
  "bool_value": true
}
```

Event không complete pending command.

---

### 6.3. Correlation rule mới

Pending request chỉ được complete khi:

```text
msg.type == "device_ack"
AND device_id matches
AND request_id matches
```

`command` có thể được dùng như validation bổ sung:

```text
AND command matches
```

nhưng không được dùng làm primary correlation key.

---

### 6.4. Ownership và sinh request ID

`request_id` trong tài liệu này là correlation ID của **Gateway ↔ BLE device**, không phải JSON-RPC `id` của MCP và cũng không phải HTTP request ID. Transport adapter không được dùng trực tiếp ID của transport làm BLE correlation ID.

Dispatcher/request manager là owner của BLE `request_id`: trước khi forward một `device_command`, gateway gán một ID mới và peripheral phải echo chính xác ID đó trong `device_ack`. Upper layer không cần biết hoặc tự chọn ID này.

Vì public API hiện nhận `const gw_message_t *msg`, implementation không được sửa trực tiếp message của caller. `device_command_handle()` nên tạo một bản sao local dùng cho wire protocol:

```c
gw_message_t wire_msg = *msg;
wire_msg.request_id = request_id;
wire_msg.has_request_id = 1;

ble_central_send_command(
    wire_msg.device_id,
    &wire_msg
);
```

Như vậy transport-originated message vẫn immutable, còn BLE correlation metadata thuộc ownership của dispatcher.

Đề xuất giữ một counter trong dispatcher/request manager:

```c
static uint32_t s_next_request_id;
```

Sinh ID dưới lock:

```c
request_id = ++s_next_request_id;
```

Nếu wrap về 0:

```c
request_id = ++s_next_request_id;
```

để tránh dùng `0` làm valid request ID.

#### Constraint

Không reuse request ID đang pending.

Với tối đa khoảng 10 device, kiểm tra collision rất rẻ.

---

## 7. Refactor 2 — Tách Pending Request Manager

Thay vì giữ toàn bộ logic trong `device_command.c`, đề xuất tách phần correlation thành module nội bộ. Đây là **refactor cấu trúc P1**, không phải điều kiện bắt buộc để sửa lỗi P0: nếu cần giảm blast radius, có thể triển khai `request_id` và ACK matching an toàn ngay trong `device_command.c`, khóa behavior bằng test, rồi mới tách module ở change-set kế tiếp.

Cấu trúc:

```text
device_command.c
device_request_manager.c
device_request_manager.h
```

Không cần public component API.

---

### 7.1. Data structure

Đề xuất:

```c
typedef struct {
    bool in_use;

    uint32_t request_id;

    char device_id[GW_MSG_DEVICE_ID_LEN];
    char command[GW_MSG_COMMAND_LEN];

    gw_message_t response;

    SemaphoreHandle_t semaphore;
} pending_request_t;
```

---

### 7.2. API nội bộ

```c
int device_request_manager_init(void);

int device_request_allocate(
    const char *device_id,
    const char *command,
    uint32_t request_id,
    pending_request_t **out_request
);

int device_request_wait(
    pending_request_t *request,
    TickType_t timeout
);

bool device_request_complete(
    const char *device_id,
    const gw_message_t *response
);

void device_request_release(
    pending_request_t *request
);
```

---

### 7.3. Invariant

Module phải đảm bảo:

```text
1 pending request / device
```

trong Phase 1.

Không cần hỗ trợ nhiều command concurrent trên cùng peripheral ngay lập tức.

Điều này giữ implementation đơn giản và tránh pressure lên BLE link.

---

## 8. Refactor 3 — Sửa lifecycle delete device

### 8.1. Không để BLE layer quay lại lookup store sau khi entry đã bị xóa

Đề xuất API mới:

```c
int ble_central_forget_peer(
    const char *device_id,
    const uint8_t ble_addr[6],
    uint8_t ble_addr_type,
    bool has_ble_addr
);
```

Dispatcher đã có snapshot `device_entry_t existing`.

Luồng mới:

```text
device_store_get(device_id, &existing)
        |
        v
ble_central_forget_peer(
    existing.device_id,
    existing.ble_addr,
    existing.ble_addr_type,
    existing.has_ble_addr
)
        |
        v
device_store_delete(device_id)
```

---

### 8.2. Transaction semantics

Đề xuất semantics:

#### BLE forget thành công + store delete thành công

```text
success
```

#### BLE forget thất bại + store chưa delete

```text
operation failed
store giữ nguyên
```

#### BLE forget thành công + store delete thất bại

```text
operation failed
device vẫn còn config nhưng bond đã bị xóa
```

Case cuối hiếm nhưng phải được log rõ.

Để semantics trên có ý nghĩa, API `ble_central_forget_peer()` phải **propagate lỗi xóa bond thực tế** thay vì chỉ log warning rồi trả success. Caller cần phân biệt ít nhất:

```text
peer runtime cleanup thành công
bond không tồn tại
bond xóa thành công
bond xóa thất bại
```

`bond không tồn tại` có thể xem là idempotent success; lỗi NVS/BLE store thực sự phải được trả lên dispatcher.

Có thể cải thiện bằng best-effort recovery, nhưng Phase 1 chưa cần transaction framework.

---

## 9. Refactor 4 — Chuẩn hóa `dispatch_result_t`

Đề xuất:

```c
typedef enum {
    DISPATCH_RESULT_TEXT = 0,
    DISPATCH_RESULT_JSON,
} dispatch_result_format_t;

typedef enum {
    DISPATCH_STATUS_OK = 0,

    DISPATCH_STATUS_INVALID_ARGUMENT,
    DISPATCH_STATUS_NOT_FOUND,
    DISPATCH_STATUS_BUSY,
    DISPATCH_STATUS_TIMEOUT,
    DISPATCH_STATUS_NOT_CONNECTED,
    DISPATCH_STATUS_TRANSPORT_ERROR,
    DISPATCH_STATUS_INTERNAL_ERROR,
} dispatch_status_t;

typedef struct {
    dispatch_status_t status;
    dispatch_result_format_t format;

    char payload[DISPATCHER_MAX_RESULT_LEN];
} dispatch_result_t;
```

---

### 9.1. Helper API

Đề xuất:

```c
void command_dispatcher_set_text_result(
    dispatch_result_t *result,
    dispatch_status_t status,
    const char *format,
    ...
);

void command_dispatcher_set_json_result(
    dispatch_result_t *result,
    dispatch_status_t status,
    const char *json
);
```

---

### 9.2. Lợi ích

Transport layer có thể map deterministic:

```text
DISPATCH_STATUS_OK
    -> HTTP 200

DISPATCH_STATUS_INVALID_ARGUMENT
    -> HTTP 400

DISPATCH_STATUS_NOT_FOUND
    -> HTTP 404

DISPATCH_STATUS_BUSY
    -> HTTP 409

DISPATCH_STATUS_TIMEOUT
    -> HTTP 504

DISPATCH_STATUS_INTERNAL_ERROR
    -> HTTP 500
```

MCP adapter cũng có thể map status sang JSON-RPC error ổn định.

Dispatcher vẫn không phụ thuộc HTTP hoặc MCP.

`status` phải là **single source of truth**. Không giữ thêm field `success` trong struct vì hai field có thể rơi vào trạng thái mâu thuẫn, ví dụ `success=true` nhưng `status=DISPATCH_STATUS_TIMEOUT`.

Nếu caller cần boolean:

```c
static inline bool dispatch_result_is_ok(
    const dispatch_result_t *result)
{
    return result != NULL &&
           result->status == DISPATCH_STATUS_OK;
}
```

---

## 10. Refactor 5 — Registry contract

Có hai lựa chọn.

### Option A — Immutable after init

Phù hợp nhất với firmware hiện tại.

Luồng:

```text
command_dispatcher_init()
        |
register defaults
        |
application registers custom commands
        |
command_dispatcher_freeze_registry()
        |
start accepting dispatch requests
```

`command_dispatcher_handle()` không nên chạy trước khi registry được freeze; nếu xảy ra, trả `DISPATCH_STATUS_INTERNAL_ERROR` hoặc state error tương đương. Điều này biến startup ordering thành một invariant có thể test thay vì assumption ngầm.

Sau freeze:

```c
command_dispatcher_register()
```

trả lỗi.

#### Ưu điểm

- deterministic;
- đơn giản;
- ít concurrency issue;
- pointer lifetime rõ hơn.

---

### Option B — Fully dynamic registry

Nếu cần runtime plugin-like behavior:

- register;
- unregister;
- copy-out API;
- generation/lifetime handling.

Đây là complexity không cần thiết cho Phase 1.

#### Quyết định đề xuất

**Chọn Option A.**

---

### 10.1. API lấy command names

Không trả pointer internal.

Đổi từ:

```c
int command_dispatcher_get_registered_names(
    const char **out_names,
    int max_names
);
```

sang:

```c
int command_dispatcher_get_registered_names(
    char out_names[][GW_MSG_COMMAND_LEN],
    int max_names
);
```

Caller nhận copy.

---

## 11. Refactor 6 — Init semantics

Đề xuất init chỉ cho phép một lần.

```c
int command_dispatcher_init(void);
```

Nếu gọi lần hai:

```text
ESP_ERR_INVALID_STATE
```

hoặc project-specific error code tương đương.

Internal state:

```c
static bool s_initialized;
```

Điều này tránh silent reset registry và pending manager.

---

## 12. Refactor 7 — `add_device` result semantics

Tách rõ:

```text
persistent operation
runtime connection operation
```

Đề xuất result JSON:

```json
{
  "device_id": "sensor-1",
  "persisted": true,
  "connect_requested": true
}
```

Nếu host chưa sẵn sàng:

```json
{
  "device_id": "sensor-1",
  "persisted": true,
  "connect_requested": false
}
```

Status tổng thể có thể vẫn là success nếu contract của `add_device` chỉ yêu cầu persistence.

Nếu API muốn `add_and_connect`, nên tạo command riêng.

#### Khuyến nghị

Giữ `add_device` là persistent operation.

Connection được xem là best-effort side effect.

---

## 13. Refactor 8 — Concurrency model

Có hai phương án.

### Option A — Giữ synchronous dispatcher

```text
caller
  |
dispatcher
  |
BLE send
  |
wait ACK
  |
return
```

#### Ưu điểm

- implementation đơn giản;
- thay đổi ít;
- phù hợp giai đoạn đầu.

#### Nhược điểm

- caller bị block;
- HTTP/MCP task bị giữ;
- khó scale.

---

### Option B — Dispatcher worker task + queue

```text
HTTP/MCP
   |
   v
dispatcher queue
   |
   v
dispatcher worker
   |
   v
BLE
```

#### Ưu điểm

- transport layer không block BLE trực tiếp;
- ownership rõ;
- backpressure kiểm soát được;
- dễ instrument queue depth.

#### Nhược điểm

- phải thiết kế async response/callback/future;
- tăng complexity.

---

### Quyết định đề xuất cho Phase 1

**Không chuyển toàn dispatcher sang async trong cùng refactor P0.**

Thứ tự:

```text
1. sửa protocol correlation
2. sửa lifecycle
3. chuẩn hóa result
4. tăng test coverage
5. benchmark HTTP/MCP concurrency
6. chỉ thêm worker queue nếu benchmark chứng minh cần
```

Lý do:

- giảm blast radius;
- dễ verify correctness;
- tránh refactor protocol và execution model cùng lúc.

---

## 14. API mục tiêu đề xuất

Public header:

```c
#ifndef COMMAND_DISPATCHER_H
#define COMMAND_DISPATCHER_H

#include <stdbool.h>
#include "cbor_codec.h"

#define DISPATCHER_MAX_RESULT_LEN 4096
#define DISPATCHER_MAX_COMMANDS   16
#define DISPATCHER_ACK_TIMEOUT_MS 2000

typedef enum {
    DISPATCH_RESULT_TEXT = 0,
    DISPATCH_RESULT_JSON,
} dispatch_result_format_t;

typedef enum {
    DISPATCH_STATUS_OK = 0,
    DISPATCH_STATUS_INVALID_ARGUMENT,
    DISPATCH_STATUS_NOT_FOUND,
    DISPATCH_STATUS_BUSY,
    DISPATCH_STATUS_TIMEOUT,
    DISPATCH_STATUS_NOT_CONNECTED,
    DISPATCH_STATUS_TRANSPORT_ERROR,
    DISPATCH_STATUS_INTERNAL_ERROR,
} dispatch_status_t;

typedef struct {
    dispatch_status_t status;
    dispatch_result_format_t format;
    char payload[DISPATCHER_MAX_RESULT_LEN];
} dispatch_result_t;

typedef void (*gateway_command_fn_t)(
    const gw_message_t *msg,
    dispatch_result_t *result
);

int command_dispatcher_init(void);

int command_dispatcher_register(
    const char *command_name,
    gateway_command_fn_t fn
);

int command_dispatcher_freeze_registry(void);

int command_dispatcher_get_registered_names(
    char out_names[][GW_MSG_COMMAND_LEN],
    int max_names
);

bool command_dispatcher_is_registered(
    const char *command_name
);

void command_dispatcher_handle(
    const gw_message_t *msg,
    dispatch_result_t *result
);

void command_dispatcher_on_device_notify(
    const char *device_id,
    const gw_message_t *msg
);

#endif
```

---

## 15. Validation ở dispatcher boundary

Có hai ingress boundary khác nhau và phải validate riêng.

### 15.1. Command ingress

`command_dispatcher_handle()` phải validate tối thiểu:

```text
msg != NULL
type != empty
type null-terminated
protocol_version supported
```

Gateway command:

```text
command != empty
```

Device command:

```text
device_id present
command != empty
```

### 15.2. BLE notification ingress

`command_dispatcher_on_device_notify()` / request manager phải validate notification trước khi correlation:

```text
device_id != NULL
msg != NULL
protocol_version supported
type == device_ack OR type == device_event
```

Với `device_ack`:

```text
device_id present
request_id present
request_id != 0
command != empty
```

`device_event` không được đi vào pending-request completion path.

Không nên để các handler tự lặp lại validation cơ bản không cần thiết.

---

## 16. Logging

Đề xuất format nhất quán.

#### Send

```text
[CMD_SEND] device=relay-1 request_id=1042 command=set_power
```

#### ACK

```text
[CMD_ACK] device=relay-1 request_id=1042 command=set_power result=ok
```

#### Timeout

```text
[CMD_TIMEOUT] device=relay-1 request_id=1042 command=set_power timeout_ms=2000
```

#### Event

```text
[DEVICE_EVENT] device=relay-1 command=state_changed
```

#### Unexpected ACK

```text
[ACK_UNMATCHED] device=relay-1 request_id=1042 command=set_power
```

Không log toàn bộ payload nếu có khả năng chứa data lớn.

---

## 17. Test plan bắt buộc

### 17.1. Dispatcher routing

- `NULL` message.
- unknown message type.
- valid gateway command.
- unknown gateway command.
- valid device command.
- missing device ID.
- missing command.
- unsupported protocol version.

---

### 17.2. Registry

- register valid command.
- reject duplicate.
- reject empty name.
- reject over-length name.
- reject null handler.
- reject over capacity.
- copy command names correctly.
- register after freeze phải fail.
- second init phải fail.

---

### 17.3. ACK correlation

#### Case 1 — Valid ACK

```text
request_id=10
ACK request_id=10
=> success
```

#### Case 2 — Wrong request ID

```text
request_id=10
ACK request_id=11
=> không wake request
```

#### Case 3 — Wrong device

```text
device=A request_id=10
ACK device=B request_id=10
=> không wake
```

#### Case 4 — Device event

```text
pending request
device_event arrives
=> không wake
```

#### Case 5 — ACK command mismatch

```text
same request_id
wrong command
=> reject/log protocol error
```

#### Case 6 — Stale ACK after timeout

```text
request A timeout
request B starts
ACK of A arrives
=> không complete B
```

#### Case 7 — Two devices concurrently

```text
A pending
B pending
=> cả hai độc lập
```

#### Case 8 — Same device second command

```text
A command 1 pending
A command 2
=> BUSY
```

#### Case 9 — Send failure

```text
BLE send fails
=> pending slot released
```

#### Case 10 — Timeout

```text
no ACK
=> TIMEOUT
=> slot reusable
```

---

### 17.4. Device lifecycle

- delete connected device.
- delete disconnected device.
- delete bonded device without runtime slot.
- BLE forget failure.
- store delete failure.
- add device without BLE address.
- add device with BLE address.
- add persisted but connect request failure.

---

### 17.5. Result contract

- text result sets `format=TEXT`.
- JSON result sets `format=JSON`.
- `status` là single source of truth; không có duplicate `success` flag.
- status code is deterministic.
- payload truncation handled safely.
- invalid JSON generation returns internal error.

---

## 18. Migration plan

### Phase A — Protocol safety

Files dự kiến:

```text
components/cbor_codec/include/cbor_codec.h
components/cbor_codec/*
components/command_dispatcher/device_command.c
components/command_dispatcher/*
device firmware counterpart
```

Thay đổi:

1. thêm `request_id`;
2. thêm `has_request_id`;
3. thêm `device_ack`;
4. thêm `device_event`;
5. update CBOR encode/decode;
6. update device firmware response;
7. update ACK matching tests.

#### Exit criteria

Không còn match ACK bằng `command == empty`.

---

### Phase B — Lifecycle correctness

Thay đổi:

1. thay API forget device;
2. truyền peer identity rõ ràng;
3. sửa delete order;
4. thêm lifecycle tests.

#### Exit criteria

Delete disconnected bonded device xóa bond được kiểm chứng bằng test/integration test.

---

### Phase C — Result contract

Thay đổi:

1. thêm status enum;
2. thêm payload format;
3. rename `message` thành `payload`;
4. update gateway command handlers;
5. update MCP/Web adapters.

#### Exit criteria

Transport layer không cần đoán text/JSON.

---

### Phase D — Registry hardening

Thay đổi:

1. init single-shot;
2. registry freeze;
3. copy-out command names;
4. update tests.

#### Exit criteria

Registry lifetime rõ ràng và deterministic.

---

### Phase E — Concurrency benchmark

Benchmark:

```text
1 device
5 devices
10 devices
```

Workload:

```text
gateway command
device command
mixed MCP/Web requests
telemetry notifications
```

Metrics:

```text
dispatcher latency
ACK latency
HTTP latency
BLE timeout rate
queue/blocking behavior
heap usage
stack high-water mark
```

Sau benchmark mới quyết định có cần dispatcher worker queue hay không.

---

## 19. Definition of Done

Refactor được xem là hoàn tất khi:

- [ ] ACK chỉ complete request bằng request ID hợp lệ.
- [ ] Device event không thể complete pending command.
- [ ] Stale ACK không thể complete request mới.
- [ ] Delete disconnected bonded device không để orphan bond.
- [ ] Result có status code và format rõ.
- [ ] Registry không expose pointer có lifetime mơ hồ.
- [ ] Init semantics được xác định rõ.
- [ ] `add_device` phân biệt persistence và connection side effect.
- [ ] Existing gateway commands vẫn hoạt động.
- [ ] Tất cả unit test cũ pass.
- [ ] Test mới cho ACK/lifecycle pass.
- [ ] Không giữ mutex trong thời gian wait ACK.
- [ ] Không tăng dynamic allocation trong command hot path.
- [ ] CMake dependency vẫn giữ component boundary hợp lý.
- [ ] README component được cập nhật theo implementation mới.

---

## 20. Thứ tự triển khai đề xuất

```text
1. Viết test tái hiện false ACK
2. Thêm request_id vào protocol
3. Sửa ACK matching
4. Viết test stale ACK
5. Sửa delete/bond lifecycle
6. Viết lifecycle tests
7. Chuẩn hóa dispatch_result_t
8. Harden registry
9. Update Web/MCP adapters
10. Update README
11. Benchmark concurrency
12. Quyết định worker queue
```

Không nên bắt đầu bằng việc thêm worker queue.

Correctness của protocol phải được ổn định trước.

---

## 21. Những phần không refactor trong đợt này

Không nằm trong scope:

- full MCP protocol implementation;
- TLS/authentication;
- persistent message queue;
- MQTT;
- multi-gateway routing;
- multiple concurrent commands trên cùng một peripheral;
- dynamic command unregister;
- plugin runtime;
- schema framework phức tạp;
- protobuf migration;
- BLE dual-role Phase 2.

Các phần này chỉ nên được xem xét sau khi Phase 1 ổn định.

---

## 22. Rủi ro của refactor

### Protocol compatibility

Thêm `request_id` yêu cầu update cả Gateway và firmware peripheral.

Mitigation:

```text
bump protocol version
```

Đề xuất:

```c
GW_PROTOCOL_VERSION 2
```

Gateway nên reject protocol version không được hỗ trợ thay vì silently decode.

---

### Increased message size

`request_id` chỉ tăng vài byte CBOR.

Ảnh hưởng không đáng kể so với `GW_MSG_MAX_LEN = 256`.

---

### Result API migration

Đổi `message` thành `payload` sẽ tác động Web/MCP layer.

Nên refactor theo một commit/change-set riêng sau protocol safety.

---

### Testability của BLE layer

Nếu `ble_central` đang khó mock, nên thêm interface nhỏ hoặc weak/mock implementation cho test component.

Không nên đưa mocking framework lớn chỉ vì dispatcher.

---

## 23. Đánh giá sau refactor mục tiêu

Nếu hoàn thành đầy đủ các thay đổi P0 và P1:

| Hạng mục | Hiện tại | Sau refactor mục tiêu |
|---|---:|---:|
| Module separation | 8/10 | 9/10 |
| Protocol correctness | 5/10 | 9/10 |
| Concurrency safety | 6/10 | 8/10 |
| API clarity | 6/10 | 9/10 |
| Testability | 6/10 | 9/10 |
| Maintainability | 8/10 | 9/10 |
| Phase 1 readiness | 7/10 | 9/10 |

Điểm tổng thể kỳ vọng:

```text
Hiện tại: 7/10
Sau refactor P0 + P1: khoảng 9/10
```

Điểm 10/10 không phải mục tiêu.

Với ESP32 Gateway Phase 1, thiết kế 9/10 nhưng đơn giản, deterministic và dễ debug tốt hơn một kiến trúc phức tạp cố đạt tối đa abstraction.

---

## 24. Kết luận

`command_dispatcher` hiện có nền kiến trúc tốt:

- dispatcher core nhỏ;
- gateway/device command tách rõ;
- registry pattern phù hợp;
- FreeRTOS synchronization đã được sử dụng đúng hướng;
- component boundary tương đối sạch.

Điểm cần sửa không nằm ở cấu trúc tổng thể mà chủ yếu nằm ở **protocol correlation, lifecycle ownership và API contract**.

Ưu tiên cao nhất:

```text
request_id + device_ack
        |
        v
delete/bond lifecycle
        |
        v
result contract
        |
        v
registry hardening
        |
        v
concurrency benchmark
```

Không nên thay đổi execution model sang async queue trước khi các lỗi correctness được xử lý và test đầy đủ.
