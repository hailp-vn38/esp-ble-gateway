# Gateway Control Flow Refactor v1.1 — Overview & Phase Map
> Bộ tài liệu này được tách từ `GATEWAY_CONTROL_FLOW_REFACTOR_PLAN_v1.1.md`.
> Mỗi file phase là tài liệu triển khai độc lập và là nguồn checklist của phase tương ứng.
> Khi phase hoàn thành, đánh `[x]` toàn bộ checklist của phase và thêm `✅ DONE (YYYY-MM-DD)` vào heading phase theo rule của `AGENTS.md`.

Tiếp: `01_GCF-01_SHARED_TYPES_DCS_URGENT_PATH.md` →

## 1. Revision, mục tiêu và phạm vi

### Revision v1.1

Bản v1.1 cập nhật sau architecture review của v1.0. Các thay đổi bắt buộc trước implementation:

1. loại dependency cycle `device_schema -> scheduler -> DCS -> device_schema`;
2. tách shared command types khỏi `device_command_service`;
3. thêm logical **device lease/reservation** cho Settings stream/transaction;
4. định nghĩa scheduler fixed-pool ownership an toàn khi submit đồng thời từ hai core;
5. bổ sung exact-once completion contract và context lifetime;
6. bổ sung targeted cancel theo `job_id/source/owner_token`;
7. bổ sung queue-wait deadline cho request có latency contract;
8. DCS urgent queue dùng compact event + explicit task wake-up;
9. dedupe chỉ áp dụng an toàn cho idempotent/background jobs;
10. Gateway READY chỉ được set **sau OTA finalize thành công**;
11. `gateway_runtime.c` trở thành required orchestration layer và phải rollback reverse-order khi partial init fail;
12. bổ sung test concurrency, lease, cancellation, deadline, late ACK, OTA readiness và dependency-cycle.

Không thay đổi Gateway Protocol v4 hoặc BLE wire format.

---

### 1. Mục tiêu

Refactor control-plane theo kiến trúc v1.1:

```text
 REST ───────────┐
 MCP LAN ────────┤
 Xiaozhi MCP ────┤
 Settings Actor ─┤
 State Seed ─────┤
                 │
 Device Schema ──┼─ submitter interface ─► Schema Control Adapter
                 │                            │
                 │                            └──────────────┐
                 ▼                                           ▼
       ┌──────────────────────────────────────────────────────────┐
       │ DEVICE CONTROL SCHEDULER                                 │
       │                                                          │
       │ per-device priority/fairness                             │
       │ logical device lease                                     │
       │ targeted cancel / queue deadline / safe dedupe           │
       │ transport inflight/device = 1                            │
       │ global transport inflight <= 4                           │
       │ bounded SMP-safe fixed pool                              │
       └────────────────────────────┬─────────────────────────────┘
                                    │ shared device command types
                                    ▼
                         ┌───────────────────────┐
                         │ DEVICE COMMAND        │
                         │ SERVICE (DCS)         │
                         │                       │
                         │ validate/encode/send  │
                         │ request_id/pending    │
                         │ sole ACK ownership    │
                         │ compact urgent path   │
                         │ timeout/cancel        │
                         └──────────┬────────────┘
                                    │
                                    ▼
                                   BLE
```

Response/data flow:

```text
BLE notify worker
    │
    ├── ACK ───────────────► DCS urgent queue + task wake
    │                            │
    │                            ▼
    │                      scheduler mailbox
    │                            │
    │             ┌──────────────┼──────────────┐
    │             ▼              ▼              ▼
    │        Settings actor   State seed     Schema adapter
    │
    ├── schema messages ─────► Device Schema
    ├── settings messages ───► Settings protocol/memory ─► actor event
    └── feature_state ───────► State Cache ─► Gateway Events ─► WebSocket
```

#### 1.1 Mục tiêu kỹ thuật

1. Không còn submit flood vào DCS.
2. Không drop ACK do normal submit queue bị đầy hoặc worker ngủ sai queue.
3. Một device chỉ có tối đa một **transport command inflight**.
4. Global transport inflight bounded, mặc định 4.
5. Phân biệt transport ACK với logical operation completion bằng device lease.
6. User command không bị state seed/background chiếm dispatch vô hạn.
7. Settings state machine có đúng một owner.
8. Every accepted scheduler job có terminal completion exactly once.
9. Targeted cancel không phá workload khác cùng device.
10. User-facing request có queue-wait deadline, không chờ vô hạn trước DCS.
11. DCS không chạy JSON/HTTP/MCP presentation work.
12. Delete device quiesce command/lease trước khi purge runtime state.
13. `BOARD_STATUS_READY` chỉ xuất hiện sau mandatory runtime init + OTA finalize thành công.
14. Dependency graph không cycle; `device_schema` không phụ thuộc scheduler.
15. Queue/pool bounded, có metrics/high-water, tối ưu internal RAM.
16. Giữ Gateway Protocol v4 và BLE wire contract.
17. REST snapshot vẫn authoritative; WebSocket vẫn là realtime delta/invalidation.

#### 1.2 Không thuộc scope

- Không đổi CBOR key hoặc Protocol v4.
- Không đổi UUID BLE service/characteristic.
- Không tăng `CONFIG_BT_NIMBLE_MAX_CONNECTIONS` quá giá trị hiện tại.
- Không viết lại BLE Central nếu không có bug mới.
- Không rewrite Web UI.
- Không tăng DCS pending từ 4 lên 9 chỉ để né BUSY.
- Không mở schema/settings concurrency nhiều stream trước khi scheduler/lease soak ổn định.
- Không chuyển toàn bộ cấu trúc sang heap/PSRAM.


## 2. Problem map

### 2. Các vấn đề hiện tại cần giải quyết

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

#### 2.2 ACK và SUBMIT dùng chung một control queue

DCS hiện dùng bounded queue cho nhiều loại event. Khi queue chứa nhiều submit event, ACK có thể không enqueue được.

ACK là transport-critical event nên không được cạnh tranh queue với workload thông thường.

#### 2.3 Settings mutable state bị truy cập từ nhiều execution context

`device_settings_worker.c` có active operation state nhưng completion callback từ DCS có thể thay đổi state này ngoài worker context.

Trên ESP32-S3 dual-core, đây là race condition tiềm ẩn.

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


## 3. Kiến trúc đích và invariant

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


## 4. File/component map

### 5. Cấu trúc file đích

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

#### 5.3 Component mới: `device_schema_control_adapter`

Schema hiện đã có `device_schema_set_submitter()`. Giữ abstraction này và đặt scheduler adapter bên ngoài schema.

ADD:

```text
components/device_schema_control_adapter/
├── CMakeLists.txt
├── include/
│   └── device_schema_control_adapter.h
└── device_schema_control_adapter.c
```

Dependency:

```text
device_schema_control_adapter
    ├── device_schema
    └── device_control_scheduler
```

`device_schema` **không** thêm dependency scheduler.

#### 5.4 Device State

ADD:

```text
components/device_state/device_state_seed.c
components/device_state/device_state_internal.h
```

EDIT:

```text
components/device_state/device_state.c
components/device_state/CMakeLists.txt
components/device_state/include/device_state.h
components/device_state/test/*
components/device_schema/include/device_schema.h
components/device_schema/device_schema*.c   # lightweight feature accessor
```

#### 5.5 Device Settings

ADD:

```text
components/device_settings/device_settings_internal.h
components/device_settings/device_settings_events.c
components/device_settings/test/test_device_settings_actor.c
```

EDIT:

```text
components/device_settings/device_settings.c
components/device_settings/device_settings_worker.c
components/device_settings/device_settings_operation.c
components/device_settings/device_settings_transaction.c
components/device_settings/device_settings_memory.c
components/device_settings/include/device_settings.h
components/device_settings/CMakeLists.txt
```

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

#### 5.8 Device lifecycle

EDIT:

```text
components/device_management/device_management.c
components/device_management/device_management_internal.h
components/device_management/include/device_management.h
components/device_management/CMakeLists.txt
components/device_management/test/*
```

#### 5.9 Main/runtime wiring

`gateway_runtime` là **required** trong v1.1.

ADD:

```text
main/gateway_runtime.c
main/gateway_runtime.h
```

EDIT:

```text
main/main.c
main/CMakeLists.txt
```

Runtime layer chịu trách nhiệm init, rollback reverse-order, readiness và deinit.

#### 5.10 Tests project

EDIT:

```text
test/CMakeLists.txt
```

Thêm component test mới vào `TEST_COMPONENTS` do project dùng `MINIMAL_BUILD ON`.

#### 5.11 Documentation

ADD:

```text
docs/GATEWAY_CONTROL_FLOW_REFACTOR_PLAN_v1.1.md
```

EDIT cuối migration:

```text
docs/GATEWAY_FLOW_REPORT.md
README.md
```

---


## 5. Thứ tự phase

| Thứ tự | Phase | File | Điều kiện đầu ra chính |
|---:|---|---|---|
| 1 | GCF-01 | `01_GCF-01_SHARED_TYPES_DCS_URGENT_PATH.md` | Shared command types; ACK/lifecycle không thể bị normal submit làm nghẽn |
| 2 | GCF-02 | `02_GCF-02_DEVICE_CONTROL_SCHEDULER.md` | Scheduler bounded, lease, deadline, targeted cancel, exact-once completion |
| 3 | GCF-03 | `03_GCF-03_SCHEMA_CONTROL_ADAPTER.md` | Schema đi qua adapter; dependency graph không cycle |
| 4 | GCF-04 | `04_GCF-04_SEQUENTIAL_STATE_SEED.md` | State seed tuần tự, background, cancel theo generation/session |
| 5 | GCF-05 | `05_GCF-05_SETTINGS_SINGLE_OWNER_ACTOR.md` | Settings FSM single-owner + logical device lease |
| 6 | GCF-06 | `06_GCF-06_WEB_MCP_MIGRATION.md` | Web/MCP không submit trực tiếp DCS; network work ngoài transport context |
| 7 | GCF-07 | `07_GCF-07_LIFECYCLE_RUNTIME_READINESS.md` | Delete quiesce/purge đầy đủ; init rollback; OTA finalize trước READY |
| 8 | GCF-08 | `08_GCF-08_HARDENING_PERFORMANCE_SOAK.md` | Regression + 9-device soak + final DoD |


## 6. Phase plan gốc

## 21. PHASE PLAN

Theo rule `AGENTS.md`, mỗi phase sau khi hoàn thành phải:

1. đánh `[x]` toàn bộ checklist phase;
2. đổi heading phase thành `✅ DONE (YYYY-MM-DD)`;
3. commit update plan doc trước phase tiếp theo;
4. nếu có plan doc liên quan khác, cập nhật đồng bộ.

---

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

- [ ] Fixed pool SMP-safe.
- [ ] Exact-once contract implemented.
- [ ] One inflight/device enforced.
- [ ] Global inflight <= 4 enforced and static-asserted against DCS public capacity.
- [ ] Scheduler device table capacity covers current 16-entry Device Store inventory.
- [ ] Lease implemented/tested.
- [ ] Targeted cancel implemented/tested.
- [ ] Queue deadline implemented/tested.
- [ ] Dedupe semantics documented/tested.
- [ ] No long callback under scheduler lock.
- [ ] Root/test builds pass.
- [ ] Plan doc updated.

---

### GCF-03 — Schema Control Adapter; no dependency cycle

#### Mục tiêu

Route schema discovery qua scheduler mà `device_schema` không depend scheduler.

#### ADD

```text
components/device_schema_control_adapter/CMakeLists.txt
components/device_schema_control_adapter/include/device_schema_control_adapter.h
components/device_schema_control_adapter/device_schema_control_adapter.c
```

#### EDIT

```text
main/main.c
main/CMakeLists.txt
test/CMakeLists.txt
```

#### Implementation

- giữ `device_schema_set_submitter()` API;
- adapter register submitter;
- map schema request -> scheduler NORMAL;
- bounded queue deadline;
- map result về schema submit result;
- remove `capability_submit_bridge_t/capability_submit()` khỏi main;
- **không edit `device_schema/CMakeLists.txt` để add scheduler**.

#### Test

1. schema discovery success;
2. scheduler queue full/deadline mapping;
3. timeout/not-connected/rejected mapping;
4. reconnect schema flow;
5. `idf.py reconfigure/build` không dependency cycle;
6. existing schema tests vẫn dùng mock submitter được.

#### Checklist

- [ ] Adapter component added.
- [ ] `device_schema` has no scheduler dependency.
- [ ] Main schema bridge removed.
- [ ] Schema requests pass scheduler.
- [ ] Dependency graph build pass.
- [ ] Existing schema mock submitter tests preserved.
- [ ] Plan doc updated.

---

### GCF-04 — Sequential State Seed + targeted cancellation

#### Mục tiêu

Loại bulk-submit và full snapshot copy lặp.

#### ADD

```text
components/device_state/device_state_seed.c
components/device_state/device_state_internal.h
```

#### EDIT

```text
components/device_state/device_state.c
components/device_state/include/device_state.h
components/device_state/CMakeLists.txt
components/device_state/test/*
components/device_schema/include/device_schema.h
components/device_schema/device_schema*.c
```

#### Implementation

- remove seed loop direct DCS submit;
- add lightweight `device_schema_get_feature_at()`;
- one active seed job/session step;
- BACKGROUND priority;
- owner token/generation by schema revision;
- `cancel_source(STATE, old_token)` on revision replacement;
- stale completion ignored;
- structured dedupe equality.

#### Test

1. N-feature schema -> N sequential reads;
2. HIGH user job queued before next dispatch runs before remaining seed;
3. R1 seed replaced by R2 without cancelling Web/Settings;
4. disconnect mid-seed;
5. duplicate feature read dedupe;
6. accessor revision mismatch;
7. no O(N²) full snapshot copy path.

#### Checklist

- [ ] Bulk seed removed.
- [ ] Lightweight schema accessor added.
- [ ] Sequential seed pass.
- [ ] Targeted cancel used, not cancel_device.
- [ ] Revision/generation stale completion guard pass.
- [ ] Background priority pass.
- [ ] State cache behavior preserved.
- [ ] Plan doc updated.

---

### GCF-05 — Settings single-owner actor + logical lease

#### Mục tiêu

Loại cross-context mutation và bảo vệ logical Settings stream/transaction khỏi interleaving.

#### ADD

```text
components/device_settings/device_settings_internal.h
components/device_settings/device_settings_events.c
components/device_settings/test/test_device_settings_actor.c
```

#### EDIT

```text
components/device_settings/device_settings.c
components/device_settings/device_settings_worker.c
components/device_settings/device_settings_operation.c
components/device_settings/device_settings_transaction.c
components/device_settings/device_settings_memory.c
components/device_settings/include/device_settings.h
components/device_settings/CMakeLists.txt
components/device_settings/test/*
```

#### Implementation

- Settings worker sole state-machine owner;
- DCS/scheduler/timer/BLE callbacks post events only;
- actor acquires lease trước READ/DESCRIBE critical stream;
- giữ lease sau ACK tới `values_end`;
- transaction giữ lease qua TX critical section/reconcile;
- generation + owner token stale guard;
- add `device_settings_forget()`;
- disconnect/cancel revoke logical operation cleanly.

#### Test

1. READ ACK before values_end -> lease remains, Web job cannot interleave;
2. values_end before ACK -> operation waits;
3. transaction lease prevents Web/MCP interleave;
4. lease release dispatches queued HIGH job;
5. stale callback generation ignored;
6. disconnect during lease/transaction;
7. forget clears snapshots/runtime;
8. actor event burst does not race `s_active_op`.

#### Checklist

- [ ] Cross-context `s_active_op` mutation removed.
- [ ] Actor events bounded.
- [ ] READ uses logical lease.
- [ ] Transaction/reconcile lease semantics implemented.
- [ ] ACK + values_end correctness pass.
- [ ] Stale generation guard pass.
- [ ] `device_settings_forget()` pass.
- [ ] No direct DCS submit in Settings.
- [ ] Plan doc updated.

---

### GCF-06 — Migrate Web + MCP callers

#### Mục tiêu

Business control chỉ qua scheduler và presentation work chạy đúng execution context.

#### EDIT

```text
components/web_server/web_command_api.c
components/web_server/CMakeLists.txt
components/web_server/test/*
components/mcp_endpoint/mcp_core.c
components/mcp_endpoint/CMakeLists.txt
components/mcp_endpoint/test/*
```

#### Implementation

- Web/MCP include shared command types + scheduler, không service submit;
- HIGH priority;
- user queue deadline bounded;
- exact-once async context/responder ownership;
- scheduler completion bridge -> HTTP/MCP worker;
- queue full/deadline/cancel mapping preserving public contract;
- remove transitional result dispatcher nếu không còn legacy consumer cần.

#### Test

1. Web/MCP concurrent commands;
2. slow HTTP client không block ACK/scheduler;
3. scheduler submit reject cleans context synchronously;
4. accepted job cancelled -> context terminal/free exactly once;
5. queue deadline -> deterministic error;
6. MCP notification cleanup;
7. Xiaozhi/local MCP response work không chạy DCS context.

#### Checklist

- [ ] Web direct DCS submit removed.
- [ ] MCP direct DCS submit removed.
- [ ] Async context exact-once lifetime tests pass.
- [ ] Queue deadline/backpressure mapped.
- [ ] JSON/HTTP/MCP work isolated.
- [ ] `git grep device_command_service_submit` acceptance pass.
- [ ] Plan doc updated.

---

### GCF-07 — Lifecycle cleanup + runtime orchestration + readiness

#### Mục tiêu

Delete quiesce an toàn và boot runtime có rollback/readiness đúng.

#### ADD

```text
main/gateway_runtime.c
main/gateway_runtime.h
```

#### EDIT

```text
components/device_management/*
main/main.c
main/CMakeLists.txt
components/device_management/test/*
```

#### Implementation

- block + quiesce scheduler on delete;
- cancel queued/inflight and revoke lease;
- settings/schema/state purge sau quiesce;
- failure rollback/unblock nếu inventory vẫn còn;
- runtime init started-flags + reverse rollback;
- local MCP endpoint mandatory;
- external bridge optional/degraded;
- OTA finalize **trước READY**;
- init failure -> ERROR + deinit started modules.

#### Test

1. delete idle/queued/inflight;
2. delete during Settings lease/transaction;
3. delete + late ACK -> no use-after-forget/double callback;
4. delete failure -> scheduler unblocked if device remains;
5. delete/re-add same id/revision no stale data;
6. Wi-Fi connected + BLE/Web/MCP init fail -> never READY;
7. OTA finalize fail -> READY never set;
8. normal STA boot -> READY exactly once after finalize;
9. provisioning mode does not init gateway runtime.

#### Checklist

- [ ] Device quiesce before purge.
- [ ] Late ACK lifecycle safe.
- [ ] Failure unblocks surviving device.
- [ ] Settings/schema/state purge complete.
- [ ] Runtime rollback reverse-order implemented.
- [ ] OTA finalize precedes READY.
- [ ] Local MCP included in ready gate.
- [ ] Provisioning regression pass.
- [ ] Plan doc updated.

---

### GCF-08 — Hardening, performance, 9-device soak

#### Mục tiêu

Không thêm feature; chỉ instrumentation, sizing, race/deadlock và documentation.

#### Test matrix

##### Single device

- 1000 sequential commands;
- state seed + user writes;
- Settings lease/refresh/transaction;
- reconnect + late ACK/cancel.

##### 4 devices

- saturate max inflight=4;
- mixed priorities;
- no starvation;
- one Settings lease while other devices remain concurrent.

##### 9 devices

- all links connected;
- reconnect/schema storm;
- state seed all devices;
- random Web/MCP controls;
- Wi-Fi/WebSocket traffic active;
- delete/re-add one device while others operate.

##### Soak

Development minimum:

```text
2h
```

Release candidate:

```text
8-24h
```

Metrics:

```text
heap_internal_free/largest
psram_free/largest
scheduler_max_queued
scheduler_max_inflight
scheduler_queue_full
scheduler_deadline_exceeded
scheduler_dcs_busy
scheduler_max_active_leases
scheduler_completion_mailbox_conflict
DCS_urgent_queue_full
DCS_ack_unmatched/duplicate
DCS_timeout_count
BLE_disconnect_count
WS_resync_required
```

#### Acceptance

- `DCS_urgent_queue_full == 0`;
- `scheduler_completion_mailbox_conflict == 0`;
- `scheduler_dcs_busy == 0` steady-state;
- no stuck lease;
- no duplicate/missing terminal callback;
- no state seed BUSY flood;
- no WDT/deadlock;
- no progressive heap leak;
- user HIGH latency bounded under background load;
- 9-device reconnect does not create command storm.

#### Checklist

- [ ] Single-device stress pass.
- [ ] 4-device mixed concurrency pass.
- [ ] 9-device reconnect/state-seed pass.
- [ ] Settings lease soak pass.
- [ ] Delete/late-ACK soak pass.
- [ ] Wi-Fi + BLE coexistence pass.
- [ ] Heap/fragmentation trend reviewed.
- [ ] Queue/lease high-water reviewed.
- [ ] No urgent overflow.
- [ ] No completion mailbox conflict.
- [ ] No persistent DCS BUSY.
- [ ] `docs/GATEWAY_FLOW_REPORT.md` updated.
- [ ] README updated if needed.
- [ ] Plan doc marked complete.


## 7. Kiến trúc cuối cùng kỳ vọng

## 30. Kiến trúc cuối cùng kỳ vọng

```text
                   ┌───────────────┐
                   │ Web REST/UI   │
                   └──────┬────────┘
                          │ HIGH + deadline
                   ┌──────▼────────┐
                   │ MCP / Xiaozhi │
                   └──────┬────────┘
                          │ HIGH + deadline
                          │
 Schema submitter ─► Schema Control Adapter ── NORMAL ─┐
 Settings Actor ── HIGH/NORMAL + optional lease ──────┤
 State Seed ────── BACKGROUND + owner token ──────────┤
                                                      ▼
                                           ┌──────────────────────┐
                                           │ Device Control       │
                                           │ Scheduler            │
                                           │                      │
                                           │ 1 transport/device   │
                                           │ max 4 global         │
                                           │ priority + fairness  │
                                           │ logical device lease │
                                           │ deadline/cancel      │
                                           │ bounded fixed pool   │
                                           └──────────┬───────────┘
                                                      │ shared command types
                                                      ▼
                                           ┌──────────────────────┐
                                           │ Device Command       │
                                           │ Service              │
                                           │                      │
                                           │ validation/encode    │
                                           │ request_id/pending   │
                                           │ ACK sole owner       │
                                           │ compact urgent path  │
                                           │ task-notify wake     │
                                           │ timeout/cancel       │
                                           └──────────┬───────────┘
                                                      │
                                                      ▼
                                                 BLE Central
                                                      │
                                                      ▼
                                                 BLE Devices

BLE notifications
       │
       ├── feature_state ─► State Cache ─► Gateway Events ─► WebSocket
       ├── schema ─────────► Device Schema
       ├── settings ───────► Settings protocol/memory ─► Settings Actor event
       └── ACK ────────────► DCS compact urgent event
```

Dependency shape quan trọng:

```text
                    device_command_types
                      ▲        ▲
                      │        │
Scheduler ─────────── DCS ───► device_schema (validation)
   ▲
   │
Schema Control Adapter
   ▲
   │
device_schema submitter interface
```

Không có edge `device_schema -> scheduler`, nên không tạo cycle.

Logical lease bảo đảm transport ACK không bị hiểu sai thành logical Settings completion. Exact-once lifecycle, targeted cancel và queue deadline giúp scheduler trở thành backpressure layer thực sự thay vì chỉ là queue trung gian.
