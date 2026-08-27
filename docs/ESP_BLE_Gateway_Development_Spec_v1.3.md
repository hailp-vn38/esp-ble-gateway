# ESP-BLE Gateway Development Specification

**Version:** 1.3.1  
**Date:** 27/08/2026  
**Status:** IMPLEMENTATION-FROZEN (supplemented)  
**Repository:** `hailp-vn38/esp-ble-gateway`  
**Verified baseline:** `a26f45d1985f0af7b61848e1ac1ca89fac463f1b`  
**Companion specification:** `ESP_BLE_Device_Development_Spec_v1.3.md`  
**Supersedes for Gateway scope:** `ESP_GATT_Capability_Cache_Message_Trace_Interop_Development_Spec_v1.2.md`

---

# 1. Purpose

Tài liệu này là specification triển khai chính thức cho repository `esp-ble-gateway` trong giai đoạn hoàn thiện ESP-GATT Protocol v3.

Mục tiêu của Gateway là:

1. quản lý capability như **persistent metadata**, độc lập với BLE session;
2. chỉ auto-discovery capability khi chưa có cache hợp lệ;
3. cung cấp manual refresh có generation/result rõ ràng và không phá cache cũ;
4. đảm bảo stale event/completion không thể mutate operation mới;
5. quản lý NVS theo nguyên tắc chỉ ghi khi persistent state thực sự cần thay đổi hoặc cần retry synchronization;
6. propagate đầy đủ response payload từ `device_ack` lên Web/MCP;
7. có full TX/RX message trace nhưng không làm nặng NimBLE host callback;
8. giữ interoperability tuyệt đối với companion Device spec.

Tài liệu được viết để developer hoặc AI coding agent có thể triển khai trực tiếp mà không phải tự quyết lại semantics.

---

# 2. Scope

## 2.1 In scope

- `components/device_capabilities`
- `components/device_store`
- `components/ble_central`
- `components/cbor_codec`
- `components/command_dispatcher`
- `components/command_executor`
- `components/message_trace` **(planned — chưa tồn tại trong repo)**
- `components/log_buffer`
- `components/web_server`
- dashboard/UI capability refresh
- `components/mcp_endpoint`
- host/unit/integration tests
- hardware verification với reference device

## 2.2 Out of scope

- Protocol v4
- periodic capability polling
- capability TTL
- auto refresh mỗi reconnect
- automatic capability revision beacon trong advertising
- dynamic MCP tool generation
- BLE application fragmentation
- cloud logging
- PCAP capture
- enum/string/float capability value type

---

# 3. Current Gateway baseline

Gateway hiện có các component chính:

```text
components/
├── ble_central/
├── cbor_codec/
├── command_dispatcher/
├── command_executor/
├── device_capabilities/
├── device_store/
├── log_buffer/
├── mcp_endpoint/
└── web_server/
```

Các behavior hiện tại cần refactor:

- capability persisted load có thể được coi là `STALE`;
- BLE READY có thể trigger discovery theo session;
- disconnect có thể làm capability state bị demote;
- manual refresh chưa có operation generation rõ ràng;
- discovery serializer dùng boolean ownership chưa đủ mạnh;
- `device_ack.int_value` chưa được propagate đầy đủ tới caller;
- refresh REST/UI chưa correlate chính xác một refresh transaction;
- raw BLE message trace chưa có correlation hoàn chỉnh;
- delete/rebind capability cleanup chưa failure-safe đối với NVS failure.

---

# 4. Shared ESP-GATT v3 contract

Gateway implementation không được đổi các contract sau nếu không có migration task riêng.

## 4.1 BLE UUID contract

```text
Service UUID: 0xABF0
Command UUID: 0xABF1  Gateway -> Device  WRITE_NO_RSP
Status UUID:  0xABF2  Device  -> Gateway NOTIFY
CCCD UUID:    0x2902
```

## 4.2 Protocol version

```text
ESP-GATT Protocol version = 3
GW_MSG_MAX_LEN            = 256
```

Gateway decoder có thể giữ backward compatibility hiện có, nhưng message mới do Gateway tạo phải explicit protocol version.

## 4.3 Capability wire order

Một request `describe_capabilities` hợp lệ phải nhận:

```text
capabilities_begin
capability_item #0
capability_item #1
...
capability_item #N-1
capabilities_end
device_ack
```

`capabilities_end` không thay thế final ACK.

## 4.4 Direct-response `device_id`

`device_id` trên wire là **Gateway routing identity**.

Ví dụ Gateway quản lý device bằng ID:

```text
lamp-1
```

thì request:

```text
device_id = lamp-1
```

và mọi direct response bound với request đó phải echo chính xác:

```text
capabilities_begin.device_id = lamp-1
capability_item.device_id     = lamp-1
capabilities_end.device_id    = lamp-1
device_ack.device_id          = lamp-1
```

Device model như:

```text
esp32s3-ref
```

không phải routing identity và không được dùng để override direct response.

## 4.5 Spontaneous event identity

Đối với `device_event` không có request trước đó, Gateway phải coi BLE connection/runtime context là source of truth cho routing.

Embedded `device_id` trong spontaneous event không được dùng để thay thế logical ID đã bind trên Gateway.

## 4.6 ACK contract

ACK hợp lệ phải có:

```text
type       = device_ack
request_id = exact request_id
command    = exact command
device_id  = exact request.device_id
bool_value = success/failure
int_value  = result/state when applicable
```

Gateway matcher tiếp tục yêu cầu exact correlation.

---

# 5. Mandatory Gateway decisions

## G1. Capability là persistent metadata

Committed capability snapshot hợp lệ vẫn usable qua:

- BLE disconnect/reconnect;
- Gateway reboot;
- Device reboot;
- RF loss;
- reconnect supervisor retry.

BLE link state không được tự động invalidate capability metadata.

## G2. Initial discovery chỉ khi chưa có cache

Khi device chuyển BLE READY:

```text
committed cache exists?
   |
   +-- yes -> no discovery
   |
   +-- no
        |
        +-- UNKNOWN -> start initial discovery
        +-- ERROR -> no automatic retry
        +-- UNSUPPORTED -> no automatic retry
```

## G3. Timeout không có nghĩa là unsupported

Chỉ explicit device rejection/unsupported response mới được map sang `UNSUPPORTED`.

Các failure sau map sang `ERROR` khi không có cache:

```text
TIMEOUT
BUSY
DISCONNECTED
TRANSPORT_ERROR
PROTOCOL_ERROR
INTERNAL_ERROR
```

Manual refresh trên cache có sẵn chỉ update refresh result; committed cache vẫn READY.

## G4. Cache state độc lập refresh state

Đúng:

```text
cache_state          = READY
refresh_active.state = RUNNING
```

Sai:

```text
cache_state = DISCOVERING
```

chỉ vì manual refresh đang chạy.

## G5. Manual refresh generation reserve đồng bộ

HTTP `202` chỉ được trả sau khi:

```text
validate
reserve generation
reserve operation token
mark QUEUED
enqueue event successfully
```

đã hoàn tất dưới synchronization phù hợp.

## G6. Operation ownership bắt buộc có token

Mọi capability operation có internal `operation_id` khác 0.

Mọi event liên quan operation phải snapshot operation ID tại thời điểm enqueue:

```text
READY/refresh start
capability BEGIN/ITEM/END notify
submit completion
disconnect cancellation
```

Stale event không match active owner/token phải bị ignore.

## G7. Global serializer có explicit owner

Không dùng một boolean `s_discovery_active` đơn độc làm ownership contract.

Serializer phải biết:

```text
active?
device_id
operation_id
operation kind
refresh generation when manual
```

Chỉ owner chính xác mới được release serializer.

## G8. Queued operation disconnect phải cancel chính xác

Nếu Device B đang `QUEUED` vì Device A sở hữu serializer và B disconnect:

```text
cancel B queued operation
invalidate B operation_id
manual B -> last_completed=DISCONNECTED
initial B -> cache ERROR(reason=DISCONNECTED)
do NOT release serializer của A
```

Không được để B treo `QUEUED` vô hạn.

## G9. Refresh active state không có COMPLETE

Target state:

```c
typedef enum {
    DEVICE_CAP_REFRESH_IDLE = 0,
    DEVICE_CAP_REFRESH_QUEUED,
    DEVICE_CAP_REFRESH_RUNNING,
} device_cap_refresh_state_t;
```

Completion là transition atomic:

```text
active -> last_completed
active.generation = 0
active.state = IDLE
```

Không có trạng thái `COMPLETE` trong active slot.

## G10. BUSY policy được freeze

Với initial discovery:

```text
SUBMIT_BUSY -> cache ERROR(reason=BUSY)
              operation ends
              no automatic reconnect retry
              manual refresh can retry
```

Với manual refresh có cache:

```text
SUBMIT_BUSY -> keep cache READY
              last_completed.result = BUSY
```

Không có hidden periodic retry/backoff trong version này.

## G11. Refresh failure không xóa cache

Nếu đã có committed snapshot, mọi refresh failure chỉ discard staging và publish refresh result.

## G12. `persist_dirty` là mandatory

Nếu RAM commit thành công nhưng NVS persistence fail:

```text
RAM = new snapshot
persist_dirty = true
cache = READY
```

Refresh sau với cùng content:

```text
persist_dirty=true -> retry NVS
```

Unchanged refresh chỉ zero-write khi `persist_dirty=false`.

## G13. Physical peer replacement invalidates capability cache

Capability cache keyed theo logical `device_id` không đủ để chứng minh physical identity.

Khi binding BLE identity thay đổi, old capability cache phải bị invalidate **trước khi new physical peer được coi là usable**.

## G14. Forget/delete/rebind phải failure-safe

`device_capabilities_forget()` target semantics:

```text
1. cancel/invalidate any active or queued operation for device
2. erase persistent capability record + nvs_commit
3. only after successful persistent erase -> clear RAM record
4. return success
```

Nếu persistent erase fail:

```text
return error
RAM cache remains
caller must not complete delete/rebind
```

Recommended delete order:

```text
device_capabilities_forget(device_id)
    |
    +-- fail -> abort delete
    |
    +-- success
          -> ble_central_forget_peer(...)
          -> device_store_delete(...)
```

Nếu BLE/store deletion fail sau capability forget, device entry có thể còn tồn tại nhưng capability đã UNKNOWN. Điều này an toàn: lần READY sau sẽ initial-discover lại.

Recommended rebind order:

```text
snapshot old binding
-> capability forget
-> set/persist new BLE identity
-> reconnect new peer
-> initial discovery when READY
```

Nếu new binding persistence fail, old binding có thể còn nhưng capability đã bị quên; không được restore stale capability cũ một cách suy đoán.

## G15. Snapshot revision/content mismatch phải observable

Persistent equality gồm revision và ordered items.

Nếu:

```text
incoming.revision == committed.revision
BUT persistent content changed
```

Gateway vẫn accept valid new snapshot, nhưng phải log:

```text
[CAP_REVISION_MISMATCH]
device=lamp-1 revision=5 content_changed=true
```

và persist snapshot mới.

## G16. Capability item order là semantic presentation order

Trong version này, device registry order là presentation order.

Do đó equality so `items[i]` theo thứ tự.

Reorder capability được coi là content change và có thể yêu cầu NVS write.

## G17. BLE readiness ownership

Public low-level `device_capabilities_refresh()` không tự phụ thuộc `ble_central` để giữ component boundary.

Contract bắt buộc:

```text
ALL production callers MUST preflight BLE runtime status.ready == true
before device_capabilities_refresh().
```

Web, MCP future tool và internal caller phải có test cho invariant này.

## G18. `frame_id` allocation thread-safe

Frame ID là boot-local correlation identifier và phải unique giữa các frame trace đồng thời.

`message_trace_next_frame_id()` phải thread-safe bằng critical section/atomic/mutex phù hợp.

Không dùng unsynchronized `++global`.

Recommended implementation trên ESP32-S3:

```c
#include <stdatomic.h>

static atomic_uint s_next_frame_id = ATOMIC_VAR_INIT(0);

uint32_t message_trace_next_frame_id(void) {
    uint32_t id;
    do {
        id = atomic_fetch_add(&s_next_frame_id, 1) + 1;
    } while (id == 0); /* skip 0 */
    return id;
}
```

Alternative dùng `portENTER_CRITICAL` nếu atomic không khả dụng:

```c
static uint32_t s_next_frame_id = 0;
static portMUX_TYPE s_frame_mux = portMUX_INITIALIZER_UNLOCKED;

uint32_t message_trace_next_frame_id(void) {
    uint32_t id;
    taskENTER_CRITICAL(&s_frame_mux);
    s_next_frame_id++;
    if (s_next_frame_id == 0) s_next_frame_id = 1; /* skip 0 */
    id = s_next_frame_id;
    taskEXIT_CRITICAL(&s_frame_mux);
    return id;
}
```

Counter wraps bằng `UINT32_MAX -> 1` (skip 0). uniqueness trong cùng boot cycle đủ cho correlation purposes.

## G19. Message trace resource budget

`message_trace` component phải fit trong RAM budget của ESP32-S3 (512KB SRAM, ~300KB usable sau system allocation).

Estimated resource breakdown:

```text
Ring buffer (128 entries × ~80 bytes/entry)    ≈ 10 KB
Frame ID counter                               ≈ 4 bytes
Format scratch buffer (per-thread)             ≈ 256 bytes × N threads
Raw trace chunk buffers (if enabled)           ≈ configurable, max 1 KB
Mutex/spinlock overhead                        ≈ 32 bytes
────────────────────────────────────────────────────────────
Total baseline                                 ≈ 11-12 KB
```

Kích thước ring buffer và raw chunk policy phải configurable qua Kconfig:

```text
CONFIG_MESSAGE_TRACE_RING_SIZE     (default: 128)
CONFIG_MESSAGE_TRACE_RAW_ENABLE    (default: 0)
CONFIG_MESSAGE_TRACE_RAW_CHUNK_BYTES (default: 32)
```

Total message_trace footprint không được vượt 20KB baseline. Nếu vượt, phải giảm ring size hoặc disable raw trace.

Worker thread stack allocation cho trace formatting không được vượt 2KB per thread.

---

# 6. Target capability state model

## 6.1 Cache state

Target states:

```c
typedef enum {
    DEVICE_CAP_STATE_UNKNOWN = 0,
    DEVICE_CAP_STATE_DISCOVERING,
    DEVICE_CAP_STATE_READY,
    DEVICE_CAP_STATE_UNSUPPORTED,
    DEVICE_CAP_STATE_ERROR,
} device_cap_state_t;
```

Nếu cần giữ enum `STALE` để source compatibility, nó phải được đánh dấu deprecated và target implementation không set state này vì disconnect/reboot.

### UNKNOWN

Không có committed snapshot và chưa có definitive attempt result.

### DISCOVERING

Chỉ dùng cho **initial discovery khi chưa có cache**.

### READY

Có committed snapshot usable.

### UNSUPPORTED

Không có snapshot và device explicit reject `describe_capabilities`.

### ERROR

Không có snapshot và discovery fail vì timeout/BUSY/disconnect/transport/protocol/internal.

Nên giữ machine-readable `last_error_reason` riêng nếu tiện triển khai.

## 6.2 Refresh result

```c
typedef enum {
    DEVICE_CAP_REFRESH_RESULT_NONE = 0,
    DEVICE_CAP_REFRESH_RESULT_SUCCESS,
    DEVICE_CAP_REFRESH_RESULT_UNCHANGED,
    DEVICE_CAP_REFRESH_RESULT_NOT_PERSISTED,
    DEVICE_CAP_REFRESH_RESULT_UNSUPPORTED,
    DEVICE_CAP_REFRESH_RESULT_BUSY,
    DEVICE_CAP_REFRESH_RESULT_TIMEOUT,
    DEVICE_CAP_REFRESH_RESULT_DISCONNECTED,
    DEVICE_CAP_REFRESH_RESULT_TRANSPORT_ERROR,
    DEVICE_CAP_REFRESH_RESULT_PROTOCOL_ERROR,
    DEVICE_CAP_REFRESH_RESULT_INTERNAL_ERROR,
} device_cap_refresh_result_t;
```

## 6.3 Active + last-completed refresh

```c
typedef struct {
    uint32_t generation; /* 0 means no active manual refresh */
    device_cap_refresh_state_t state;
} device_cap_refresh_active_t;

typedef struct {
    uint32_t generation; /* 0 means no completed refresh yet */
    device_cap_refresh_result_t result;
    int64_t finished_at_ms;
} device_cap_refresh_completed_t;
```

Client A vẫn đọc được result generation 8 khi generation 9 đã bắt đầu:

```text
active.generation         = 9
active.state              = RUNNING
last_completed.generation = 8
last_completed.result     = UNCHANGED
```

## 6.4 Internal operation model

```c
typedef enum {
    DEVICE_CAP_OP_NONE = 0,
    DEVICE_CAP_OP_INITIAL,
    DEVICE_CAP_OP_MANUAL,
} device_cap_operation_kind_t;

typedef enum {
    DEVICE_CAP_OP_IDLE = 0,
    DEVICE_CAP_OP_QUEUED,
    DEVICE_CAP_OP_RUNNING,
} device_cap_operation_state_t;
```

Per-device record cần tối thiểu:

```c
typedef struct {
    bool used;
    bool has_committed;
    bool persist_dirty;

    device_capability_snapshot_t committed;

    bool staging_active;
    uint32_t staging_operation_id;
    uint16_t staging_expected;
    device_capability_snapshot_t staging;

    device_cap_operation_kind_t operation_kind;
    device_cap_operation_state_t operation_state;
    uint32_t operation_id;

    device_cap_refresh_active_t refresh_active;
    device_cap_refresh_completed_t refresh_last_completed;

    /* optional diagnostic reason fields */
} capability_record_t;
```

Struct alignment requirement:

- Tất cả `uint32_t` fields phải naturally aligned (offset chia hết cho 4).
- Nếu compiler thêm padding giữa các fields, kiểm tra bằng `static_assert(sizeof(capability_record_t) % 4 == 0, "alignment")`.
- Trên ESP32-S3 (32-bit), `bool` là 1 byte. Nếu cần compact, group các bools lại và đặt ở đầu struct.
- `device_capability_snapshot_t` phải có alignment >= 4 bytes.
- Không dùng `__attribute__((packed))` trừ khi cần wire-format serialization trực tiếp; RAM layout ưu tiên speed over space.
- Tổng sizeof mỗi record không được vượt 512 bytes (budget: 12 devices × 512 = 6KB).

Global owner:

```c
typedef struct {
    bool active;
    char device_id[GW_MSG_DEVICE_ID_LEN];
    uint32_t operation_id;
    device_cap_operation_kind_t kind;
    uint32_t refresh_generation;
} capability_global_owner_t;
```

Counters:

```text
next_operation_id  -> skip 0
next_generation    -> per-device or globally monotonic, skip 0
```

Generation chỉ cần unique cho từng device/client correlation; operation ID phải đủ để reject stale runtime events.

---

# 7. Refresh API contract

Target public API:

```c
esp_err_t device_capabilities_refresh(
    const char *device_id,
    uint32_t *out_generation);
```

Production caller phải preflight BLE READY.

API phải làm atomically ở mức record + queue reservation:

```text
validate initialized
validate device exists
lock
  find/create record
  reject if any capability operation for same device QUEUED/RUNNING
  reserve generation != 0
  reserve operation_id != 0
  operation_kind  = MANUAL
  operation_state = QUEUED
  refresh_active.generation = generation
  refresh_active.state      = QUEUED
  enqueue CAP_EVENT_REFRESH carrying generation + operation_id
  if enqueue fails:
      rollback reservation
unlock
return generation
```

Không được tăng generation lần đầu trong worker.

Error mapping recommendation:

```text
invalid arg        -> ESP_ERR_INVALID_ARG
unknown device     -> ESP_ERR_NOT_FOUND
same-device op     -> ESP_ERR_INVALID_STATE / explicit BUSY result
queue full         -> ESP_ERR_NO_MEM
not initialized    -> ESP_ERR_INVALID_STATE
```

REST layer map error thành HTTP status phù hợp.

---

# 8. Initial discovery lifecycle

## 8.1 Boot

```text
device_store_init()
-> device_capabilities_init()
-> load persisted snapshots
```

Persisted valid snapshot:

```text
has_committed = true
cache_state   = READY
persist_dirty = false
updated_at_ms = 0
```

`updated_at_ms=0` nghĩa là snapshot chưa được refreshed trong current boot.

## 8.2 BLE READY

```text
on_ready(device_id)
  |
  +-- committed READY -> no discovery
  |
  +-- operation QUEUED/RUNNING -> no duplicate
  |
  +-- UNKNOWN -> reserve INITIAL operation and queue
  |
  +-- ERROR/UNSUPPORTED -> no auto retry
```

Duplicate READY callbacks phải idempotent.

## 8.3 Initial BUSY

Nếu command request slot đang bận:

```text
INITIAL submit -> BUSY
-> end operation
-> cache ERROR(reason=BUSY)
-> no hidden retry
```

User có thể manual refresh sau khi device READY.

---

# 9. Global serializer and event ordering

Gateway version này giữ global capability serializer để giảm complexity.

## 9.1 Start next pending

Worker chỉ start pending operation nếu:

```text
global_owner.active == false
```

Khi start:

```text
global_owner.active = true
global_owner.device_id = target
global_owner.operation_id = record.operation_id
record.operation_state = RUNNING
manual -> refresh_active.state = RUNNING
```

## 9.2 Event envelope

Capability worker event cần chứa:

```c
typedef struct {
    capability_event_type_t type;
    char device_id[GW_MSG_DEVICE_ID_LEN];
    uint32_t operation_id;
    uint32_t refresh_generation;
    gw_message_t message;
    device_cap_submit_result_t completion;
} capability_event_t;
```

Event message ownership:

- `gw_message_t` trong event phải là **deep copy**, không phải pointer/reference.
- Producer (command executor callback hoặc notify handler) phải copy message data vào event trước khi enqueue.
- Consumer (capability worker) owns event sau khi dequeue và phải free/release sau khi xử lý.
- Không dùng同一 `gw_message_t` instance cho multiple events.
- Nếu `gw_message_t` chứa embedded buffer (không phải pointer), copy bằng assignment.
- Nếu `gw_message_t` chứa pointer, producer phải allocate copy và consumer phải free.

Notify event phải capture current operation token lúc enqueue.

Không lookup token mới trong worker rồi giả định event thuộc token đó.

## 9.3 BEGIN -> ITEM -> END -> completion invariant

Tất cả capability protocol events và final command completion phải serialize qua cùng capability worker.

Không mutate final capability result trực tiếp từ command-executor callback.

Required order:

```text
CAP_EVENT_NOTIFY BEGIN
CAP_EVENT_NOTIFY ITEM*
CAP_EVENT_NOTIFY END
CAP_EVENT_COMPLETION
```

Device gửi END trước ACK; Gateway phải bảo toàn ordering ở processing boundary.

## 9.4 Stale completion

Completion chỉ hợp lệ nếu:

```text
record.operation_id == event.operation_id
AND global_owner matches device_id + operation_id
```

Nếu không:

```text
log stale completion
ignore
DO NOT release current owner
```

## 9.5 Stale notify

BEGIN/ITEM/END chỉ được apply khi:

```text
record operation matches event.operation_id
operation state RUNNING
```

Stale notify đã nằm trong queue từ operation cũ phải bị ignore.

---

# 10. Disconnect semantics

Disconnect không invalidate committed snapshot.

## 10.1 No capability operation

```text
READY cache remains READY
UNKNOWN remains UNKNOWN
ERROR remains ERROR
UNSUPPORTED remains UNSUPPORTED
```

## 10.2 Running owned operation

Nếu disconnect đúng device/operation đang sở hữu serializer:

```text
invalidate operation token
clear staging
manual -> publish DISCONNECTED to last_completed
initial without cache -> ERROR(reason=DISCONNECTED)
release owner only when owner token matches
start next pending device
```

## 10.3 Queued operation

Nếu target disconnect khi operation còn QUEUED:

```text
invalidate queued token
remove/logically cancel queued work
operation_state = IDLE
manual:
    active -> last_completed(DISCONNECTED)
initial:
    cache ERROR(reason=DISCONNECTED)
```

Không release owner của device khác.

Worker khi gặp queued event đã cancel phải check token/state và ignore.

## 10.4 Disconnect during NVS persistence

Nếu disconnect xảy ra trong lúc NVS write đang chạy (step 7-9 của §13.1):

```text
NVS write continues to completion (non-cancellable)
record lock is NOT held during NVS write
  -> disconnect handler can safely read record state

on NVS success:
    persist_dirty=false
    cache state unchanged (READY or ERROR depending on prior state)

on NVS failure:
    persist_dirty=true
    next refresh will retry persistence

disconnect handler does NOT:
    - cancel in-progress NVS write
    - revert RAM committed snapshot
    - modify persist_dirty flag
```

Device vẫn còn trong registry; BLE identity vẫn valid;下次 READY chỉ cần check cache state, không cần rediscovery.

## 10.5 Disconnect during capability forget

Nếu disconnect xảy ra trong lúc `device_capabilities_forget()` đang chạy:

```text
NVS erase is atomic at key level
  -> erase completes or fails atomically

if erase was already committed:
    RAM clear proceeds, device capability removed

if erase not yet started:
    forget handler aborts cleanly

if erase in progress:
    forget handler waits for NVS completion
    then clears RAM if erase succeeded
    or returns error if erase failed
```

---

# 11. Capability protocol validation

Giữ các invariant:

- protocol v3 cho capability messages;
- exact routing `device_id` match connection/request context;
- max 12 capabilities;
- begin có `snapshot_id`, `total`, `capability_revision`;
- item snapshot ID match staging;
- sequence liên tục từ 0;
- no duplicate command;
- command charset hợp lệ;
- valid value type;
- INT có `min`, `max`, `step`;
- `min <= max`;
- `step != 0`;
- end snapshot ID match;
- end total match expected;
- received item count match expected;
- complete END mới được commit.

Invalid frame thuộc active capability operation phải kết thúc operation bằng `PROTOCOL_ERROR`, không silently chờ timeout nếu lỗi đã definitive.

---

# 12. Snapshot equality and revision semantics

Không dùng raw `memcmp()` trên struct.

Persistent equality so:

```text
schema_version
count
revision
items[i].command
items[i].label
items[i].unit
items[i].value_type
items[i].flags
items[i].min_value
items[i].max_value
items[i].step
```

Không dùng:

```text
snapshot_id
updated_at_ms
runtime operation fields
```

Item order là semantic presentation order.

Schema migration:

```text
schema_version mismatch:
  - minor version bump: accept, log warning, persist with new schema
  - major version bump: reject snapshot, treat as UNKNOWN, rediscover

schema_version comparison:
  - loaded.schema_version < current_schema_version -> migration needed
  - loaded.schema_version > current_schema_version -> reject (newer firmware)
  - loaded.schema_version == current_schema_version -> normal equality check

Migration strategy:
  - minor: field addition with safe default, no data loss
  - major: structural change, full rediscovery required
```

## 12.1 Same revision but changed content

```text
same revision + changed persistent content
-> accept valid staging
-> log CAP_REVISION_MISMATCH
-> commit RAM
-> persist
```

Không reject chỉ vì device firmware quên bump revision.

## 12.2 Revision changed but other content same

Revision là persistent content, do đó:

```text
revision changed -> persistent snapshot changed -> NVS write required
```

---

# 13. Commit and persistence

## 13.1 Commit sequence

```text
1. verify event token owns active operation
2. validate complete staging
3. compare persistent content
4. if changed:
      replace committed RAM atomically under lock
      cache READY
      persist_dirty=true
5. else:
      keep committed content
6. clear staging
7. if changed OR persist_dirty:
      persist committed snapshot outside long-held record lock
8. on NVS success:
      persist_dirty=false
9. on NVS failure:
      persist_dirty=true
10. finalize operation result
```

RAM commit không rollback nếu NVS fail.

Locking strategy:

```text
Per-record lock (s_record_mux) protects:
  - committed/staging fields
  - operation state
  - refresh state
  - persist_dirty flag

持lock scope ngắn nhất có thể:
  Step 1-6: acquire record lock -> read/validate/compare -> release lock
  Step 7:   NVS write OUTSIDE record lock (NVS có internal locking)
  Step 8-9: acquire record lock -> update persist_dirty -> release lock

NVS写入期间, record lock KHÔNG được giữ:
  - NVS write có thể slow (10-50ms cho single key write)
  - Giữ lock sẽ block所有 BLE notify processing cho device khác
  - Nếu disconnect xảy ra trong lúc NVS write, next refresh sẽ retry (persist_dirty=true)

Tránh lock inversion:
  - Capability record lock -> NEVER acquire NVS lock trong khi hold
  - NVS lock internally managed by esp_partition API
  - Nếu cần atomic cross-record operation, acquire locks theo device_id order (lexicographic)
```

## 13.2 Manual result mapping

Changed + NVS success:

```text
SUCCESS
```

Unchanged + clean:

```text
UNCHANGED
NVS writes = 0
```

Changed + NVS fail:

```text
NOT_PERSISTED
persist_dirty=true
```

Unchanged + dirty + retry success:

```text
UNCHANGED
persist_dirty=false
```

Unchanged + dirty + retry fail:

```text
NOT_PERSISTED
persist_dirty=true
```

## 13.3 Initial result mapping

Initial valid snapshot + RAM commit + NVS success:

```text
READY
```

Initial valid snapshot + RAM commit + NVS fail:

```text
READY
persist_dirty=true
```

There is no manual generation to publish.

---

# 14. NVS schema and timestamp semantics

Persist only persistent capability content.

Recommended persisted record:

```c
#define CAPABILITY_PERSISTED_SCHEMA_VERSION 1

typedef struct {
    uint8_t schema_version;     /* must == CAPABILITY_PERSISTED_SCHEMA_VERSION */
    uint8_t count;
    uint16_t reserved;          /* pad to 4-byte alignment, must be 0 */
    char device_id[GW_MSG_DEVICE_ID_LEN];
    uint32_t revision;
    device_capability_t items[DEVICE_CAP_MAX_PER_DEVICE];
} persisted_snapshot_t;
```

Schema version rules:

```text
CAPABILITY_PERSISTED_SCHEMA_VERSION = current implementation version
On load:
  if schema_version == CAPABILITY_PERSISTED_SCHEMA_VERSION -> normal equality check
  if schema_version < CAPABILITY_PERSISTED_SCHEMA_VERSION  -> migration if minor, reject if major
  if schema_version > CAPABILITY_PERSISTED_SCHEMA_VERSION  -> reject (newer firmware wrote this)
  if schema_version == 0                                   -> reject (corrupted/empty)
```

Version numbering convention:

```text
schema_version = MAJOR * 16 + MINOR (nibble format)
MAJOR bump: structural change, full rediscovery required
MINOR bump: field addition with safe default, no data loss
```

`snapshot_id` is runtime transaction metadata and should not be required in new persisted schema.

If schema migration keeps old field for compatibility, it must not participate in identity/equality semantics.

Do not persist:

```text
updated_at_ms
refresh generation
operation_id
staging
active state
last completion
```

`updated_at_ms` is monotonic current-boot uptime only:

```text
0 = unknown/not refreshed in this boot
```

Không render nó như Unix timestamp.

---

# 15. Failure-safe capability forget

Target API:

```c
esp_err_t device_capabilities_forget(const char *device_id);
```

Required semantics:

```text
validate device_id
lock
  snapshot record/key/index
  invalidate queued/running operation token
  logically cancel staging
unlock

persist erase + nvs_commit
  |
  +-- fail:
        keep/reconstruct RAM committed record as usable
        return error
        caller must abort delete/rebind
  |
  +-- success:
        lock
          clear RAM record
        unlock
        return ESP_OK
```

Implementation must avoid the old unsafe order:

```text
clear RAM first
then erase NVS
```

because NVS failure can leave stale snapshot that returns after reboot.

If concurrency makes full rollback complicated, use a temporary `FORGETTING` internal flag and block reads/operations until persistence outcome is known; public cache state must remain semantically safe.

---

# 16. Delete and rebind integration

## 16.1 Delete device

Target dispatcher sequence:

```text
1. device_store_get() snapshot identity
2. device_capabilities_forget()
   - failure -> abort and return error
3. ble_central_forget_peer()
   - failure -> device entry retained, capability now UNKNOWN
4. device_store_delete()
5. success
```

If step 3/4 fails after capability forget, this is acceptable safe degradation. Device remains configured without capability cache and will rediscover next READY.

## 16.2 Physical BLE identity change

Any future path that changes `ble_addr` or `ble_addr_type` for existing logical ID must:

```text
forget capability successfully first
then change binding
```

No path may bind new physical peer while keeping old committed capability snapshot.

---

# 17. REST API contract

## 17.1 GET `/api/capabilities`

GET is cache-only.

It must never trigger BLE traffic.

Recommended response:

```json
{
  "device_id": "lamp-1",
  "cache": {
    "state": "ready",
    "revision": 7,
    "persist_dirty": false,
    "updated_at_ms": 123456,
    "commands": []
  },
  "refresh": {
    "active": {
      "generation": 0,
      "state": "idle"
    },
    "last_completed": {
      "generation": 8,
      "result": "unchanged",
      "finished_at_ms": 122000
    }
  }
}
```

`commands` vẫn usable khi refresh active.

## 17.2 POST `/api/capabilities/refresh`

Preflight:

```text
device exists?
BLE runtime status.ready == true?
no same-device capability operation queued/running?
```

Then:

```text
call device_capabilities_refresh(device_id, &generation)
```

Successful response:

```http
202 Accepted
```

```json
{
  "accepted": true,
  "device_id": "lamp-1",
  "generation": 9
}
```

Do not return 202 if enqueue failed.

Rate limiting:

```text
Minimum interval between refresh POST for same device: 2 seconds
If client sends refresh within interval:
  -> 429 Too Many Requests
  -> Response body: {"error": "rate_limited", "retry_after_ms": 2000}

Per-device rate limit state tracked separately:
  - last_refresh_request_ms per device
  - checked BEFORE generation reservation
  - NOT checked if queue is full (different error path)

Global rate limit (optional, configurable):
  CONFIG_REFRESH_RATE_LIMIT_GLOBAL_MAX=4 (max concurrent refresh operations)
  If global limit exceeded: 503 Service Unavailable

Rate limit does NOT apply to:
  - initial discovery (automatic, not user-triggered)
  - GET /api/capabilities (cache-only, no BLE traffic)
```

Recommended error mapping:

```text
not found      -> 404
not BLE ready  -> 502
same-device op -> 409
queue/resource -> 503/507 per existing API policy
internal       -> 500
```

## 17.3 Generation polling rule

Client with generation G:

```text
if last_completed.generation == G
    -> operation complete
else if active.generation == G
    -> queued/running
else if both generations > G
    -> result no longer retained; client should refresh view
else
    -> pending/unknown according to API contract
```

Version này giữ active + one last-completed slot, không giữ unlimited history.

---

# 18. Dashboard/UI behavior

Refresh button:

```text
POST refresh
-> receive generation
-> poll GET capabilities
-> match exact generation
```

Không dùng fixed 2.5-second delay.

Không mark server operation failed chỉ vì UI đã poll 8 giây.

Nếu operation vẫn QUEUED/RUNNING lâu:

```text
show "still pending" / spinner
allow user leave view
server remains source of truth
```

While refresh runs:

- cached controls remain visible;
- cached commands remain usable subject to ordinary per-device command serialization;
- refresh failure shows error but keeps old controls;
- success reloads command metadata.

---

# 19. MCP behavior

## 19.1 `list_device_capabilities`

Cache-only, same semantics as REST GET.

## 19.2 `device_command`

Static allowlist remains an independent security boundary.

Capability advertisement does not auto-authorize MCP command.

Gateway configuration must explicitly allow desired commands, for example:

```text
CONFIG_MCP_DEVICE_COMMAND_ALLOWLIST="set_led,get_state"
```

## 19.3 Future refresh MCP method

Nếu thêm MCP refresh trong version này hoặc sau đó, caller phải preflight `ble_central_get_device_status(...).ready` trước khi gọi low-level refresh API.

---

# 20. Ordinary device command response propagation

Current gap phải được sửa: successful ACK không được chỉ collapse thành text "acknowledged".

Target successful JSON result:

```json
{
  "device_id": "lamp-1",
  "command": "get_state",
  "request_id": 123,
  "success": true,
  "response": {
    "has_int_value": true,
    "int_value": 1,
    "has_bool_value": true,
    "bool_value": true
  }
}
```

Exact shape có thể align với existing dispatcher result conventions, nhưng machine-readable response fields phải tồn tại.

For control command `set_led`, ACK result có thể phản ánh resulting state.

For query `get_state`, `int_value` phải reach Web/MCP caller.

Negative ACK:

```text
bool_value=false -> DISPATCH_STATUS_DEVICE_ERROR
```

while preserving machine-readable ACK fields when useful.

---

# 21. Message trace architecture

Trace must correlate:

```text
request_id   -> logical command transaction
frame_id     -> one encoded/received BLE frame within current boot
snapshot_id  -> capability response transaction emitted by device
operation_id -> internal Gateway capability ownership guard
```

`operation_id` is not a wire field.

## 21.1 Target API

Recommended component:

```text
components/message_trace/
├── CMakeLists.txt
├── include/message_trace.h
├── message_trace.c
└── test/
```

Core APIs:

```c
uint32_t message_trace_next_frame_id(void);

void message_trace_tx_decoded(uint32_t frame_id,
                              const char *device_id,
                              const gw_message_t *message,
                              size_t encoded_len);

void message_trace_tx_raw(uint32_t frame_id,
                          const uint8_t *data,
                          size_t len);

void message_trace_tx_result(uint32_t frame_id, int result);

void message_trace_rx_raw(uint32_t frame_id,
                          const char *device_id,
                          const uint8_t *data,
                          size_t len);

void message_trace_rx_decoded(uint32_t frame_id,
                              const char *device_id,
                              const gw_message_t *message);

void message_trace_rx_decode_error(uint32_t frame_id,
                                   const char *device_id,
                                   int decode_result);
```

Formatter should emit via `ESP_LOG*`; `log_buffer` captures logs through existing logging path rather than `message_trace` depending directly on log buffer.

## 21.2 Thread-safe frame ID

`message_trace_next_frame_id()` must be safe across:

- command executor worker;
- BLE central worker;
- notify RX worker;
- other trace-producing tasks.

Counter wraps by skipping 0.

Unit test should allocate concurrently and assert no duplicates in sample set.

---

# 22. TX trace placement

Mandatory order:

```text
1. encode
2. if encode fails -> log with request_id when known, no frame_id required
3. allocate frame_id THREAD-SAFELY
4. log decoded TX summary
5. optional raw TX chunks
6. validate MTU/payload size
7. if MTU reject -> log TX_RESULT using same frame_id
8. GATT WriteNoRsp
9. log TX_RESULT using same frame_id
```

Every successfully encoded frame gets a frame ID **before MTU validation**.

Example:

```text
[MSG_TX] frame=41 device=lamp-1 request=812 type=device_command command=get_state len=37
[MSG_TX_RESULT] frame=41 rc=MTU_TOO_SMALL
```

---

# 23. RX trace placement

NimBLE host callback must remain bounded:

```text
validate basic length
copy bytes to bounded queue object
allocate/capture minimal correlation if safe
queue
return
```

No hex formatting, cJSON, long logs or CBOR decode in host callback.

Recommended worker flow:

```text
RX queue
-> allocate frame_id if not already assigned
-> raw trace chunks
-> decode CBOR
-> decoded trace or decode-error trace
-> route capability / ACK / event
```

Raw RX cannot assume request ID before decode.

---

# 24. Raw trace format and resource policy

Default raw chunk size:

```text
32 bytes
```

so encoded hex + metadata fits existing log entry budget.

Example:

```text
[MSG_RX_RAW] frame=77 part=0/2 len=32 hex=A401...
[MSG_RX_RAW] frame=77 part=1/2 len=11 hex=...
```

Trace Kconfig should support:

```text
CONFIG_MESSAGE_TRACE_ENABLE
CONFIG_MESSAGE_TRACE_RAW_ENABLE
CONFIG_MESSAGE_TRACE_RAW_CHUNK_BYTES=32
```

Runtime logging must still respect ESP log levels.

Sensitive future fields must be redacted before raw logging is enabled in production.

---

# 25. Log buffer policy

Do not assume 256 entries are cheap.

RAM budget must account for:

- ring entries;
- Web snapshot/copy;
- formatting buffers;
- task stacks.

Recommended default entry count:

```text
128
```

unless measurement proves higher safe.

`/api/logs` first phase should preserve current response shape for backward compatibility.

Add `limit` parameter or bounded snapshot:

```text
GET /api/logs?limit=64
```

Expose dropped count through status/metadata without silently changing existing array response contract.

---

# 26. BLE scan name secondary fix

Current Device places service UUID in primary advertisement and name in scan response.

Gateway scan logic should not require service UUID and name to exist in the same advertisement packet.

Preferred fix:

```text
merge ADV + SCAN_RSP records by BLE address
```

This is UX improvement, not capability protocol blocker.

---

# 27. Gateway file-by-file implementation plan

## 27.1 `components/device_capabilities/include/device_capabilities.h`

Required:

- target cache state contract;
- refresh state without `COMPLETE`;
- refresh result enum;
- active + last-completed structs;
- expanded submit taxonomy;
- refresh API returning generation;
- copy-out status API;
- forget API semantics documented;
- test hooks guarded appropriately.

## 27.2 `components/device_capabilities/device_capabilities.c`

Refactor:

- persistent READY load;
- cache-first READY behavior;
- per-device operation token;
- global owner struct;
- synchronous refresh reservation;
- stale notify/completion guards;
- queued disconnect cancellation;
- initial BUSY -> ERROR;
- manual BUSY result;
- staging validation;
- revision mismatch warning;
- ordered content equality;
- `persist_dirty`;
- failure-safe persistent erase before RAM clear;
- correct delete/rebind support;
- deterministic worker test signaling.

## 27.3 `main/main.c`

Update capability submit bridge mapping:

```text
DISPATCH_STATUS_OK                -> SUBMIT_OK
DEVICE_ERROR/UNSUPPORTED_COMMAND  -> SUBMIT_REJECTED
BUSY                              -> SUBMIT_BUSY
TIMEOUT                           -> SUBMIT_TIMEOUT
NOT_CONNECTED                     -> SUBMIT_NOT_CONNECTED
TRANSPORT_ERROR                   -> SUBMIT_TRANSPORT_ERROR
other                             -> SUBMIT_INTERNAL_ERROR
```

Completion callback must enqueue capability completion event; it must not finalize capability state directly.

## 27.4 `components/command_dispatcher/device_command.c`

Required:

- preserve exact ACK message;
- return machine-readable ACK response fields;
- retain request_id/command/device_id correlation;
- keep capability validation before normal command send;
- ensure `get_state` result reaches caller.

## 27.5 `components/command_dispatcher/device_request_manager.*`

Keep one pending request per device for this phase.

Do not relax concurrency until separately designed.

Tests:

- exact request/command/device matching;
- mismatched ACK rejected;
- response payload retained.

## 27.6 `components/command_dispatcher/gateway_commands.c`

Required:

- cache-only capability list;
- delete flow reordered for failure-safe capability forget;
- future rebind flow must forget capability first;
- keep logical device identity distinct from BLE identity.

## 27.7 `components/ble_central/include/ble_central.h`

Use `ble_central_get_device_status()` as source of truth for runtime readiness.

No capability manager should infer readiness from persistent store.

## 27.8 `components/ble_central/ble_central.c` / GATT send path

Integrate TX trace:

```text
encode -> frame_id -> trace -> MTU -> write -> result trace
```

## 27.9 `components/ble_central/ble_central_notify.c`

Move/keep heavy raw RX formatting and decode in worker context.

Capture frame-level trace and queue-drop diagnostics.

## 27.10 `components/ble_central/ble_central_scan.c`

Optional Phase F UX fix: merge scan response name with primary service advertisement by address.

## 27.11 `components/message_trace/*`

New component with thread-safe frame ID and formatter tests.

## 27.12 `components/log_buffer/*`

- bounded RAM policy;
- dropped-count observability;
- test large trace volume;
- no change to core log capture semantics unless required.

## 27.13 `components/web_server/web_gateway_api.c`

- GET capabilities returns cache + refresh status;
- POST refresh preflights `status.ready`;
- returns generation only after successful reservation/enqueue;
- preserves machine-readable device command results.

## 27.14 Dashboard

- generation-aware polling;
- no fixed completion delay;
- preserve controls during refresh;
- clear failure/success status.

## 27.15 MCP

- keep static allowlist;
- preserve ACK output payload;
- any future refresh path must preflight BLE READY.

---

# 28. Required Gateway tests

## 28.1 Capability/cache

### GW-CAP-001 — unknown READY discovers once

```text
UNKNOWN + READY -> one initial operation
repeated READY while queued/running -> no duplicate
```

### GW-CAP-002 — persisted cache hit

```text
boot load valid cache -> READY
READY event -> no BLE capability request
```

### GW-CAP-003 — reconnect preserves cache

```text
READY cache -> disconnect -> READY cache
reconnect -> no discovery
```

### GW-CAP-004 — unsupported policy

Explicit rejection with no cache -> UNSUPPORTED; reconnect does not retry.

### GW-CAP-005 — timeout policy

Timeout with no cache -> ERROR, not UNSUPPORTED.

### GW-CAP-006 — initial BUSY policy

BUSY with no cache -> ERROR(reason=BUSY), no hidden retry.

### GW-CAP-007 — synchronous manual reservation

Generation and operation ID allocated before refresh API returns success.

### GW-CAP-008 — same-device concurrent op rejected

Initial queued/running blocks manual refresh; manual queued/running blocks another manual refresh.

### GW-CAP-009 — cross-device serializer

A RUNNING, B QUEUED; A completion starts B.

### GW-CAP-010 — queued target disconnect

A RUNNING, B QUEUED; B disconnect cancels B only and A owner remains active.

### GW-CAP-011 — running owner disconnect

Only exact device + operation token can release owner.

### GW-CAP-012 — stale completion

Old operation completion cannot mutate/release new operation.

### GW-CAP-013 — stale notify

Old queued BEGIN/ITEM/END cannot mutate new staging.

### GW-CAP-014 — END before completion

Completion is finalized only after prior END event processed.

### GW-CAP-015 — unchanged clean

No NVS write.

### GW-CAP-016 — changed snapshot

RAM commit + one persistence attempt.

### GW-CAP-017 — NVS failure

RAM remains new READY; `persist_dirty=true`.

### GW-CAP-018 — dirty unchanged retry

Same incoming snapshot retries NVS and clears dirty on success.

### GW-CAP-019 — partial/invalid snapshot

Never replaces committed cache.

### GW-CAP-020 — revision mismatch

Same revision + changed content accepted, persisted and warning emitted.

### GW-CAP-021 — order semantic

Same items in different order are considered changed.

### GW-CAP-022 — forget NVS failure

Persistent erase failure must not report forget success and must not create reboot resurrection hazard.

### GW-CAP-023 — delete aborts on capability forget failure

Device store/binding remains; stale cache is not silently orphaned.

### GW-CAP-024 — delete after successful forget

If later BLE peer forget/store delete fails, capability remains forgotten and next READY can rediscover.

### GW-CAP-025 — rebind invalidates old cache

New physical identity can never inherit old peer capability snapshot.

## 28.2 REST/UI

### GW-REST-001

GET never submits BLE command.

### GW-REST-002

POST requires `ble_central_get_device_status().ready == true`.

### GW-REST-003

POST returns exact reserved generation.

### GW-REST-004

Second same-device POST while queued/running -> 409.

### GW-REST-005

Client generation remains observable through active/last-completed transition.

### GW-REST-006

Queued/running operation is not marked failed solely by UI elapsed time.

## 28.3 ACK/command

### GW-ACK-001

Exact ACK request_id/command/device_id matches.

### GW-ACK-002

Mismatched device_id rejected.

### GW-ACK-003

`get_state` `int_value` reaches dispatcher JSON.

### GW-ACK-004

Negative ACK maps DEVICE_ERROR without losing useful correlation fields.

## 28.4 Trace

### GW-TRACE-001

Every successfully encoded TX gets nonzero frame ID before MTU validation.

### GW-TRACE-002

MTU rejection includes same frame ID.

### GW-TRACE-003

Concurrent frame-ID allocation produces no duplicates in test window.

### GW-TRACE-004

Raw RX reconstructed exactly from chunks.

### GW-TRACE-005

Raw formatting occurs outside NimBLE callback.

### GW-TRACE-006

Decode error has frame correlation.

### GW-TRACE-007

ACK decoded trace includes request ID.

### GW-TRACE-008

Capability decoded trace includes snapshot ID.

### GW-TRACE-009

Queue drop/GATT failure/ACK mismatch observable.

## 28.5 Edge cases

### GW-EDGE-001 — NVS corruption recovery

```text
corrupt persisted snapshot (invalid schema_version or truncated data)
-> load treats as UNKNOWN
-> next READY triggers initial discovery
-> fresh snapshot replaces corrupted data
```

### GW-EDGE-002 — concurrent refresh from multiple REST clients

```text
Client A POST refresh device-1 -> 202 gen=1
Client B POST refresh device-1 -> 409 (same device)
Client C POST refresh device-2 -> 202 gen=1 (different device, allowed)
```

### GW-EDGE-003 — boot during active BLE scan

```text
device_store_init -> load persisted cache -> READY
BLE scan starts -> device-1 discovered -> on_ready
committed READY exists -> no discovery triggered
```

### GW-EDGE-004 — disconnect during NVS write

```text
operation commits RAM -> NVS write starts
disconnect event arrives
NVS write completes
persist_dirty set correctly
cache state unchanged
下次 READY: no rediscovery needed
```

### GW-EDGE-005 — schema version mismatch

```text
loaded schema_version = 1 (old)
current_schema_version = 2 (minor bump)
-> accept, log migration warning, persist with schema_version=2
```

### GW-EDGE-006 — schema version major mismatch

```text
loaded schema_version = 1 (old)
current_schema_version = 2 (major bump)
-> reject snapshot, treat as UNKNOWN, rediscover
```

### GW-EDGE-007 — rate limit enforcement

```text
POST refresh device-1 -> 202 gen=1
POST refresh device-1 (within 2s) -> 429 rate_limited
wait 2s
POST refresh device-1 -> 202 gen=2
```

### GW-EDGE-008 — struct alignment verification

```text
static_assert(sizeof(capability_record_t) % 4 == 0, "alignment")
static_assert(sizeof(persisted_snapshot_t) % 4 == 0, "alignment")
```

### GW-EDGE-009 — forget during active operation

```text
device-1 has RUNNING capability operation
device_capabilities_forget(device-1)
-> invalidate running token
-> cancel staging
-> persist erase
-> clear RAM
-> operation completion event ignored (stale token)
```

---

# 29. Cross-repository tests owned by Gateway integration

Gateway CI/integration process should consume shared golden CBOR vectors covering:

- device_command set_led bool;
- device_command get_state none;
- device_ack success + int result;
- capabilities_begin;
- capability_item BOOL;
- capability_item NONE;
- capabilities_end;
- malformed sequence/snapshot cases.

Do not rely only on a hand-mirrored "gateway-style" codec in Device repo.

At least one compatibility test should compile/use actual Gateway codec source or consume vectors generated by it.

Any protocol field/size/key change requires both repo tests updated in the same feature change.

---

# 30. Hardware verification matrix

Use reference Device from companion spec.

## HW-GW-001 First pair

```text
add/bind lamp-1
-> connect/security/GATT ready
-> initial capability discovery
-> cache READY
```

## HW-GW-002 Command control

```text
set_led true
-> ACK success
-> physical LED on
```

## HW-GW-003 Query result

```text
get_state
-> ACK int_value reaches REST/MCP result
```

## HW-GW-004 Reconnect x20

No repeated capability discovery after cache exists.

## HW-GW-005 Gateway reboot

NVS cache loads READY before BLE reconnect and reconnect does not auto-refresh.

## HW-GW-006 Device reboot

Reconnect uses cached capability.

## HW-GW-007 Manual unchanged refresh

Returns UNCHANGED and performs no NVS write when clean.

## HW-GW-008 Changed capability revision

New firmware metadata -> manual refresh -> new cache persisted.

## HW-GW-009 One-side bond reset

Recovery succeeds according to Device repeat-pairing behavior.

## HW-GW-010 Trace stress

Enable raw trace and execute repeated command/capability flow; no harmful connection regression.

## HW-GW-011 Delete/re-add same logical ID

After successful delete, re-add `lamp-1` to a different physical reference device; old capability must not reappear after reboot.

---

# 31. Implementation phases

## Phase A — Capability state/operation refactor

- enums/data model;
- boot READY cache;
- sync refresh reservation;
- tokenized queue;
- global owner;
- queued/running disconnect handling;
- BUSY/error policy.

Exit: GW-CAP-001..014 pass.

## Phase B — Persistence and identity safety

- ordered equality;
- revision mismatch log;
- `persist_dirty`;
- NVS retry;
- failure-safe forget;
- delete/rebind sequencing.

Exit: GW-CAP-015..025 pass.

## Phase C — ACK result propagation

- dispatcher JSON result;
- Web/MCP response preservation.

Exit: GW-ACK suite passes.

## Phase D — REST/UI refresh

- BLE ready preflight;
- generation response;
- active/last-completed polling;
- dashboard behavior.

Exit: GW-REST suite passes.

## Phase E — Message trace

- new component;
- thread-safe frame ID;
- TX order fixed;
- RX worker raw/decode logging;
- log-buffer pressure controls.

Exit: GW-TRACE suite passes.

## Phase F — Cross-repo/hardware

- shared vectors;
- reference device hardware matrix;
- scan-name UX fix if desired.

Exit: hardware acceptance complete.

---

# 32. Build and verification workflow

Typical Gateway build:

```bash
idf.py set-target esp32s3
idf.py build
```

Run component/host tests according to repository test setup.

Verification order:

```text
1. device_capabilities unit tests
2. request manager / dispatcher tests
3. message_trace tests
4. web API tests
5. full firmware build
6. cross-repo CBOR interoperability
7. hardware matrix
```

A phase is not complete merely because firmware builds.

---

# 33. Gateway acceptance criteria

- [ ] Valid persisted capability loads as READY.
- [ ] Reconnect and reboot do not cause rediscovery when cache exists.
- [ ] Unknown device discovers once on BLE READY.
- [ ] Explicit reject -> UNSUPPORTED; timeout/BUSY/transport -> ERROR when no cache.
- [ ] Manual refresh generation is reserved before API success returns.
- [ ] Refresh active state only uses IDLE/QUEUED/RUNNING.
- [ ] Same-device initial/manual operations cannot overlap.
- [ ] Cross-device capability operations serialize through explicit owner.
- [ ] Queued disconnect cancels target only.
- [ ] Stale notify/completion cannot mutate newer operation.
- [ ] END is processed before final completion.
- [ ] Refresh failure never deletes valid committed cache.
- [ ] Unchanged clean refresh does zero NVS writes.
- [ ] NVS failure leaves valid RAM + persist_dirty.
- [ ] Later unchanged refresh retries dirty persistence.
- [ ] Same revision + changed content is accepted and warned.
- [ ] Item order is treated consistently as presentation semantics.
- [ ] Capability forget is persistent-first/failure-safe.
- [ ] Delete/rebind cannot leave a new physical peer with old capability cache.
- [ ] GET capabilities is cache-only.
- [ ] POST refresh requires BLE runtime READY.
- [ ] `get_state` payload reaches Web/MCP caller.
- [ ] MCP allowlist remains independent of capability advertisement.
- [ ] Every successfully encoded TX frame has a thread-safe frame ID before MTU validation.
- [ ] Raw RX formatting does not run in NimBLE host callback.
- [ ] Trace errors/drops are observable.
- [ ] Cross-repo Protocol v3 tests pass.
- [ ] Hardware reconnect/reboot/manual-refresh matrix passes.
- [ ] NVS corruption recovery: corrupted snapshot treated as UNKNOWN, rediscovered on next READY.
- [ ] Concurrent refresh from multiple clients: second same-device POST returns 409.
- [ ] Boot during active BLE scan: committed READY cache prevents rediscovery.
- [ ] Disconnect during NVS write: cache state unchanged, persist_dirty set correctly.
- [ ] Schema version migration: minor bump accepted with warning, major bump rejected.
- [ ] Rate limiting: refresh POST within 2s returns 429.
- [ ] Struct alignment: capability_record_t and persisted_snapshot_t are 4-byte aligned.
- [ ] Forget during active operation: running token invalidated, completion ignored.
- [ ] Message trace footprint <= 20KB baseline.
- [ ] Event message in capability_event_t is deep copy.

---

# 34. Rules for AI coding agent

1. Không đổi UUID hoặc CBOR key của Protocol v3.
2. Không thêm periodic capability polling.
3. Không gọi discovery từ GET endpoint.
4. Không demote READY cache vì disconnect/reboot.
5. Không dùng refresh state để invalidate committed cache.
6. Không trả HTTP 202 trước khi generation + operation token đã reserve và enqueue thành công.
7. Không dùng active refresh `COMPLETE`; completion chuyển vào last-completed rồi active về IDLE.
8. Không map TIMEOUT/BUSY thành UNSUPPORTED.
9. Không auto-retry hidden khi initial BUSY; target policy là ERROR + manual retry.
10. Không apply stale notify/completion khi operation token mismatch.
11. Không release global owner nếu caller/event không sở hữu exact token.
12. Phải cancel queued target khi disconnect mà không ảnh hưởng owner device khác.
13. Không bỏ `persist_dirty`.
14. Không clear RAM capability trước persistent erase khi forget/delete/rebind.
15. Không cho new physical BLE binding inherit old capability cache.
16. Không bỏ warning same revision + changed content.
17. Giữ ordered capability equality theo presentation order.
18. Mọi production refresh caller phải preflight `status.ready`.
19. Không collapse ACK thành text-only result.
20. Không override wire routing `device_id` bằng model/native ID.
21. `frame_id` allocation phải thread-safe (atomic hoặc portMUX).
22. TX frame ID phải được cấp trước MTU validation sau khi encode thành công.
23. Không format raw RX trong NimBLE host callback.
24. Không mở MCP allowlist tự động từ capability.
25. Mọi phase phải có tests tương ứng trước khi coi hoàn tất.
26. Không persist runtime fields (`updated_at_ms`, `refresh generation`, `operation_id`).
27. Schema migration phải xử lý minor/major version riêng; major mismatch phải reject.
28. Capability record sizeof không được vượt 512 bytes.
29. Message trace total footprint không được vượt 20KB baseline.
30. Event message trong `capability_event_t` phải là deep copy, không phải reference.

---

# 35. Companion Device contract

Gateway implementation giả định companion Device tuân thủ:

```text
ABF0 service
ABF1 WriteNoRsp command
ABF2 Notify status
Protocol v3
exact request_id echo
exact request.device_id echo on direct response
capability order BEGIN -> ITEM* -> END -> ACK
capability total <= 12
capability revision supplied on BEGIN
set_led BOOL advertised
get_state NONE advertised
no heap allocation in GATT command-write callback
repeat-pairing recovery implemented
```

Nếu Device không đáp ứng một contract trên, không được "fix" bằng cách làm Gateway permissive một cách im lặng. Phải cập nhật cả hai companion specs và cross-repo tests.

---

# 36. Change summary from combined v1.2

Gateway v1.3 split-spec incorporates all Gateway-relevant content from combined v1.2 and the subsequent freeze review:

- splits Gateway and Device responsibilities into independent specs;
- fixes remaining TX trace ordering inconsistency: frame ID is allocated before MTU validation;
- removes active refresh `COMPLETE` state;
- freezes initial `SUBMIT_BUSY -> ERROR` semantics;
- defines queued-operation disconnect cancellation;
- defines global serializer owner/token rules;
- makes same-revision/content-change warning mandatory;
- freezes capability order as semantic presentation order;
- makes production refresh caller BLE-ready preflight an explicit invariant;
- requires thread-safe frame-ID allocation;
- redesigns capability forget as persistent-first and failure-safe;
- freezes safe delete/rebind order so a new physical peer cannot inherit stale cache;
- adds tests for all above cases;
- changes routing examples to `lamp-1` and reserves `esp32s3-ref` for native model examples.

This file is the authoritative implementation specification for `esp-ble-gateway` v1.3 scope.

---

# 37. Change summary from v1.3 to v1.3.1

Supplements to v1.3 implementation-frozen spec:

- marks `components/message_trace` as **(planned — chưa tồn tại trong repo)** in §2.1;
- adds explicit frame_id allocation implementation with `atomic_uint` and `portMUX` alternatives in §G18;
- adds §G19: message_trace resource budget (12KB baseline, 20KB max, Kconfig defaults);
- adds struct alignment guidance and 512-byte per-record limit in §6.4;
- renames `OK_NOT_PERSISTED` to `NOT_PERSISTED` for consistency in §6.2;
- adds event message deep-copy ownership semantics in §9.2;
- adds schema version migration strategy (minor/major nibble format) in §12 and §14;
- adds explicit locking strategy (per-record lock, NVS outside lock scope) in §13.1;
- adds disconnect-during-NVS-write edge case handling in §10.4;
- adds disconnect-during-capability-forget edge case handling in §10.5;
- adds rate limiting for POST refresh (2s per-device, configurable global max) in §17.2;
- adds §28.5: 9 edge-case test cases (NVS corruption, concurrent refresh, boot+scan, NVS+disconnect, schema migration, rate limit, alignment, forget during operation);
- adds 12 new acceptance criteria items in §33;
- adds 5 new AI coding agent rules (schema migration, record size, trace budget, event deep copy) in §34.

This file is the authoritative implementation specification for `esp-ble-gateway` v1.3.1 scope.
