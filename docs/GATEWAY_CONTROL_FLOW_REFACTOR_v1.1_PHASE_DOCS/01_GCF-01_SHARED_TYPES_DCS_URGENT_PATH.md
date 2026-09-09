# GCF-01 — Shared Command Types + DCS Urgent Path
> Bộ tài liệu này được tách từ `GATEWAY_CONTROL_FLOW_REFACTOR_PLAN_v1.1.md`.
> Mỗi file phase là tài liệu triển khai độc lập và là nguồn checklist của phase tương ứng.
> Khi phase hoàn thành, đánh `[x]` toàn bộ checklist của phase và thêm `✅ DONE (YYYY-MM-DD)` vào heading phase theo rule của `AGENTS.md`.

← Trước: `00_OVERVIEW_AND_PHASE_MAP.md` | Tiếp: `02_GCF-02_DEVICE_CONTROL_SCHEDULER.md` →

## 0. Phase gate

- **Phase:** `GCF-01`
- **Phụ thuộc:** không
- **Trạng thái:** `[x] DONE (2026-09-09)`
- **Quy tắc hoàn thành:** chỉ chuyển phase tiếp theo khi toàn bộ checklist + test bắt buộc + acceptance của file này PASS.


## 1. Vấn đề cần giải quyết

#### 2.2 ACK và SUBMIT dùng chung một control queue

DCS hiện dùng bounded queue cho nhiều loại event. Khi queue chứa nhiều submit event, ACK có thể không enqueue được.

ACK là transport-critical event nên không được cạnh tranh queue với workload thông thường.

#### 2.3 Settings mutable state bị truy cập từ nhiều execution context

`device_settings_worker.c` có active operation state nhưng completion callback từ DCS có thể thay đổi state này ngoài worker context.

Trên ESP32-S3 dual-core, đây là race condition tiềm ẩn.

#### 2.8 ACK transport không đồng nghĩa logical Settings complete

`read_settings` có thể ACK trước khi `settings_values_end` tới. Nếu scheduler mark device idle ngay sau DCS completion, Web/MCP/State có thể chen vào giữa Settings stream. Cần logical device lease/reservation.

#### 2.9 Scheduler public API không được coupling business modules vào DCS

Nếu scheduler public header expose type chỉ nằm trong `device_command_service.h`, business modules vẫn phụ thuộc transport layer về compile-time. Shared request/result/status types phải được tách thành component riêng.

#### 2.10 Schema migration có nguy cơ dependency cycle

DCS hiện dùng `device_schema_validate_command()`. Nếu `device_schema` lại `REQUIRES device_control_scheduler`, graph trở thành:

```text
device_schema -> scheduler -> DCS -> device_schema
```

Schema phải giữ submitter abstraction và scheduler bridge nằm ở adapter component độc lập.


## 2. Thiết kế, file và code contract cần triển khai

##### INV-04: ACK path không dùng normal submit queue

ACK/lifecycle/cancel/shutdown phải có urgent path riêng, compact event storage và wake DCS worker ngay khi event đến.

##### INV-05: Mutable state machine có một owner

Ví dụ Settings:

```text
API / BLE / scheduler / timer
        │
        ▼
 settings event queue
        │
        ▼
 Settings Worker
        │
        └── sole owner của operation/transaction/reconcile runtime
```

Settings memory snapshot có thể có lock riêng; "single owner" áp dụng cho **state machine mutation**, không bắt buộc mọi immutable snapshot read phải chạy trong worker.

##### INV-06: Scheduler không sở hữu protocol logic

Scheduler không encode CBOR, không validate feature schema, không hiểu settings payload semantics.

Scheduler chỉ biết:

- device id;
- priority;
- shared command request type;
- source;
- job id;
- owner token;
- optional lease id;
- queue deadline;
- dedupe metadata;
- completion destination.

##### INV-07: DCS là sole ACK owner

Không module nào khác match ACK theo `request_id` để complete command.

`device_state_on_command_ack()` chỉ là observer/cache updater; không consume ACK ownership.

##### INV-08: State seed là best-effort/background

State seed không được làm user command, schema, hoặc Settings critical flow fail vì BUSY.

##### INV-09: Queue/pool bounded

Không queue/pool nào tăng động theo số request. Mọi capacity phải có metric/high-water.

##### INV-10: Exact-once terminal completion

Nếu scheduler **accept** một job thì consumer callback phải nhận đúng một terminal result:

```text
OK / command error / CANCELLED / SUPERSEDED / DEADLINE_EXCEEDED
```

Nếu submit bị reject synchronously thì scheduler không sở hữu `context` và callback **không được gọi**.

##### INV-11: No long work trong transport-critical context

Không cJSON, HTTP send, MCP serialize, NVS write lớn hoặc schema iteration dài trong DCS/ACK path.

##### INV-12: Dependency graph không cycle

Đặc biệt cấm:

```text
device_schema -> device_control_scheduler -> device_command_service -> device_schema
```

Schema phải giữ transport abstraction và được nối scheduler bằng adapter component độc lập.

---

#### 5.1 Component mới: `device_command_types`

Tách request/result/status shared types khỏi DCS để business modules và scheduler không phụ thuộc transport service header.

ADD:

```text
components/device_command_types/
├── CMakeLists.txt
└── include/
    └── device_command_types.h
```

MOVE/DEFINE tại đây:

- `device_command_origin_t`;
- `device_command_settings_payload_t`;
- `device_command_request_t`;
- `device_command_status_t`;
- `device_command_result_t`.

`device_command_service.h` include shared types và chỉ expose service API/hooks/stats.

#### 5.6 DCS

ADD:

```text
components/device_command_service/device_command_urgent.c
components/device_command_service/device_command_result.c   # transitional only if required
```

EDIT:

```text
components/device_command_service/device_command_service.c
components/device_command_service/device_command_service_internal.h
components/device_command_service/device_command_worker.c
components/device_command_service/device_command_pending.c
components/device_command_service/device_command_request.c
components/device_command_service/include/device_command_service.h
components/device_command_service/CMakeLists.txt
components/device_command_service/test/test_device_command_service.c
```

#### 5.7 Web/MCP

EDIT:

```text
components/web_server/web_command_api.c
components/web_server/CMakeLists.txt
components/mcp_endpoint/mcp_core.c
components/mcp_endpoint/CMakeLists.txt
```

Có thể ADD nếu completion bridge quá dài:

```text
components/web_server/web_command_result.c
components/mcp_endpoint/mcp_command_result.c
```

Không tạo persistent task mới nếu có thể dùng `httpd_queue_work()`/worker hiện có.

## 8. DCS refactor

### 8.1 Mục tiêu

DCS là transport command engine:

```text
validate -> encode -> send -> pending/request_id -> ACK correlation -> timeout/cancel -> compact result
```

DCS vẫn có thể depend `device_schema` để validate CONTROL request trong phase này. Chính vì vậy Schema không được depend scheduler trực tiếp; adapter component phá dependency cycle.

#### 8.2 Shared command types extraction

Trước scheduler migration, move public request/result/status types sang `device_command_types`.

DCS header sau refactor chỉ expose:

- init/deinit;
- submit;
- on_notify/on_disconnect/cancel;
- transport hooks;
- stats.

#### 8.3 Split urgent/normal queues + explicit wake-up

DCS có:

```text
normal queue
    - SUBMIT

urgent queue
    - compact ACK
    - DISCONNECT
    - CANCEL
    - SHUTDOWN
```

Không block worker chỉ trên normal queue. Mọi producer sau enqueue phải wake worker bằng task notification (preferred) hoặc Queue Set.

Preferred worker loop:

```text
while running:
    drain urgent queue
    check timeout deadlines
    process at most one normal event
    drain urgent again
    calculate nearest timeout
    ulTaskNotifyTake(... nearest timeout ...)
```

ACK đến urgent queue phải wake worker ngay cả khi normal queue rỗng.

#### 8.4 Compact urgent ACK event

Không copy full `gw_message_t` vào urgent queue.

```c
typedef struct {
    device_id_t device_id;
    uint32_t request_id;
    device_command_t command;
    bool accepted;

    bool has_bool_value;
    bool bool_value;
    bool has_int_value;
    int32_t int_value;

    bool has_feature_value_bool;
    bool feature_value_bool;
    bool has_feature_value_int;
    int32_t feature_value_int;
} dcs_ack_event_t;
```

Chỉ copy field DCS/result consumer thực sự cần.

Initial:

```c
#define DCS_URGENT_QUEUE_LEN 8
```

#### 8.5 Connectivity validation consistency

Hiện code kiểm tra connection riêng cho CONTROL origin. Refactor phải review và thống nhất policy cho Schema/State/Settings để disconnected job fail predictable trước send thay vì tùy origin rơi vào transport error.

Policy cuối phải được unit-test và document.

#### 8.6 Completion isolation

DCS callback registered bởi scheduler phải cực ngắn:

```text
copy result to scheduler inflight mailbox
notify scheduler
return
```

Legacy callers trong migration có thể dùng transitional `device_command_result.c`, nhưng phải xóa khi GCF-06 hoàn tất nếu không còn cần.

#### 8.7 DCS stats

Mở rộng:

```c
uint32_t submitted;
uint32_t completed_ok;
uint32_t completed_error;
uint32_t timeout_count;
uint32_t disconnect_count;
uint32_t transport_errors;
uint32_t queue_full;
uint32_t urgent_queue_full;
uint32_t ack_received;
uint32_t ack_unmatched;
uint32_t ack_duplicate;
uint32_t ack_dispatch_latency_max_us;
uint32_t max_pending;
```

Acceptance soak:

```text
urgent_queue_full == 0
```

## 17. CMake dependency changes

### 17.1 `device_command_types`

```cmake
idf_component_register(
    INCLUDE_DIRS "include"
    REQUIRES cbor_codec device_types
)
```

Đây là header-only component; xác nhận form này với ESP-IDF 6.1 build hiện tại trong GCF-01.

### 17.2 Scheduler

```cmake
idf_component_register(
    SRCS
        "device_control_scheduler.c"
        "device_control_scheduler_worker.c"
        "device_control_scheduler_queue.c"
        "device_control_scheduler_lease.c"
    INCLUDE_DIRS "include"
    REQUIRES
        device_command_types
        device_command_service
        device_types
        memory_policy
    PRIV_REQUIRES
        freertos
        esp_timer
)
```

Scheduler không depend Web/MCP/Settings/Schema.

### 17.3 DCS

`device_command_service`:

- add `device_command_types`;
- giữ `device_schema` dependency cho CONTROL validation hiện tại;
- không depend scheduler;
- expose compile-time capacity macro ổn định trong public service header/config:

```c
#define DEVICE_COMMAND_SERVICE_MAX_PENDING 4
```

Internal `DCS_MAX_PENDING` phải alias macro này hoặc bị thay thế để scheduler/integration layer có thể static-assert mà không include private header.

### 17.4 Schema adapter

```cmake
idf_component_register(
    SRCS "device_schema_control_adapter.c"
    INCLUDE_DIRS "include"
    REQUIRES
        device_schema
        device_control_scheduler
        device_command_types
)
```

`device_schema/CMakeLists.txt` **không add scheduler**.

### 17.7 Compile-time guards

Không copy magic capacity 16/9/4 không kiểm soát.

Tạo common constants hoặc static assert phù hợp:

```c
_Static_assert(DEVICE_CTRL_MAX_INFLIGHT <= DEVICE_COMMAND_SERVICE_MAX_PENDING, ...);
_Static_assert(DEVICE_CTRL_MAX_DEVICES >= DEVICE_STORE_MAX_DEVICES, ...);
```

Nếu `CONFIG_BT_NIMBLE_MAX_CONNECTIONS` không dùng được trong compile unit đó, document mapping và add build-time assertion tại integration layer có access config.

## 19. Memory budget

### 19.1 Nguyên tắc

ESP32-S3 có PSRAM nhưng transport-critical metadata vẫn ưu tiên internal RAM.

#### Internal RAM

Ưu tiên:

- queue control structures;
- scheduler per-device metadata;
- DCS pending table;
- urgent ACK queue storage;
- compact urgent ACK/lifecycle structs;
- scheduler slot state/generation/completion mailbox metadata.

#### PSRAM/external preferred

Có thể dùng cho:

- command request payload storage nếu slot struct lớn;
- schema snapshots tạm;
- settings snapshot/large strings;
- Web/MCP JSON allocations qua memory policy hiện có.

#### 19.2 Không tạo task tràn lan

Target new persistent tasks:

1. Control Scheduler worker: +1 task.
2. Settings: reuse worker hiện có.
3. State seed: ưu tiên reuse scheduler completion + compact actor mechanism; nếu cần task riêng thì stack nhỏ và benchmark.
4. Web/MCP: reuse HTTP/MCP worker, không tạo task mới nếu tránh được.

#### 19.3 Metrics cần log

At checkpoints:

- free internal heap;
- largest internal block;
- free PSRAM;
- largest PSRAM block;
- scheduler pool high-water mark;
- DCS urgent queue high-water mark;
- scheduler queue full count;
- scheduler queue deadline count;
- active lease count/max hold time;
- completion mailbox conflict count.

---

## 20. Logging và observability

Thêm structured log prefix:

```text
[CTRL]
[DCS]
[STATE-SEED]
[SETTINGS]
```

Ví dụ:

```text
[CTRL][dev01][job=123] queued prio=HIGH src=WEB
[CTRL][dev01][job=123] dispatch inflight=3
[DCS][dev01][req=456] sent
[DCS][dev01][req=456] ack status=OK latency=42ms
[CTRL][dev01][job=123] complete status=OK
```

Không log payload settings string nhạy cảm nếu có thể chứa credentials.

#### 20.1 Scheduler stats

Đề xuất:

```c
typedef struct {
    uint32_t submitted;
    uint32_t dispatched;
    uint32_t completed;
    uint32_t cancelled;
    uint32_t superseded;
    uint32_t deadline_exceeded;
    uint32_t queue_full;
    uint32_t deduped;
    uint32_t max_queued;
    uint32_t max_inflight;
    uint32_t lease_granted;
    uint32_t lease_revoked;
    uint32_t max_active_leases;
    uint32_t lease_hold_max_ms;
    uint32_t dcs_busy;
    uint32_t completion_mailbox_conflict;
} device_control_scheduler_stats_t;
```

Acceptance production:

```text
dcs_busy ~= 0
completion_mailbox_conflict == 0
DCS urgent_queue_full == 0
```

---


## 3. Implementation plan của phase

### GCF-01 — Shared command types + DCS urgent path ✅ DONE (2026-09-09)

#### Mục tiêu

Loại coupling type không cần thiết và bảo vệ transport-critical path trước scheduler.

#### ADD

```text
components/device_command_types/CMakeLists.txt
components/device_command_types/include/device_command_types.h
components/device_command_service/device_command_urgent.c
components/device_command_service/device_command_result.c   # transitional nếu cần
```

#### EDIT

```text
components/device_command_service/*
components/device_command_service/include/device_command_service.h
components/device_command_service/CMakeLists.txt
components/device_command_service/test/test_device_command_service.c
```

#### Implementation

- move request/result/status shared types sang `device_command_types`;
- split urgent/normal queue;
- compact ACK event, không copy full `gw_message_t`;
- ACK/disconnect/cancel/shutdown enqueue urgent;
- every producer wake DCS task bằng task notification/Queue Set;
- worker urgent-first + nearest-timeout wait;
- exact-once pending completion;
- expose `DEVICE_COMMAND_SERVICE_MAX_PENDING` public capacity macro;
- normalize connectivity validation policy across origins;
- giữ DCS submit API compatibility cho caller chưa migrate.

#### Test

1. normal queue full nhưng ACK vẫn process;
2. worker đang sleep/normal-idle, urgent ACK wakes immediately;
3. 4 pending + ACK burst;
4. 9 disconnect/lifecycle event burst;
5. ACK + disconnect race -> one terminal callback;
6. late/duplicate/unmatched ACK;
7. compact urgent event size checked;
8. legacy DCS tests pass.

#### Checklist

- [x] `device_command_types` added and build-linked.
- [x] Business shared types removed from DCS service header ownership.
- [x] Urgent queue separate from normal.
- [x] Urgent producer wakes DCS worker.
- [x] ACK event compact.
- [x] ACK not dropped under normal queue pressure.
- [x] Exact-once DCS completion tests pass.
- [x] Public DCS pending-capacity macro added and internal alias consistent.
- [x] Connectivity policy documented/tested.
- [x] Root/test builds pass.
- [x] Plan doc updated.

---


## 4. Test cases chi tiết

### TC-DCS-001 — ACK under normal queue pressure

Fill normal queue near/full, inject valid ACK.

Expected ACK enqueue/process, completion before timeout, urgent overflow 0.

### TC-DCS-002 — Disconnect vs ACK exact-once

Inject ACK and disconnect nearly simultaneously.

Expected one terminal result according to actual ordering, never double callback.

### TC-DCS-003 — Urgent event wakes worker

Worker idle waiting for notification/deadline. Inject ACK to urgent queue.

Expected immediate wake/process without waiting normal queue timeout.

### TC-DCS-004 — Lifecycle burst

Burst ACK/disconnect/cancel events for up to 9 device identities.

Expected bounded processing, urgent overflow 0 under target test load.

### TC-DCS-005 — Compact ACK extraction

Build full `gw_message_t` ACK with unrelated schema/settings fields.

Expected DCS urgent event copies only defined compact fields and result remains correct.

### TC-BUILD-001 — Dependency graph

Run root + test reconfigure/build after GCF-03.

Expected no `device_schema -> scheduler -> DCS -> device_schema` cycle and adapter linked under `MINIMAL_BUILD ON`.


## 5. Acceptance / static analysis / build-hardware gate

## 23. Static analysis / grep acceptance

#### After GCF-03 — dependency isolation

```sh
git grep "device_control_scheduler" -- components/device_schema
```

Expected: no production include/dependency inside `device_schema` itself. Scheduler references belong to `device_schema_control_adapter`.

## 24. Build validation

Root firmware:

```sh
git submodule update --init --recursive
idf.py set-target esp32s3
idf.py reconfigure
idf.py build
```

Test app:

```sh
cd test
idf.py set-target esp32s3
idf.py reconfigure
idf.py build
idf.py -p <PORT> flash monitor
```

Do project bật `MINIMAL_BUILD ON`, nếu component mới compile riêng nhưng không có trong final app, kiểm tra `REQUIRES` chain trước.

---


## 6. Checklist đóng phase

- [x] Tất cả file **ADD** trong phase đã được thêm vào CMake dependency graph.
- [x] Tất cả file **EDIT** trong phase đã được cập nhật đúng contract mới.
- [x] Không còn caller/flow legacy bị phase này yêu cầu loại bỏ.
- [x] Unit test của phase PASS trên test project.
- [x] Firmware root project build PASS cho `esp32s3`.
- [x] Không xuất hiện warning mới liên quan ownership, queue, task stack hoặc lifetime.
- [x] Static grep/acceptance của phase PASS.
- [x] Hardware test bắt buộc của phase PASS nếu phase yêu cầu BLE/Wi-Fi thực.
- [x] Memory checkpoint không regression ngoài budget đã định nghĩa.
- [x] Checklist chi tiết trong phần Implementation plan đã được đánh `[x]` toàn bộ.
- [x] Heading phase được cập nhật `✅ DONE (2026-09-09)` trước khi bắt đầu phase kế tiếp.
