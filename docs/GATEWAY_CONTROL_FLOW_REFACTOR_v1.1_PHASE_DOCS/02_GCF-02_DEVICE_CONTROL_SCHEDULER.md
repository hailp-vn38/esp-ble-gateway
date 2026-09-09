# GCF-02 — Device Control Scheduler + Lease/Cancel/Deadline ✅ DONE (2026-09-09)
> Bộ tài liệu này được tách từ `GATEWAY_CONTROL_FLOW_REFACTOR_PLAN_v1.1.md`.
> Mỗi file phase là tài liệu triển khai độc lập và là nguồn checklist của phase tương ứng.
> Khi phase hoàn thành, đánh `[x]` toàn bộ checklist của phase và thêm `✅ DONE (YYYY-MM-DD)` vào heading phase theo rule của `AGENTS.md`.

← Trước: `01_GCF-01_SHARED_TYPES_DCS_URGENT_PATH.md` | Tiếp: `03_GCF-03_SCHEMA_CONTROL_ADAPTER.md` →

## 0. Phase gate

- **Phase:** `GCF-02`
- **Phụ thuộc:** GCF-01 phải DONE
- **Trạng thái:** `[x] DONE (2026-09-09)`
- **Quy tắc hoàn thành:** chỉ chuyển phase tiếp theo khi toàn bộ checklist + test bắt buộc + acceptance của file này PASS.


## 1. Vấn đề cần giải quyết

#### 2.1 State seed bulk-submit xung đột với DCS

`components/device_state/device_state.c` hiện duyệt toàn bộ feature sau schema commit và gọi `device_command_service_submit()` liên tục.

Trong khi DCS chỉ cho phép một pending command trên cùng device.

Hậu quả:

```text
schema commit
    │
    ▼
submit read F1
submit read F2
submit read F3
submit read F4
    │
    ▼
DCS nhận F1 -> pending
DCS nhận F2 -> BUSY
DCS nhận F3 -> BUSY
DCS nhận F4 -> BUSY
```

State cache có thể chỉ được seed một phần.

#### 2.4 Listener order không phải scheduler guarantee

Đăng ký Settings listener trước State listener không đảm bảo Settings command đã lấy command slot trước State seed.

Ordering phải do scheduler enforce, không dựa vào task scheduling timing.

#### 2.5 DCS completion callback chạy presentation/network work

Web/MCP completion hiện có thể serialize JSON và gửi response trong callback chain bắt nguồn từ DCS.

DCS phải được giữ ngắn:

```text
validate -> send -> ACK correlation -> timeout/cancel -> dispatch result
```

Không làm:

```text
cJSON -> HTTP response -> MCP formatting -> socket transport
```

#### 2.6 Device delete chưa purge toàn bộ settings runtime

Delete hiện purge command/schema/state/BLE/store nhưng chưa có contract rõ ràng để purge settings snapshot, pending operation, transaction và reconcile state.

#### 2.7 Gateway READY đang gắn quá sớm với Wi-Fi CONNECTED

Wi-Fi connected chỉ nghĩa là network ready, không có nghĩa BLE/DCS/Web/MCP/Gateway đã ready.

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

#### 2.11 Cancel/context lifetime/deadline chưa có contract tổng quát

Sau khi thêm queue trung gian, cần định nghĩa rõ:

- synchronous reject có gọi callback hay không;
- accepted job bị cancel/dedupe/delete phải terminal thế nào;
- ai sở hữu `void *context`;
- cancel một seed revision có được ảnh hưởng Web/Settings hay không;
- job chờ scheduler bao lâu trước khi trở thành stale.

---


## 2. Thiết kế, file và code contract cần triển khai

### 3. Kiến trúc đích và invariants

#### 3.1 Invariant bắt buộc

##### INV-01: Transport inflight tối đa một command/device

```text
transport_inflight(device_id) <= 1
```

Đây là invariant của scheduler/DCS và không được nới.

##### INV-02: Logical device lease tách biệt transport inflight

Một ACK của DCS chỉ kết thúc **transport command**, không nhất thiết kết thúc logical operation.

Ví dụ `read_settings`:

```text
READ_SETTINGS
    ↓
ACK                 <- transport command complete
    ↓
settings values stream
    ↓
settings_values_end <- logical operation complete
```

Vì vậy scheduler phải hỗ trợ logical reservation/lease:

```text
lease(device_id) = owner
```

Khi lease đang active:

- chỉ job có `lease_id` tương ứng được dispatch trên device đó;
- job Web/MCP/State/Schema khác vẫn queue nhưng không chen vào;
- lease được release bởi actor owner khi logical operation hoàn tất/cancel.

Settings describe/read, transaction/reconcile là consumer chính của lease trong refactor này.

##### INV-03: Global bounded transport inflight

Mặc định:

```text
DEVICE_CTRL_MAX_INFLIGHT = 4
```

Giá trị này độc lập số BLE links nhưng phải compile-time assert:

```c
_Static_assert(DEVICE_CTRL_MAX_INFLIGHT <= DEVICE_COMMAND_SERVICE_MAX_PENDING,
               "scheduler inflight exceeds DCS pending capacity");
```

Không tăng `DEVICE_COMMAND_SERVICE_MAX_PENDING` chỉ để che orchestration bug.

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

### 4. Priority model

Public scheduler priority chỉ có workload priorities:

```c
typedef enum {
    DEVICE_CTRL_PRIORITY_HIGH = 0,
    DEVICE_CTRL_PRIORITY_NORMAL,
    DEVICE_CTRL_PRIORITY_LOW,
    DEVICE_CTRL_PRIORITY_BACKGROUND,
} device_control_priority_t;
```

Không expose `CRITICAL` cho business caller. Transport-critical ACK/lifecycle nằm hoàn toàn trong DCS urgent path.

| Workload | Priority |
|---|---|
| Settings transaction/reconcile đang giữ lease | HIGH |
| REST user control | HIGH |
| MCP/Xiaozhi control | HIGH |
| Schema discovery | NORMAL |
| Settings describe/read interactive | NORMAL |
| Settings background refresh | LOW |
| Explicit state read | NORMAL |
| Automatic state seed | BACKGROUND |

#### 4.1 Fairness

Trong cùng priority:

- round-robin theo device;
- một device có lease chỉ block chính device đó;
- không drain toàn bộ queue A trước B/C;
- global inflight vẫn bounded.

Ví dụ:

```text
A: A1 A2 A3
B: B1 B2
C: C1

Dispatch:
A1 -> B1 -> C1 -> A2 -> B2 -> A3
```

#### 4.2 Starvation protection

Nếu HIGH liên tục:

- sau `HIGH_BURST_MAX` dispatch, cho một NORMAL runnable;
- không preempt inflight command;
- không phá active lease;
- BACKGROUND chỉ chạy khi không có HIGH/NORMAL runnable trên device không bị lease khác giữ.

Initial:

```text
HIGH_BURST_MAX = 8
```

---

#### 5.2 Component mới: `device_control_scheduler`

ADD:

```text
components/device_control_scheduler/
├── CMakeLists.txt
├── include/
│   └── device_control_scheduler.h
├── device_control_scheduler.c
├── device_control_scheduler_worker.c
├── device_control_scheduler_queue.c
├── device_control_scheduler_lease.c
├── device_control_scheduler_internal.h
└── test/
    └── test_device_control_scheduler.c
```

## 6. Public API đề xuất cho Device Control Scheduler

File:

```text
components/device_control_scheduler/include/device_control_scheduler.h
```

Scheduler include shared command types, **không include `device_command_service.h`**.

```c
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "device_command_types.h"
#include "esp_err.h"

typedef uint32_t device_control_job_id_t;
typedef uint32_t device_control_owner_token_t;
typedef uint32_t device_control_lease_t;

typedef enum {
    DEVICE_CTRL_PRIORITY_HIGH = 0,
    DEVICE_CTRL_PRIORITY_NORMAL,
    DEVICE_CTRL_PRIORITY_LOW,
    DEVICE_CTRL_PRIORITY_BACKGROUND,
} device_control_priority_t;

typedef enum {
    DEVICE_CTRL_SOURCE_WEB = 0,
    DEVICE_CTRL_SOURCE_MCP,
    DEVICE_CTRL_SOURCE_SCHEMA,
    DEVICE_CTRL_SOURCE_SETTINGS,
    DEVICE_CTRL_SOURCE_STATE,
    DEVICE_CTRL_SOURCE_INTERNAL,
} device_control_source_t;

typedef enum {
    DEVICE_CTRL_DEDUPE_NONE = 0,
    DEVICE_CTRL_DEDUPE_DROP_IF_EXISTS,
    DEVICE_CTRL_DEDUPE_REPLACE_QUEUED,
} device_control_dedupe_t;

typedef enum {
    DEVICE_CTRL_TERMINAL_COMMAND = 0,
    DEVICE_CTRL_TERMINAL_CANCELLED,
    DEVICE_CTRL_TERMINAL_SUPERSEDED,
    DEVICE_CTRL_TERMINAL_DEADLINE_EXCEEDED,
} device_control_terminal_t;

typedef struct {
    device_command_request_t request;
    device_control_priority_t priority;
    device_control_source_t source;
    device_control_dedupe_t dedupe;

    device_control_owner_token_t owner_token;
    device_control_lease_t lease_id;      /* 0 = no lease */

    /* 0 = no queue-wait deadline. Transport timeout remains owned by DCS. */
    uint32_t max_queue_wait_ms;

    /* Hash is only an accelerator; equality must verify structured key fields. */
    uint32_t dedupe_hash;
} device_control_job_t;

typedef struct {
    device_control_job_id_t scheduler_job_id;
    device_control_terminal_t terminal;
    device_command_result_t command_result; /* valid for COMMAND terminal */
} device_control_result_t;

typedef void (*device_control_completion_fn)(
    const device_control_result_t *result,
    void *context);

typedef struct {
    const char *device_id;
    device_control_priority_t priority;
    device_control_source_t source;
    device_control_owner_token_t owner_token;
} device_control_lease_request_t;

typedef void (*device_control_lease_completion_fn)(
    esp_err_t status,
    device_control_lease_t lease_id,
    void *context);

esp_err_t device_control_scheduler_init(void);
void device_control_scheduler_deinit(void);

esp_err_t device_control_scheduler_submit(
    const device_control_job_t *job,
    device_control_completion_fn completion,
    void *context,
    device_control_job_id_t *out_job_id);

esp_err_t device_control_scheduler_cancel_job(device_control_job_id_t job_id);

esp_err_t device_control_scheduler_cancel_source(
    const char *device_id,
    device_control_source_t source,
    device_control_owner_token_t owner_token);

esp_err_t device_control_scheduler_cancel_device(const char *device_id);

esp_err_t device_control_scheduler_acquire_lease(
    const device_control_lease_request_t *request,
    device_control_lease_completion_fn completion,
    void *context);

esp_err_t device_control_scheduler_release_lease(device_control_lease_t lease_id);

esp_err_t device_control_scheduler_block_device(const char *device_id);
esp_err_t device_control_scheduler_unblock_device(const char *device_id);

/* Block new jobs, cancel queued/inflight work, and wait bounded until quiesced. */
esp_err_t device_control_scheduler_quiesce_device(
    const char *device_id,
    uint32_t timeout_ms);

bool device_control_scheduler_is_idle(const char *device_id);
void device_control_scheduler_get_stats(/* stats struct */);
```

#### 6.1 Submit/ownership contract

##### Submit rejected synchronously

Ví dụ pool full, invalid input hoặc scheduler chưa init:

- return != `ESP_OK`;
- scheduler không sở hữu `context`;
- callback không được gọi;
- caller tự cleanup context.

##### Submit accepted

Sau `ESP_OK`:

- scheduler sở hữu job lifecycle;
- callback terminal **exactly once**;
- caller không free/reuse context trước callback;
- sau callback scheduler không chạm context nữa.

##### Dedupe contract

`DEVICE_CTRL_DEDUPE_DROP_IF_EXISTS` chỉ dùng cho background/idempotent reads.

Nếu duplicate đã tồn tại:

- return `ESP_ERR_INVALID_STATE`/project alias `ALREADY_EXISTS` synchronously;
- callback của request mới không được đăng ký;
- `out_job_id` có thể trả existing id nếu API implementation hỗ trợ.

`DEVICE_CTRL_DEDUPE_REPLACE_QUEUED`:

- chỉ replace **queued, chưa inflight** job;
- old accepted job nhận terminal `SUPERSEDED` exactly once;
- không dùng cho user writes, MCP semantic writes hoặc Settings transaction.

#### 6.2 Queue deadline contract

`max_queue_wait_ms` đo từ lúc scheduler accept đến lúc dispatch DCS.

Nếu quá deadline trước dispatch:

```text
terminal = DEADLINE_EXCEEDED
```

DCS ACK timeout chỉ bắt đầu sau khi command thực sự dispatch.

Initial policy khuyến nghị:

| Workload | max queue wait |
|---|---:|
| Web/MCP user control | 1000-1500 ms |
| Schema discovery | 5000 ms |
| Settings background refresh | 5000 ms hoặc policy actor |
| State seed | 0, best-effort/no deadline |

Giữ tổng Web/MCP queue wait + DCS ACK timeout dưới HTTP timeout hiện tại.

#### 6.3 Lease request ownership contract

Lease acquisition cũng là accepted-operation contract:

- scheduler copy `device_id/source/owner_token` ngay khi accept;
- synchronous reject -> callback lease không gọi;
- accepted lease request -> callback exactly once với `lease_id != 0` hoặc terminal cancel/error;
- `cancel_source()` phải cancel cả queued lease request matching owner token;
- active lease bị disconnect/delete revoke theo lifecycle event và actor được đánh thức/cancel qua integration contract;
- `release_lease()` chỉ hợp lệ khi actor owner release đúng lease; release sai/stale lease trả error, không unblock nhầm owner.

Scheduler track `lease_started_us` và metric `lease_hold_max_ms`; phase đầu chỉ WARN khi lease giữ quá ngưỡng debug, không auto-revoke transaction đang hợp lệ.

#### 6.4 Không expose DCS submit cho business modules sau migration

Sau GCF-06:

```sh
git grep "device_command_service_submit(" -- components main
```

Production caller chỉ còn scheduler internals. `device_schema` không direct depend scheduler; adapter component làm bridge.

---

### 7. Internal scheduler design

#### 7.1 Fixed pool + one worker

Không tạo queue/task per device.

Initial constants:

```c
#define DEVICE_CTRL_MAX_DEVICES   16   /* current Device Store capacity */
#define DEVICE_CTRL_MAX_JOBS      24
#define DEVICE_CTRL_MAX_INFLIGHT  4
```

Scheduler table capacity theo inventory/runtime identities, không theo số BLE link đồng thời. Hiện Device Store cho phép 16 entry còn NimBLE max connections là 9; global transport inflight vẫn 4.

Không cần completion queue nếu dùng per-slot completion mailbox + task notification; cách này loại nguy cơ terminal result bị drop.

#### 7.2 Slot state machine và concurrent submit contract

```c
typedef enum {
    CTRL_SLOT_FREE = 0,
    CTRL_SLOT_ENQUEUE_PENDING,
    CTRL_SLOT_QUEUED,
    CTRL_SLOT_INFLIGHT,
    CTRL_SLOT_COMPLETION_PENDING,
    CTRL_SLOT_COMPLETING,
} device_control_slot_state_t;
```

Submit có thể đến đồng thời từ Core0/Core1. Flow bắt buộc:

```text
caller
  ↓
short scheduler lock
  ├─ find FREE slot
  ├─ copy request/context
  ├─ generation++
  └─ state = ENQUEUE_PENDING
  ↓ unlock
post slot index/wake worker
  ├─ success -> worker becomes lifecycle owner
  └─ fail    -> reclaim same generation under short lock
```

Không giữ lock khi:

- gọi DCS;
- gọi consumer callback;
- serialize/log lớn;
- scan schema.

#### 7.3 Job slot

```c
typedef struct {
    device_control_slot_state_t state;
    uint32_t generation;
    device_control_job_id_t id;

    device_control_job_t job;
    device_control_completion_fn completion;
    void *context;

    int64_t enqueue_us;
    int64_t queue_deadline_us;

    device_command_result_t pending_result;
    bool completion_written;

    int16_t next;
} device_control_job_slot_t;
```

Pool metadata ưu tiên internal RAM. Nếu request payload làm struct quá lớn, tách payload storage sang external-preferred nhưng slot state/generation phải ở internal RAM.

#### 7.4 Per-device state

```c
typedef struct {
    bool used;
    bool blocked;
    device_id_t device_id;

    bool transport_inflight;
    int16_t inflight_job;

    device_control_lease_t active_lease;
    device_control_owner_token_t lease_owner_token;
    device_control_source_t lease_source;
    int64_t lease_started_us;

    int16_t queue_head;
    int16_t queue_tail;

    uint32_t dispatched;
    uint32_t completed;
} device_control_device_t;
```

#### 7.5 Lease/reservation state machine

Lease acquisition là scheduler operation, không phải mutex lấy trực tiếp từ caller context.

```text
LEASE_REQUEST queued
      ↓ arbitration theo priority/fairness
no transport inflight + no active lease
      ↓
LEASE_GRANTED
      ↓
actor submit jobs carrying lease_id
      ↓
logical operation complete/cancel
      ↓
RELEASE_LEASE
```

Khi lease active:

- jobs không có matching lease không dispatch trên device;
- matching lease jobs vẫn chịu `transport_inflight <= 1`;
- disconnect/delete auto-revoke lease và actor nhận cancel event/generation invalidation;
- lease callback không mutate Settings state trực tiếp, chỉ post event về actor.

#### 7.6 Scheduler worker responsibilities

Worker là sole owner sau slot accepted/enqueued:

- move `ENQUEUE_PENDING -> QUEUED`;
- priority/fairness arbitration;
- grant/release lease;
- expire queue deadlines;
- dispatch DCS;
- process per-slot completion pending;
- exact-once terminal transition;
- targeted cancel/supersede;
- unblock next runnable job;
- stats/high-water.

#### 7.7 Completion path không được drop

Preferred v1.1:

```text
DCS completion callback
      │
      ├─ validate slot index + generation
      ├─ copy compact device_command_result_t into inflight slot
      ├─ state = COMPLETION_PENDING
      └─ xTaskNotifyGive(scheduler_task)
             │
             ▼
       Scheduler Worker
             ├─ clear transport inflight
             ├─ terminal callback exactly once
             └─ dispatch next runnable job
```

Không dùng bounded completion queue làm nơi duy nhất giữ terminal result.

Nếu implementation vẫn dùng queue, queue overflow phải có fallback mailbox và tuyệt đối không drop completion.

#### 7.8 Targeted cancel

Ba cấp:

1. `cancel_job(job_id)` — đúng một job;
2. `cancel_source(device, source, owner_token)` — ví dụ seed revision cũ;
3. `cancel_device(device)` — lifecycle destructive operation.

State seed revision replacement **không** được gọi `cancel_device()`.

#### 7.9 DCS BUSY policy

Sau migration hoàn chỉnh, DCS BUSY từ scheduler là invariant violation hoặc bounded transition artifact:

```text
scheduler_dcs_busy++
log warning
bounded retry only if documented
otherwise terminal error
```

Expected steady-state:

```text
scheduler_dcs_busy == 0
```

---

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

### 17.6 Main

Add:

```text
device_control_scheduler
device_schema_control_adapter
device_command_types
```

và `gateway_runtime.c` vào `SRCS`.

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

### GCF-02 — Device Control Scheduler + lease/cancel/deadline

#### Mục tiêu

Tạo scheduler bounded, SMP-safe và đủ contract trước khi migrate caller.

#### ADD

```text
components/device_control_scheduler/CMakeLists.txt
components/device_control_scheduler/include/device_control_scheduler.h
components/device_control_scheduler/device_control_scheduler.c
components/device_control_scheduler/device_control_scheduler_worker.c
components/device_control_scheduler/device_control_scheduler_queue.c
components/device_control_scheduler/device_control_scheduler_lease.c
components/device_control_scheduler/device_control_scheduler_internal.h
components/device_control_scheduler/test/test_device_control_scheduler.c
```

#### EDIT

```text
main/CMakeLists.txt
test/CMakeLists.txt
```

#### Implementation

- fixed job pool + slot generation/state machine;
- concurrent submit short-lock contract;
- per-device priority queue + round-robin fairness;
- one transport inflight/device, max 4 global;
- per-slot completion mailbox + task notify;
- exact-once callback/context ownership;
- queue deadline;
- `cancel_job`, `cancel_source`, `cancel_device`;
- device block/unblock/quiesce;
- async logical lease acquire/release;
- dedupe restricted to safe/idempotent work;
- compile-time inflight/capacity guards.

#### Test

1. same device serialization;
2. 5 devices with max inflight 4;
3. Core0/Core1 concurrent submit into fixed pool;
4. HIGH before queued BACKGROUND at dispatch boundary;
5. lease blocks non-owner job but allows matching lease job;
6. release lease unblocks queued user job;
7. targeted cancel does not cancel other source/job;
8. queue deadline terminal exactly once;
9. replace queued -> old callback SUPERSEDED;
10. completion mailbox never loses terminal result;
11. block/quiesce/unblock lifecycle.

#### Checklist

- [x] Fixed pool SMP-safe.
- [x] Exact-once contract implemented.
- [x] One inflight/device enforced.
- [x] Global inflight <= 4 enforced and static-asserted against DCS public capacity.
- [x] Scheduler device table capacity covers current 16-entry Device Store inventory.
- [x] Lease implemented/tested.
- [x] Targeted cancel implemented/tested.
- [x] Queue deadline implemented/tested.
- [x] Dedupe semantics documented/tested.
- [x] No long callback under scheduler lock.
- [x] Root/test builds pass.
- [x] Plan doc updated.

---


## 4. Test cases chi tiết

### TC-CTRL-001 — Same device transport serialization

Given 10 accepted jobs for Device A.

Expected:

```text
max transport inflight A = 1
all accepted jobs terminal exactly once
```

### TC-CTRL-002 — Multi-device bounded concurrency

A/B/C/D/E each have one runnable command, max inflight 4.

Expected 4 dispatch, fifth waits; completion opens one slot.

### TC-CTRL-003 — Priority at dispatch boundary

Given Device A has queued BACKGROUND F3 and a HIGH user job is accepted **before the next scheduler dispatch decision**.

Expected HIGH dispatch before F3. Do not preempt already inflight F2.

### TC-CTRL-004 — SMP concurrent submit

Two tasks pinned/directed to different cores submit until pool pressure.

Expected:

- no duplicate slot ownership;
- no corrupted linked queue;
- accepted count == terminal callback count;
- rejected submit owns no context.

### TC-CTRL-005 — Targeted cancel

Queue Web job, State owner token R1 jobs, and Settings job on same device.

Call:

```text
cancel_source(device, STATE, R1)
```

Expected only R1 state jobs terminal CANCELLED.

### TC-CTRL-006 — Queue deadline

Keep device unavailable for dispatch longer than `max_queue_wait_ms`.

Expected terminal `DEADLINE_EXCEEDED`, DCS never receives job, callback once.

### TC-CTRL-007 — Completion mailbox integrity

Complete up to max inflight nearly simultaneously while scheduler worker is temporarily delayed.

Expected no terminal result loss and `completion_mailbox_conflict == 0`.

### TC-CTRL-008 — Dedupe replace semantics

Queued background read A then replacement B with same structured key.

Expected A terminal `SUPERSEDED`, B remains queued/executes. Never apply to user writes.

### TC-CTRL-009 — Lease blocks interleaving

Acquire Settings lease on Device A. Submit matching READ and separate Web HIGH job.

Expected READ dispatch; after ACK lease remains; Web waits until explicit lease release.

### TC-CTRL-010 — Lease does not block other devices

Lease Device A while B/C have runnable jobs.

Expected B/C continue up to global concurrency.


## 5. Acceptance / static analysis / build-hardware gate

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
