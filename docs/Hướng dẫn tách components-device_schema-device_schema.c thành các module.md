# Hướng dẫn tách `components/device_schema/device_schema.c` thành các module

## 1. Mục tiêu

Branch áp dụng:

```text
dev-ws
```

Component:

```text
components/device_schema/
```

Mục tiêu của refactor:

- Giảm kích thước và độ phức tạp của `device_schema.c`.
- Mỗi module chỉ đảm nhiệm một nhóm trách nhiệm rõ ràng.
- Giữ nguyên public API `device_schema.h`.
- Không thay đổi wire protocol BLE hiện tại.
- Không thay đổi flow WebSocket/event đang sử dụng `GW_EVENT_DEVICE_SCHEMA`.
- Không thay đổi format NVS.
- Không làm tăng đáng kể SRAM/PSRAM.
- Tránh duplicate state giữa nhiều module.
- Cho phép unit test từng phần độc lập hơn.
- Dễ bổ sung schema/features sau này.

Đây nên là **structural refactor**, không phải rewrite.

---

# 2. Hiện trạng

`device_schema.c` hiện chịu trách nhiệm đồng thời cho:

1. Global runtime state.
2. Record lookup và locking.
3. Discovery serializer.
4. Submit command `describe_capabilities`.
5. Completion callback.
6. Parser các message:
   - `capabilities_begin`
   - `capability_item`
   - `feature_item`
   - `capabilities_end`
7. Commit snapshot.
8. Persist schema vào NVS.
9. Publish `GW_EVENT_DEVICE_SCHEMA`.
10. Disconnect handling.
11. Refresh lifecycle.
12. FreeRTOS worker.
13. Event queue.
14. Queue metrics.
15. Public API.
16. Command validation.
17. Test reset.

Ngay phần đầu file đã chứa FreeRTOS queue/task/mutex, global owner, listener, operation IDs và queue metrics chung trong một module. 

`device_schema_internal.h` hiện cũng đã xác định `schema_record_t`, bao gồm committed snapshot, staging snapshot, operation state và refresh state. Đây là điểm tốt để tiếp tục modular hóa. 

Hai phần đã được tách sẵn:

```text
device_schema_store.c
device_schema_validate.c
```

`device_schema_validate.c` hiện chứa các pure helper như:

```c
schema_valid_command_name()
schema_valid_tool()
schema_tool_equal()
schema_valid_feature_id()
schema_resolve_writable_tool()
```

Nên giữ module này và mở rộng đúng phạm vi validation thay vì đưa logic validation trở lại `device_schema.c`. 

---

# 3. Cấu trúc đề xuất

Không nên chia thành quá nhiều file nhỏ. Với ESP32-S3, cấu trúc cân bằng giữa maintainability và complexity nên là:

```text
components/device_schema/
├── CMakeLists.txt
├── device_schema.c
├── device_schema_internal.h
│
├── device_schema_runtime.c
├── device_schema_worker.c
├── device_schema_discovery.c
├── device_schema_protocol.c
│
├── device_schema_store.c
├── device_schema_validate.c
│
├── include/
│   └── device_schema.h
│
└── test/
    └── test_device_schema.c
```

Vai trò:

```text
device_schema.c
        │
        ├── device_schema_runtime.c
        ├── device_schema_worker.c
        │       │
        │       ├── device_schema_discovery.c
        │       └── device_schema_protocol.c
        │
        ├── device_schema_store.c
        └── device_schema_validate.c
```

---

# 4. `device_schema.c` — public facade

Sau khi refactor, file này nên chỉ chứa public API và orchestration mức cao.

Mục tiêu:

```text
~150–300 LOC
```

Nên giữ tại đây:

```c
esp_err_t device_schema_init(void);

void device_schema_set_submitter(
    device_schema_submit_fn submitter);

esp_err_t device_schema_register_commit_listener(...);

esp_err_t device_schema_register_commit_listener2(...);

esp_err_t device_schema_on_ready(...);

void device_schema_on_disconnect(...);

bool device_schema_on_notify(...);

esp_err_t device_schema_refresh(...);

esp_err_t device_schema_get(...);

esp_err_t device_schema_get_refresh_status(...);

device_schema_validation_t
device_schema_validate_command(...);

esp_err_t device_schema_forget(...);

void device_schema_get_queue_stats(...);

const char *device_schema_state_name(...);

const char *device_schema_refresh_result_name(...);

void device_schema_reset_for_test(void);
```

Public header hiện đã định nghĩa rõ API này, vì vậy **không cần thay đổi `include/device_schema.h` trong phase đầu tiên**. 

Ý tưởng quan trọng:

```text
Public API
    ↓
device_schema.c
    ↓
internal modules
```

Không để caller bên ngoài component gọi trực tiếp:

```text
device_schema_worker_*
device_schema_protocol_*
device_schema_discovery_*
device_schema_runtime_*
```

---

# 5. `device_schema_runtime.c` — owner của shared state

Đây là module quan trọng nhất của quá trình tách.

Hiện tại `device_schema.c` có quá nhiều global:

```c
s_records
s_mutex
s_queue
s_worker
s_submitter
s_commit_listener
s_commit_listener_context
s_commit_listener2
s_commit_listener2_context
s_initialized
s_shutdown
s_owner
s_next_operation_id
s_next_global_generation

s_q_enqueued
s_q_dropped
s_q_high_watermark
s_q_message_alloc_fail
s_last_drop_log_us
```

Không nên giải quyết bằng cách biến tất cả chúng thành:

```c
extern ...
```

trong `device_schema_internal.h`.

Cách đó chỉ biến một monolith thành nhiều file cùng phụ thuộc vào global state.

## Đề xuất

Tạo một runtime context duy nhất:

```c
typedef struct {
    schema_record_t *records;

    SemaphoreHandle_t mutex;

    device_schema_submit_fn submitter;

    device_schema_commit_listener_t commit_listener;
    void *commit_listener_context;

    device_schema_commit_listener2_t commit_listener2;
    void *commit_listener2_context;

    schema_global_owner_t owner;

    uint32_t next_operation_id;
    uint32_t next_generation;

    bool initialized;
} schema_runtime_t;
```

Queue-specific state nên nằm riêng trong worker context:

```c
typedef struct {
    QueueHandle_t queue;
    TaskHandle_t task;

    volatile bool shutdown;

    uint32_t enqueued;
    uint32_t dropped;
    uint32_t high_watermark;
    uint32_t message_alloc_fail;

    int64_t last_drop_log_us;
} schema_worker_context_t;
```

Runtime có thể là singleton nội bộ:

```c
static schema_runtime_t s_runtime;
```

và expose qua:

```c
schema_runtime_t *schema_runtime_get(void);
```

hoặc tốt hơn, các helper cụ thể:

```c
bool schema_runtime_lock(void);
void schema_runtime_unlock(void);

schema_record_t *
schema_runtime_find_record_locked(const char *device_id);

schema_record_t *
schema_runtime_find_or_create_record_locked(
    const char *device_id);

uint32_t schema_runtime_next_operation_id(void);
uint32_t schema_runtime_next_generation(void);
```

## Nguyên tắc

Không expose:

```c
extern schema_record_t *s_records;
```

Không expose:

```c
extern SemaphoreHandle_t s_mutex;
```

State phải có owner rõ ràng.

---

# 6. `device_schema_worker.c` — FreeRTOS queue và event dispatcher

Hiện worker thực hiện:

```c
xQueueReceive()
switch (event.type)
```

với các event:

```c
SCHEMA_EVENT_READY
SCHEMA_EVENT_REFRESH
SCHEMA_EVENT_DISCONNECT
SCHEMA_EVENT_NOTIFY
SCHEMA_EVENT_COMPLETION
```

Ngoài ra `device_schema_on_notify()` allocate một bản copy `gw_message_t`, đưa pointer vào queue rồi worker mới xử lý/free. 

Logic này nên chuyển nguyên trạng sang:

```text
device_schema_worker.c
```

Internal API đề xuất:

```c
esp_err_t schema_worker_init(void);

esp_err_t schema_worker_post_ready(
    const char *device_id);

esp_err_t schema_worker_post_refresh(
    const char *device_id,
    uint32_t operation_id,
    uint32_t generation);

esp_err_t schema_worker_post_disconnect(
    const char *device_id);

bool schema_worker_post_notify(
    const char *device_id,
    const gw_message_t *message);

esp_err_t schema_worker_post_completion(
    const char *device_id,
    uint32_t operation_id,
    uint32_t generation,
    device_schema_submit_result_t result);

void schema_worker_get_stats(
    device_schema_queue_stats_t *out);

void schema_worker_reset_for_test(void);
```

Worker dispatcher:

```c
static void schema_worker(void *arg)
{
    schema_event_t event;

    for (;;) {
        if (xQueueReceive(...) != pdTRUE) {
            ...
        }

        switch (event.type) {
        case SCHEMA_EVENT_READY:
            schema_discovery_handle_ready(...);
            break;

        case SCHEMA_EVENT_REFRESH:
            schema_discovery_start(...);
            break;

        case SCHEMA_EVENT_DISCONNECT:
            schema_discovery_handle_disconnect(...);
            break;

        case SCHEMA_EVENT_NOTIFY:
            schema_protocol_handle_message(...);
            break;

        case SCHEMA_EVENT_COMPLETION:
            schema_discovery_handle_completion(...);
            break;
        }
    }
}
```

Worker không nên biết chi tiết:

```text
staging.tool_count
staging.feature_count
NVS
refresh result mapping
```

Worker chỉ dispatch.

---

# 7. `device_schema_discovery.c` — discovery state machine

Chuyển các phần sau từ `device_schema.c` sang module này:

```c
start_discovery()
start_next_pending()
discovery_done()
make_completion_context()
handle_disconnect()
handle_completion()
```

và logic:

```text
READY
  ↓
INITIAL DISCOVERY
  ↓
QUEUE/RUNNING
  ↓
describe_capabilities
  ↓
BLE callback
  ↓
COMPLETION
```

Module này phải sở hữu toàn bộ khái niệm:

```text
INITIAL
MANUAL

IDLE
QUEUED
RUNNING

operation_id
generation
global owner
```

## Loại bỏ magic numbers

Hiện code sử dụng:

```c
operation_kind == 2 /* MANUAL */

operation_state == 1 /* QUEUED */

operation_state == 2 /* RUNNING */
```

Nên thay ngay trong quá trình modular hóa bằng enum nội bộ:

```c
typedef enum {
    SCHEMA_OPERATION_NONE = 0,
    SCHEMA_OPERATION_INITIAL,
    SCHEMA_OPERATION_MANUAL,
} schema_operation_kind_t;

typedef enum {
    SCHEMA_OPERATION_IDLE = 0,
    SCHEMA_OPERATION_QUEUED,
    SCHEMA_OPERATION_RUNNING,
} schema_operation_state_t;
```

Sau đó sửa `schema_record_t`:

```c
schema_operation_kind_t operation_kind;
schema_operation_state_t operation_state;
```

Đây không làm tăng RAM đáng kể nếu compiler vẫn dùng `int`, nhưng loại bỏ rất nhiều ambiguity.

Nếu cần ép kích thước:

```c
uint8_t operation_kind;
uint8_t operation_state;
```

và dùng enum chỉ làm symbolic constant.

---

# 8. Global discovery serializer vẫn phải giữ nguyên

Một điểm rất quan trọng: hiện schema discovery được serialize toàn hệ thống bằng `s_owner`.

Ví dụ:

```text
device A đang discovery
device B request discovery
    ↓
B = QUEUED
    ↓
A completion
    ↓
start_next_pending()
    ↓
B chạy
```

Không được vô tình biến refactor thành:

```text
mỗi device chạy discovery song song
```

vì BLE command executor có thể không hỗ trợ flow đó.

`schema_global_owner_t` do đó nên chuyển vào `device_schema_discovery.c` hoặc runtime context, nhưng semantics phải giữ nguyên.

---

# 9. `device_schema_protocol.c` — parser transaction

Module này xử lý transaction:

```text
capabilities_begin
        ↓
capability_item × N
        ↓
feature_item × M
        ↓
capabilities_end
        ↓
commit
```

Chuyển:

```c
message_device_matches()

handle_begin()
handle_tool_item()
handle_feature_item()
handle_end()

snapshot_content_equal()
```

sang:

```text
device_schema_protocol.c
```

API nội bộ duy nhất có thể là:

```c
void schema_protocol_handle_message(
    const char *device_id,
    const gw_message_t *message);
```

Bên trong:

```c
if (strcmp(message->type, "capabilities_begin") == 0) {
    handle_begin(...);
} else if (...) {
    ...
}
```

Như vậy worker không cần hiểu wire protocol.

---

# 10. Protocol module phải sở hữu staging transaction

`staging` hoạt động giống transaction buffer:

```text
BEGIN
 ↓
staging_active = true

ITEM
 ↓
append staging

FEATURE
 ↓
append staging

END
 ↓
validate completeness
 ↓
committed = staging
```

Các invariant quan trọng phải giữ nguyên:

### Tool sequence

```c
message->sequence == record->staging.tool_count
```

### Feature sequence

```c
message->sequence ==
    staging_expected_tools + staging.feature_count
```

### Không duplicate command

```text
tools[i].command
```

phải unique.

### Không duplicate feature

```text
features[i].feature_id
```

phải unique.

### Writable feature

Nếu:

```text
feature_tool != ""
```

thì command tương ứng phải tồn tại trong tool list.

### Commit chỉ khi đủ dữ liệu

```text
staging.tool_count == expected_tools
staging.feature_count == expected_features
```

Các behavior này đã được test trong `test_device_schema.c`, gồm missing item, duplicate tool, duplicate feature và feature tham chiếu tool không tồn tại.  

---

# 11. Commit pipeline không được thay đổi

Hiện `handle_end()` không chỉ commit RAM.

Flow thực tế:

```text
validate END
    ↓
commit staging → committed
    ↓
schema_persist_record()
    ↓
commit listener 1
    ↓
commit listener 2
    ↓
gateway_events_publish(
    GW_EVENT_DEVICE_SCHEMA
)
```

Việc publish `GW_EVENT_DEVICE_SCHEMA` là đặc biệt quan trọng với branch `dev-ws`, vì WebSocket/realtime consumer có thể phụ thuộc vào event này. 

Do đó `device_schema_protocol.c` phải giữ thứ tự semantic:

```text
commit
→ persistence decision
→ listeners
→ gateway event
```

Không publish event khi transaction chưa commit thành công.

---

# 12. Tách commit notification thành helper nội bộ

Trong `device_schema_protocol.c` nên có:

```c
static void publish_commit(
    const char *device_id,
    const device_schema_snapshot_t *snapshot)
{
    schema_runtime_notify_commit(
        device_id,
        snapshot->revision);

    gateway_event_t event = {0};

    event.type = GW_EVENT_DEVICE_SCHEMA;

    strlcpy(
        event.device_id,
        device_id,
        sizeof(event.device_id));

    event.schema_revision = snapshot->revision;

    gateway_events_publish(&event);
}
```

Lợi ích:

```text
protocol parsing
```

không bị trộn với:

```text
listener/event fan-out
```

---

# 13. `device_schema_validate.c`

File hiện tại đã đúng hướng.

Nên để module này sở hữu:

```c
schema_valid_command_name()
schema_valid_tool()
schema_tool_equal()
schema_valid_feature_id()
schema_resolve_writable_tool()
```

Có thể chuyển tiếp:

```c
device_schema_validate_command()
```

vào đây.

Tuy nhiên function này cần truy cập committed record, vì vậy validation module không nên truy cập trực tiếp global state.

Đề xuất:

```c
esp_err_t schema_runtime_get_snapshot(
    const char *device_id,
    device_schema_snapshot_t *out);
```

Sau đó:

```c
device_schema_validate_command()
```

validate trên snapshot local.

Ưu điểm:

- thời gian giữ mutex ngắn;
- validation không phụ thuộc implementation của record table;
- tránh expose `s_records`.

---

# 14. `device_schema_store.c`

Module NVS hiện đã được tách và nên tiếp tục giữ nguyên trách nhiệm:

```c
schema_persist_record()
schema_load_persisted()
schema_erase_nvs()
schema_cleanup_legacy_caps()
```

Internal header hiện đã expose chính xác các hàm này. 

Không nên đưa:

```text
FreeRTOS mutex
event queue
BLE submit
WebSocket event
```

vào store.

Store chỉ làm persistence.

---

# 15. Đề xuất `device_schema_internal.h`

Sau refactor, internal header có thể tổ chức như sau:

```c
#ifndef DEVICE_SCHEMA_INTERNAL_H
#define DEVICE_SCHEMA_INTERNAL_H

#include "device_schema.h"

/* ---------- Operation ---------- */

typedef enum {
    SCHEMA_OPERATION_NONE = 0,
    SCHEMA_OPERATION_INITIAL,
    SCHEMA_OPERATION_MANUAL,
} schema_operation_kind_t;

typedef enum {
    SCHEMA_OPERATION_IDLE = 0,
    SCHEMA_OPERATION_QUEUED,
    SCHEMA_OPERATION_RUNNING,
} schema_operation_state_t;

/* ---------- Record ---------- */

typedef struct {
    bool used;
    bool has_committed;
    bool persist_dirty;

    device_schema_snapshot_t committed;

    bool staging_active;
    uint32_t staging_operation_id;
    uint16_t staging_expected_tools;
    uint16_t staging_expected_features;

    device_schema_snapshot_t staging;

    schema_operation_kind_t operation_kind;
    schema_operation_state_t operation_state;
    uint32_t operation_id;

    device_schema_refresh_active_t refresh_active;
    device_schema_refresh_completed_t refresh_last_completed;
} schema_record_t;

/* ---------- Runtime ---------- */

bool schema_runtime_lock(void);
void schema_runtime_unlock(void);

schema_record_t *
schema_runtime_find_locked(const char *device_id);

schema_record_t *
schema_runtime_find_or_create_locked(
    const char *device_id);

uint32_t schema_runtime_next_operation_id(void);
uint32_t schema_runtime_next_generation(void);

/* ---------- Worker ---------- */

esp_err_t schema_worker_init(void);
void schema_worker_reset_for_test(void);

/* ---------- Discovery ---------- */

void schema_discovery_handle_ready(
    const char *device_id);

void schema_discovery_start(
    const char *device_id,
    schema_operation_kind_t kind,
    uint32_t generation);

void schema_discovery_handle_disconnect(
    const char *device_id);

void schema_discovery_handle_completion(...);

/* ---------- Protocol ---------- */

void schema_protocol_handle_message(
    const char *device_id,
    const gw_message_t *message);

/* ---------- Store ---------- */

esp_err_t schema_persist_record(...);
void schema_load_persisted(...);
esp_err_t schema_erase_nvs(...);
void schema_cleanup_legacy_caps(void);

/* ---------- Validation ---------- */

bool schema_valid_command_name(...);
bool schema_valid_tool(...);
bool schema_tool_equal(...);
bool schema_valid_feature_id(...);
int8_t schema_resolve_writable_tool(...);

#endif
```

Không cần expose static implementation detail nếu chỉ một `.c` sử dụng.

---

# 16. CMake sau khi tách

Hiện component build:

```cmake
SRCS
    "device_schema.c"
    "device_schema_store.c"
    "device_schema_validate.c"
```



Sau refactor:

```cmake
idf_component_register(
    SRCS
        "device_schema.c"
        "device_schema_runtime.c"
        "device_schema_worker.c"
        "device_schema_discovery.c"
        "device_schema_protocol.c"
        "device_schema_store.c"
        "device_schema_validate.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        cbor_codec
        device_store
        nvs_flash
        esp_timer
        freertos
        memory_policy
        gateway_events
)
```

Không cần tạo mỗi file thành một ESP-IDF component riêng.

Tất cả vẫn nên thuộc component:

```text
device_schema
```

---

# 17. Thứ tự refactor an toàn

Không nên tách tất cả trong một commit.

## Phase 1 — chuẩn hóa type

Thay magic number:

```c
1 /* INITIAL */
2 /* MANUAL */

0 /* IDLE */
1 /* QUEUED */
2 /* RUNNING */
```

bằng symbolic enum.

Không thay behavior.

Chạy test.

---

## Phase 2 — tách runtime helpers

Di chuyển:

```c
lock_records()
unlock_records()
find_record_locked()
find_or_create_record_locked()
next_operation_id()
next_generation()
```

sang:

```text
device_schema_runtime.c
```

Chạy test.

---

## Phase 3 — tách protocol

Di chuyển:

```c
message_device_matches()
snapshot_content_equal()

handle_begin()
handle_tool_item()
handle_feature_item()
handle_end()
```

sang:

```text
device_schema_protocol.c
```

Worker gọi:

```c
schema_protocol_handle_message()
```

Chạy test.

Đây là phase có rủi ro cao nhất đối với schema commit và WebSocket event.

---

## Phase 4 — tách discovery

Di chuyển:

```c
make_completion_context()
discovery_done()
start_next_pending()
start_discovery()
handle_disconnect()
handle_completion()
```

sang:

```text
device_schema_discovery.c
```

Chạy test.

---

## Phase 5 — tách worker

Di chuyển:

```c
schema_event_type_t
schema_event_t

schema_worker()
enqueue_schema_event()

queue stats
notify message allocation
```

sang:

```text
device_schema_worker.c
```

Chạy test.

---

## Phase 6 — làm mỏng `device_schema.c`

Cuối cùng `device_schema.c` chỉ giữ facade/public API và initialization orchestration.

Chạy toàn bộ test suite.

---

# 18. Không thay đổi memory model trong commit đầu

Hiện `device_schema_init()` cấp record table bằng:

```c
gw_mem_calloc(
    DEVICE_STORE_MAX_DEVICES,
    sizeof(*s_records),
    GW_MEM_EXTERNAL_PREFERRED);
```

tức là schema records đã ưu tiên external memory. 

`device_schema_on_notify()` cũng allocate copy của `gw_message_t` với:

```c
GW_MEM_EXTERNAL_PREFERRED
```

trước khi đẩy pointer vào queue. 

Trong pure refactor nên giữ nguyên hai behavior này.

Không đồng thời đổi sang:

```text
static buffers
pool allocator
zero-copy
different queue representation
```

vì nếu xảy ra regression sẽ rất khó xác định nguyên nhân.

---

# 19. Tối ưu RAM nên làm ở phase riêng

Sau khi modularization ổn định mới xem xét memory optimization.

Có thể tối ưu `schema_event_t` bằng tagged union:

```c
typedef struct {
    schema_event_type_t type;

    char device_id[GW_MSG_DEVICE_ID_LEN];

    union {
        struct {
            uint32_t operation_id;
            uint32_t generation;
        } refresh;

        struct {
            uint32_t operation_id;
            uint32_t generation;
            device_schema_submit_result_t result;
        } completion;

        struct {
            gw_message_t *message;
        } notify;
    } data;
} schema_event_t;
```

Hiện mọi event đều chứa đồng thời:

```text
operation_id
refresh_generation
message pointer
completion result
```

dù từng event chỉ dùng một phần.

Queue depth hiện là:

```c
#define SCHEMA_EVENT_QUEUE_DEPTH 32
```

nên giảm vài byte trên `schema_event_t` sẽ được nhân lên 32 queue slots.

Nhưng việc này nên là commit riêng.

---

# 20. Không tạo mutex riêng cho mỗi module

Một lỗi thiết kế dễ gặp khi tách file:

```text
runtime mutex
protocol mutex
discovery mutex
worker mutex
```

Không nên.

Record table có một synchronization domain duy nhất.

Nên giữ:

```text
1 schema mutex
```

cho:

```text
records
owner
operation state
refresh state
listener registration
```

Queue có FreeRTOS synchronization riêng và không cần mutex bổ sung.

---

# 21. Quy tắc lock

Sau refactor nên thống nhất naming convention:

```c
schema_runtime_find_locked()
schema_runtime_find_or_create_locked()
```

Suffix `_locked` có nghĩa:

> caller phải đang giữ schema mutex.

Ví dụ hợp lệ:

```c
if (!schema_runtime_lock()) {
    return ESP_ERR_TIMEOUT;
}

schema_record_t *record =
    schema_runtime_find_locked(device_id);

...

schema_runtime_unlock();
```

Không cho `_locked()` tự lock bên trong.

Điều này tránh nested mutex/deadlock khó phát hiện.

---

# 22. Callback không chạy trong mutex

Commit listener hiện được gọi **sau khi unlock mutex**. Đây là behavior đúng và phải giữ nguyên. Code hiện còn ghi chú rõ consumer chỉ nên enqueue work. 

Không refactor thành:

```c
lock();

listener(...);

unlock();
```

vì listener có thể:

- publish WebSocket event;
- enqueue công việc khác;
- gọi API schema;
- gây deadlock/reentrancy.

Quy tắc:

```text
collect state under lock
        ↓
unlock
        ↓
external callback
```

---

# 23. Không gọi NVS quá lâu trong critical section

Trong `handle_end()` hiện có một số path persistence liên quan tới mutex.

Khi tách module, nên hướng tới pattern:

```c
schema_runtime_lock();

copy snapshot to local;
determine persist action;

schema_runtime_unlock();

schema_persist_record(...);
```

Sau đó lock lại ngắn để cập nhật:

```text
persist_dirty
```

nếu cần.

Mục tiêu là mutex bảo vệ RAM state chứ không bao quanh flash I/O lâu hơn cần thiết.

Không nhất thiết phải thay toàn bộ trong commit đầu; có thể làm ở cleanup commit sau refactor.

---

# 24. Public `device_schema_get()` nên copy snapshot

Pattern tốt:

```c
lock
 ↓
copy committed → caller buffer
 ↓
unlock
```

Không expose pointer:

```c
const device_schema_snapshot_t *
device_schema_get_ptr(...);
```

vì caller có thể giữ pointer trong lúc worker cập nhật record.

Public API hiện dùng copy-out và nên giữ cách này. 

---

# 25. `device_schema_refresh()` thuộc facade + discovery

Public validation:

```text
device tồn tại?
schema initialized?
operation đang chạy?
```

có thể ở `device_schema.c`.

State transition:

```text
MANUAL
QUEUED
generation
operation ID
```

nên được encapsulate trong discovery module.

Ví dụ:

```c
esp_err_t device_schema_refresh(
    const char *device_id,
    uint32_t *out_generation)
{
    if (...) {
        return ...;
    }

    return schema_discovery_request_refresh(
        device_id,
        out_generation);
}
```

Như vậy facade không biết:

```text
operation_state
operation_kind
owner
```

---

# 26. `device_schema_forget()` thuộc runtime/store

Forget hiện gồm hai phần:

```text
clear/cancel RAM operation
        +
erase NVS
```

Nên giữ transaction semantics:

```text
cancel runtime operation
        ↓
attempt NVS erase
        ↓
nếu thành công mới zero record
```

Hiện code cố tình giữ RAM cache nếu NVS erase thất bại. Đây là behavior cần bảo toàn. 

Không refactor thành:

```text
zero RAM
↓
NVS erase fail
```

vì sẽ thay behavior.

---

# 27. Các test hiện có phải tiếp tục pass

Test hiện tại đã bao phủ khá tốt:

```text
validation helper
init lifecycle
unknown device
discovery happy path
missing tool item
duplicate tool
duplicate feature
missing writable tool
forget
NVS persistence
corrupt NVS data
legacy cleanup
```

 

Trong modularization, nguyên tắc là:

```text
move code
→ build
→ run tests
→ commit
→ move next responsibility
```

Không:

```text
move 1000 LOC
→ sửa behavior
→ tối ưu memory
→ đổi API
→ chạy test cuối cùng
```

---

# 28. Test mới nên bổ sung trước refactor

Nên bổ sung regression tests cho boundary giữa các module.

### Discovery serializer

```text
A running
B ready
C refresh

→ chỉ A được submit
→ B/C queued
→ completion A
→ đúng một pending operation tiếp theo được start
```

### Disconnect khi RUNNING

```text
manual refresh running
→ disconnect
→ owner released
→ operation idle
→ refresh completed = DISCONNECTED
→ next pending starts
```

### Disconnect khi QUEUED

```text
queued manual refresh
→ disconnect
→ queue reservation cancelled
→ BLE submit không xảy ra
```

### Stale completion

```text
old operation completion
→ không được clear current owner
→ không được mutate current operation
```

### Commit listener

```text
schema END successful
→ listener 1 exactly once
→ listener 2 exactly once
```

### Gateway event

```text
successful schema commit
→ exactly one GW_EVENT_DEVICE_SCHEMA
→ correct device_id
→ correct revision
```

### Queue overflow

```text
queue full
→ dropped++
→ allocated notify message được free
```

Các test này đặc biệt quan trọng với `dev-ws`.

---

# 29. Một điểm đáng lưu ý trong code hiện tại

Trong disconnect của manual refresh đang có sequence tương tự:

```c
record->refresh_active.state =
    DEVICE_SCHEMA_REFRESH_IDLE;

record->refresh_active.generation = 0;

record->refresh_last_completed.generation =
    record->refresh_active.generation;
```

Điều đó khiến completed generation có khả năng nhận `0` thay vì generation vừa bị disconnect. 

Đây nên được ghi thành **bug/regression test riêng**.

Không nên âm thầm sửa nó trong cùng commit "split modules", vì khi test behavior thay đổi sẽ khó phân biệt refactor với bugfix.

Tốt nhất:

```text
Commit A: add failing regression test
Commit B: fix disconnect generation
Commit C+: modularization
```

hoặc modularize trước nhưng bảo toàn behavior, sau đó sửa bug bằng PR riêng.

---

# 30. Một điểm khác cần cleanup: operation ID

`device_schema_refresh()` hiện reserve:

```c
uint32_t op_id = next_operation_id();
record->operation_id = op_id;
```

nhưng khi worker thực sự chạy `start_discovery()`, function này lại tạo:

```c
uint32_t op_id = next_operation_id();
```

và overwrite operation ID.  

Behavior này nên được làm rõ.

Có hai model hợp lệ:

### Model A — reservation có operation ID

```text
refresh()
→ create operation_id
→ queue event(operation_id)
→ start_discovery(operation_id)
```

ID không đổi.

Đây là model tôi khuyến nghị.

### Model B — chỉ RUNNING operation có ID

```text
refresh()
→ reserve generation
→ queue
→ start_discovery()
→ create operation_id
```

Nếu dùng model này thì không nên tạo operation ID trong `device_schema_refresh()`.

Không nên giữ cả hai.

Tuy nhiên cũng nên sửa ở commit riêng sau khi có regression test.

---

# 31. Dependency direction mong muốn

Sau refactor:

```text
device_schema.c
    ↓
runtime
worker
discovery
validate
store

worker
    ↓
discovery
protocol

discovery
    ↓
runtime
worker

protocol
    ↓
runtime
validate
store
gateway_events
```

Quan trọng nhất:

```text
store
```

không phụ thuộc:

```text
worker/discovery
```

và:

```text
validate pure helpers
```

không phụ thuộc FreeRTOS.

---

# 32. Tránh circular dependency

Một chỗ dễ tạo circular dependency:

```text
worker → discovery
discovery → worker
```

vì discovery callback cần gửi COMPLETION về queue.

Có thể giải bằng một interface nhỏ:

```c
esp_err_t schema_worker_post_completion(...);
```

Discovery biết worker API nhưng worker dispatch sang discovery.

Ở C linker đây không phải vấn đề nếu header được tổ chức đúng, nhưng kiến trúc vẫn nên rõ.

Một cách sạch hơn:

```text
schema_event_queue.c
```

là module rất nhỏ chỉ chịu trách nhiệm enqueue.

Tuy nhiên với quy mô hiện tại chưa cần thêm module này.

---

# 33. Quy tắc include

Public file:

```c
#include "device_schema.h"
```

Internal implementation:

```c
#include "device_schema_internal.h"
```

Không cho component khác include:

```text
device_schema_internal.h
```

`device_schema_internal.h` hiện nằm ngoài `include/`, đây là cách bố trí đúng để giữ nó private. 

---

# 34. Logging

Nên giữ tag theo module để debug rõ hơn:

```c
static const char *TAG = "schema";
```

hoặc:

```text
schema
schema_worker
schema_discovery
schema_proto
schema_store
```

Tôi khuyến nghị:

```c
"schema"
"schema_worker"
"schema_disc"
"schema_proto"
"schema_store"
```

ESP_LOG khi đó cho biết lỗi nằm ở layer nào mà không cần thêm text.

---

# 35. Definition of Done

Refactor chỉ được xem là hoàn thành khi:

```text
[ ] device_schema.c chỉ còn facade/orchestration
[ ] không có extern global mutable state
[ ] chỉ một mutex bảo vệ record/runtime state
[ ] worker chỉ dispatch event
[ ] discovery sở hữu operation lifecycle
[ ] protocol sở hữu staging transaction
[ ] store chỉ làm persistence
[ ] validate pure helper không phụ thuộc FreeRTOS
[ ] public device_schema.h không đổi API
[ ] NVS format không đổi
[ ] GW_EVENT_DEVICE_SCHEMA vẫn được publish sau commit
[ ] listeners vẫn chạy ngoài mutex
[ ] queue allocation/free semantics không đổi
[ ] existing tests pass
[ ] serializer tests pass
[ ] disconnect tests pass
[ ] stale completion test pass
[ ] WebSocket schema update flow vẫn hoạt động
```

---

# 36. Cấu trúc cuối cùng khuyến nghị

```text
device_schema/
│
├── device_schema.c
│   └── Public facade
│
├── device_schema_runtime.c
│   ├── record table
│   ├── mutex
│   ├── listeners
│   ├── IDs
│   └── shared runtime state
│
├── device_schema_worker.c
│   ├── FreeRTOS task
│   ├── queue
│   ├── queue metrics
│   └── event dispatch
│
├── device_schema_discovery.c
│   ├── global serializer
│   ├── READY
│   ├── refresh
│   ├── submit
│   ├── completion
│   └── disconnect
│
├── device_schema_protocol.c
│   ├── BEGIN
│   ├── TOOL_ITEM
│   ├── FEATURE_ITEM
│   ├── END
│   └── commit/event publishing
│
├── device_schema_store.c
│   └── NVS
│
├── device_schema_validate.c
│   └── validation
│
└── device_schema_internal.h
    └── private contracts
```

Đây là mức tách phù hợp nhất với component hiện tại: đủ nhỏ để maintain, nhưng không biến code ESP32 thành hàng chục abstraction/file không cần thiết.

---

# 37. Kết luận

Ưu tiên refactor nên là:

```text
runtime helpers
    ↓
protocol
    ↓
discovery
    ↓
worker
    ↓
thin facade
```

Không nên bắt đầu bằng việc "cắt file theo số dòng".

Ranh giới phải dựa trên ownership:

```text
Who owns state?
Who owns lifecycle?
Who owns protocol?
Who owns persistence?
Who owns scheduling?
```

Với `dev-ws`, boundary quan trọng nhất cần bảo toàn là:

```text
BLE schema discovery
        ↓
atomic schema commit
        ↓
NVS persistence
        ↓
commit listeners
        ↓
GW_EVENT_DEVICE_SCHEMA
        ↓
WebSocket/realtime consumers
```

Nếu giữ nguyên pipeline này, việc tách `device_schema.c` có thể thực hiện từng bước với rủi ro thấp và không ảnh hưởng flow realtime.