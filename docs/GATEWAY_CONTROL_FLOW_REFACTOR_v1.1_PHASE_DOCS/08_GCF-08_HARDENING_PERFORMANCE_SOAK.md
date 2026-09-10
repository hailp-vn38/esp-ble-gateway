# GCF-08 — Hardening, Performance, 9-Device Soak
> Bộ tài liệu này được tách từ `GATEWAY_CONTROL_FLOW_REFACTOR_PLAN_v1.1.md`.
> Mỗi file phase là tài liệu triển khai độc lập và là nguồn checklist của phase tương ứng.
> Khi phase hoàn thành, đánh `[x]` toàn bộ checklist của phase và thêm `✅ DONE (YYYY-MM-DD)` vào heading phase theo rule của `AGENTS.md`.

← Trước: `07_GCF-07_LIFECYCLE_RUNTIME_READINESS.md`

## 0. Phase gate

- **Phase:** `GCF-08`
- **Phụ thuộc:** GCF-07 phải DONE
- **Trạng thái:** `[~] IN PROGRESS` — các phase GCF-05..07 chưa DONE; chỉ
  instrumentation và validation không cần 9 thiết bị đang được thực hiện.
- **Quy tắc hoàn thành:** chỉ chuyển phase tiếp theo khi toàn bộ checklist + test bắt buộc + acceptance của file này PASS.


## 1. Vấn đề cần giải quyết

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


## 2. Thiết kế, file và code contract cần triển khai

## 18. Kconfig / sdkconfig

Không cần tạo Kconfig nếu values ổn định và compile-time internal.

Nếu muốn tunable:

```text
CONFIG_GATEWAY_CTRL_MAX_JOBS=24
CONFIG_GATEWAY_CTRL_MAX_INFLIGHT=4
```

Khuyến nghị phase đầu **không expose quá nhiều Kconfig**.

Giữ constants internal, benchmark trước.

Không sửa generated `sdkconfig`.

---

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
- [x] Plan doc updated (IN PROGRESS; chưa đủ gate để complete).

#### Kết quả validation hiện tại — 2026-09-10

- Root firmware build PASS với ESP-IDF 6.1 / target `esp32s3`.
- Firmware production đã flash lên board `9c:13:9e:aa:fa:bc` qua
  `/dev/cu.usbmodem2101`.
- Boot STA thực tế đi hết chuỗi `dev_cmd_svc_ready` →
  `control_scheduler_ready` → `ds_worker_ready` → `mcp_exposure_ready` →
  `gateway_ready`; không còn lỗi scheduler init kép.
- `/api/status` đã expose counter monotonic `control_plane.scheduler` và
  `control_plane.command_service`; snapshot idle sau boot có
  `dcs_busy=0`, `completion_mailbox_conflict=0`, `urgent_queue_full=0`.
- Save/reboot/reconnect regression trên một thiết bị PASS: WebSocket kết thúc
  `succeeded`, revision `21 → 22`, REST value khớp; diagnostics sau test có
  `reconcile_success=3`, `reconcile_fail=0`, `reconcile_conflict=0`,
  `outcome_unknown=0`, `dcs_busy=0`, `completion_mailbox_conflict=0` và
  `urgent_queue_full=0`.
- Các giá trị idle trên chưa thay thế single/4/9-device stress hoặc soak 2h+;
  các gate đó vẫn để mở.


## 4. Test cases chi tiết

## 22. Detailed test cases

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

### TC-STATE-001 — Full sequential seed

Schema N readable properties -> exactly N sequential reads, no bulk-submit.

### TC-STATE-002 — Revision replacement

R1 seed active, commit R2.

Expected targeted cancel R1, stale R1 completion ignored, R2 starts; Web/Settings jobs untouched.

### TC-STATE-003 — Lightweight schema accessor

Iterate N features using `device_schema_get_feature_at()`.

Expected correct revision guard and no full-snapshot-per-feature path.

### TC-SET-001 — ACK before stream end

READ ACK arrives, no `values_end` yet.

Expected operation remains active and lease remains held.

### TC-SET-002 — Stream end before ACK

Expected operation waits for ACK before success/release.

### TC-SET-003 — Stale callback

Generation 10 cancelled/reused as 11, inject completion 10.

Expected actor ignores stale event without corrupting 11.

### TC-SET-004 — Web cannot interleave after READ ACK

READ ACK arrives while values stream delayed. Queue Web HIGH command same device.

Expected Web command does not dispatch until Settings releases lease.

### TC-SET-005 — Transaction lease

TX_BEGIN -> TX_SET -> TX_COMMIT/reconcile with Web/MCP queued same device.

Expected no non-owner interleave inside defined transaction lease scope.

### TC-WEB-001 — Slow client isolation

Delay Web response handling while unrelated BLE ACKs occur.

Expected DCS/scheduler continue normally.

### TC-WEB-002 — Async context exact-once

Test synchronous scheduler reject, accepted+OK, accepted+cancel, accepted+deadline.

Expected context/request completed/freed exactly once for each path.

### TC-MCP-001 — Responder lifetime

Same terminal matrix for cloned MCP responder including notification calls.

Expected responder release exactly once.

### TC-LIFE-001 — Delete/re-add

Load schema/settings/state, delete, re-add same id/revision.

Expected no stale runtime snapshot.

### TC-LIFE-002 — Delete inflight + late ACK

Quiesce/cancel device then inject late ACK from old request.

Expected unmatched/ignored safely; no callback into forgotten state; no double terminal.

### TC-LIFE-003 — Delete failure rollback

Fail a purge/store step while inventory remains.

Expected device scheduler block is released and lifecycle reports degraded/error deterministically.

### TC-BOOT-001 — Partial init failure

Mock mandatory runtime init failure.

Expected ERROR, reverse-order deinit, never READY.

### TC-BOOT-002 — OTA finalize failure

Mock `gateway_ota_finalize()` failure/rollback path after all modules start.

Expected READY never emitted.

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

#### After GCF-04 — no bulk state submit

```sh
git grep "device_command_service_submit" -- components/device_state
git grep "device_schema_get(" -- components/device_state/device_state_seed.c
```

Expected direct DCS submit = none; seed must use scheduler and lightweight accessor rather than full snapshot each feature.

#### After GCF-05 — Settings actor ownership

```sh
git grep "s_active_op" -- components/device_settings
```

Review all writes: only actor worker execution path mutates state. Callback/timer/BLE entry points only post events.

Also:

```sh
git grep "device_command_service_submit" -- components/device_settings
```

Expected none.

#### After GCF-06 — scheduler is production command entry point

```sh
git grep "device_command_service_submit(" -- components main
```

Expected production use only under scheduler/DCS compatibility internals; no Web/MCP/State/Settings/main business caller.

#### Completion context audit

```sh
git grep "completion" -- components/web_server components/mcp_endpoint components/device_settings
```

Review:

- no blocking presentation work in DCS callback;
- accepted async context has exactly-one terminal cleanup;
- scheduler/DCS callbacks do not mutate Settings actor state.

#### READY ordering audit

```sh
git grep "BOARD_STATUS_READY" -- main components
```

Expected STA READY emitted only after successful `gateway_ota_finalize()` path in runtime orchestration.

#### Build graph acceptance

Root/test `idf.py reconfigure` + build must pass with `MINIMAL_BUILD ON`; adapter component must be reachable from main dependency chain.

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

## 25. Hardware validation sequence

Recommended order sau mỗi phase có thay transport/control flow:

1. flash gateway;
2. boot STA mode;
3. connect 1 BLE device;
4. verify schema discovery;
5. verify Settings describe/read;
6. verify complete state seed;
7. REST read/write feature;
8. MCP read/write feature;
9. disconnect/reconnect;
10. delete/re-add;
11. repeat với 3-4 devices;
12. cuối cùng test 9 links.

Không bắt đầu 9-device debug trước khi single-device invariants pass.

---

## 26. Rollback strategy

Mỗi phase phải build/test độc lập.

Không gộp các thay đổi sau vào một commit lớn:

```text
DCS urgent split
+ scheduler
+ settings actor
+ Web migration
+ MCP migration
```

Recommended commit boundaries:

```text
refactor(types): extract shared device command types
refactor(dcs): isolate urgent ack path and wake mechanism
feat(control): add bounded scheduler with lease/cancel/deadline
refactor(schema): add scheduler adapter without dependency cycle
refactor(state): make state seeding sequential
refactor(settings): make worker sole state owner
refactor(web): route device control through scheduler
refactor(mcp): route tool control through scheduler
fix(lifecycle): purge runtime state on device delete
fix(boot): gate ready on gateway initialization
chore(control): hardening metrics and soak fixes
```

Nếu một phase gây regression, revert phase đó không làm mất các phase trước.

---

## 27. Definition of Done toàn bộ refactor

### Architecture

- [ ] `device_command_types` owns shared request/result/status types.
- [ ] Production business modules không direct-submit DCS.
- [ ] `device_schema` không depend scheduler; adapter component nối hai layer.
- [ ] Scheduler là outbound control-plane entry point.
- [ ] DCS vẫn sole ACK owner.
- [ ] Transport inflight <= 1/device.
- [ ] Global inflight bounded và compile-time compatible DCS pending capacity.
- [ ] Logical device lease tồn tại cho Settings critical flows.
- [ ] ACK/lifecycle có urgent compact path + explicit worker wake.
- [ ] Settings worker là sole state-machine owner.
- [ ] State seed sequential/background + lightweight schema accessor.
- [ ] Web/MCP presentation work không chạy DCS/scheduler critical context.

### Correctness

- [ ] Every accepted scheduler job terminal exactly once.
- [ ] Synchronous reject transfers no context ownership.
- [ ] Targeted cancel không ảnh hưởng unrelated source/job.
- [ ] Dedupe replace terminal old job `SUPERSEDED`.
- [ ] Queue deadline prevents stale user requests dispatching indefinitely.
- [ ] Settings READ success requires ACK + values_end while lease held.
- [ ] Settings transaction lease prevents forbidden interleave.
- [ ] Full state seed hoàn tất multi-feature device.
- [ ] Delete/re-add không stale state/settings/schema.
- [ ] Late ACK after delete/cancel safe.
- [ ] No cross-device request/ACK mismatch.

### Lifecycle/Readiness

- [ ] Delete blocks + quiesces before runtime purge.
- [ ] Delete failure unblocks surviving device.
- [ ] `gateway_runtime` reverse-order rollback implemented.
- [ ] Local MCP endpoint thuộc mandatory READY gate.
- [ ] OTA finalize chạy trước READY.
- [ ] Partial init/OTA failure never emits READY.
- [ ] Provisioning mode vẫn defer gateway runtime.

### Reliability

- [ ] `DCS_urgent_queue_full == 0` trong soak.
- [ ] `scheduler_completion_mailbox_conflict == 0`.
- [ ] `scheduler_dcs_busy == 0` steady-state.
- [ ] No stuck lease.
- [ ] No WDT/deadlock.
- [ ] No progressive heap leak/fragmentation trend bất thường.

### Performance

- [ ] HIGH command latency bounded dưới state seed/background load.
- [ ] 4 simultaneous devices tận dụng bounded concurrency.
- [ ] Active lease Device A không block B/C/D.
- [ ] 9 BLE links reconnect không tạo command storm.
- [ ] Wi-Fi/WebSocket load không gây ACK timeout bất thường.

### Build/Test

- [ ] Root `idf.py reconfigure && idf.py build` pass.
- [ ] Test project reconfigure/build pass.
- [ ] No CMake dependency cycle.
- [ ] Unity tests pass ESP32-S3.
- [ ] Physical BLE integration pass.
- [ ] 2h+ development soak pass.
- [ ] Release soak target pass trước production.

### Documentation

- [ ] Tất cả phase marked DONE đúng repo rule.
- [ ] `docs/GATEWAY_FLOW_REPORT.md` phản ánh scheduler + lease architecture.
- [ ] Comments cũ về listener ordering/reserve slot được xóa/sửa.
- [ ] README architecture updated nếu cần.

## 28. File-by-file implementation summary

| File | Action | Nội dung chính |
|---|---|---|
| `components/device_command_types/CMakeLists.txt` | ADD | shared command type component |
| `components/device_command_types/include/device_command_types.h` | ADD | request/result/status/origin/settings payload types |
| `components/device_control_scheduler/CMakeLists.txt` | ADD | scheduler sources/dependencies |
| `components/device_control_scheduler/include/device_control_scheduler.h` | ADD | submit/cancel/lease/deadline/stats API |
| `components/device_control_scheduler/device_control_scheduler.c` | ADD | init/deinit/public API + submit ownership |
| `components/device_control_scheduler/device_control_scheduler_worker.c` | ADD | arbitration/dispatch/deadline/completion |
| `components/device_control_scheduler/device_control_scheduler_queue.c` | ADD | SMP-safe pool/per-device priority queues/dedupe |
| `components/device_control_scheduler/device_control_scheduler_lease.c` | ADD | logical device reservation/block/quiesce |
| `components/device_control_scheduler/device_control_scheduler_internal.h` | ADD | slot state/generation/constants |
| `components/device_control_scheduler/test/test_device_control_scheduler.c` | ADD | scheduler/lease/SMP Unity tests |
| `components/device_schema_control_adapter/CMakeLists.txt` | ADD | adapter dependencies without cycle |
| `components/device_schema_control_adapter/include/device_schema_control_adapter.h` | ADD | adapter init/deinit API |
| `components/device_schema_control_adapter/device_schema_control_adapter.c` | ADD | schema submitter -> scheduler bridge |
| `components/device_command_service/device_command_urgent.c` | ADD | compact urgent event helpers |
| `components/device_command_service/device_command_result.c` | ADD/TRANSITIONAL | isolate legacy completion only if needed |
| `components/device_command_service/device_command_service.c` | EDIT | dual queue + notify wake init/public API |
| `components/device_command_service/device_command_worker.c` | EDIT | urgent-first notified loop + timeout |
| `components/device_command_service/device_command_pending.c` | EDIT | exact-once terminal/cancel safety |
| `components/device_command_service/device_command_request.c` | EDIT | shared types include + connection policy review |
| `components/device_command_service/device_command_service_internal.h` | EDIT | compact urgent structs/constants |
| `components/device_command_service/include/device_command_service.h` | EDIT | service-only API/stats + public pending-capacity macro; include shared types |
| `components/device_command_service/CMakeLists.txt` | EDIT | add command types; retain schema validation dep |
| `components/device_state/device_state.c` | EDIT | remove bulk seed; cache only |
| `components/device_state/device_state_seed.c` | ADD | sequential session/owner token/targeted cancel |
| `components/device_state/device_state_internal.h` | ADD | private seed/cache contracts |
| `components/device_state/CMakeLists.txt` | EDIT | scheduler/shared types dependency |
| `components/device_schema/include/device_schema.h` | EDIT | lightweight feature accessor API |
| `components/device_schema/*` | EDIT | implement accessor; **no scheduler dependency** |
| `components/device_settings/device_settings_internal.h` | ADD | actor + lease state contract |
| `components/device_settings/device_settings_events.c` | ADD | bounded actor events |
| `components/device_settings/device_settings_worker.c` | EDIT | sole owner; lease lifecycle; event transitions |
| `components/device_settings/device_settings_operation.c` | EDIT | generation/owner-token actor ownership |
| `components/device_settings/device_settings_transaction.c` | EDIT | scheduler + matching lease jobs |
| `components/device_settings/device_settings.c` | EDIT | lifecycle bridge/forget API |
| `components/device_settings/device_settings_memory.c` | EDIT | destructive purge helpers |
| `components/device_settings/include/device_settings.h` | EDIT | forget + ownership docs |
| `components/web_server/web_command_api.c` | EDIT | scheduler submit/deadline + HTTP work completion |
| `components/mcp_endpoint/mcp_core.c` | EDIT | scheduler submit/deadline + responder completion bridge |
| `components/device_management/device_management.c` | EDIT | block/quiesce/purge/unblock rollback |
| `components/device_management/device_management_internal.h` | EDIT | scheduler/settings lifecycle hooks |
| `main/gateway_runtime.c` | ADD/REQUIRED | runtime init/deinit/rollback/readiness owner |
| `main/gateway_runtime.h` | ADD/REQUIRED | runtime API |
| `main/main.c` | EDIT | base boot only; remove schema bridge; READY after finalize |
| `main/CMakeLists.txt` | EDIT | runtime/scheduler/adapter/shared types dependencies |
| `test/CMakeLists.txt` | EDIT | include new test components |
| `docs/GATEWAY_FLOW_REPORT.md` | EDIT/LATE | final architecture report |

## 29. Recommended implementation order summary

```text
GCF-01  Shared command types + DCS urgent ACK/wake
   ↓
GCF-02  Control Scheduler + lease/cancel/deadline
   ↓
GCF-03  Schema Control Adapter; prove no dependency cycle
   ↓
GCF-04  Sequential State Seed + targeted cancel
   ↓
GCF-05  Settings single-owner actor + logical lease
   ↓
GCF-06  Web + MCP migration
   ↓
GCF-07  Lifecycle quiesce + gateway runtime/readiness
   ↓
GCF-08  Hardening + 9-device soak + docs
```

Không đổi thứ tự GCF-01/GCF-02/GCF-03:

- shared types và ACK safety trước;
- scheduler contract hoàn chỉnh trước caller migration;
- chứng minh dependency graph trước khi State/Settings phụ thuộc scheduler rộng hơn.

Không migrate Settings trước khi lease đã có test pass.

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


## 6. Checklist đóng phase

- [ ] Tất cả file **ADD** trong phase đã được thêm vào CMake dependency graph.
- [ ] Tất cả file **EDIT** trong phase đã được cập nhật đúng contract mới.
- [ ] Không còn caller/flow legacy bị phase này yêu cầu loại bỏ.
- [ ] Unit test của phase PASS trên test project.
- [x] Firmware root project build PASS cho `esp32s3`.
- [ ] Không xuất hiện warning mới liên quan ownership, queue, task stack hoặc lifetime.
- [ ] Static grep/acceptance của phase PASS.
- [ ] Hardware test bắt buộc của phase PASS nếu phase yêu cầu BLE/Wi-Fi thực.
- [ ] Memory checkpoint không regression ngoài budget đã định nghĩa.
- [ ] Checklist chi tiết trong phần Implementation plan đã được đánh `[x]` toàn bộ.
- [ ] Heading phase được cập nhật `✅ DONE (YYYY-MM-DD)` trước khi bắt đầu phase kế tiếp.
