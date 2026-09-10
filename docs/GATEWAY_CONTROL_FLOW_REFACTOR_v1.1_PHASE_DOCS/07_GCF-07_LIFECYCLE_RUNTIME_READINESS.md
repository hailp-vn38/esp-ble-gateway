# GCF-07 — Lifecycle Cleanup + Runtime Orchestration + Readiness
> Bộ tài liệu này được tách từ `GATEWAY_CONTROL_FLOW_REFACTOR_PLAN_v1.1.md`.
> Mỗi file phase là tài liệu triển khai độc lập và là nguồn checklist của phase tương ứng.
> Khi phase hoàn thành, đánh `[x]` toàn bộ checklist của phase và thêm `✅ DONE (YYYY-MM-DD)` vào heading phase theo rule của `AGENTS.md`.

← Trước: `06_GCF-06_WEB_MCP_MIGRATION.md` | Tiếp: `08_GCF-08_HARDENING_PERFORMANCE_SOAK.md` →

## 0. Phase gate

- **Phase:** `GCF-07`
- **Phụ thuộc:** GCF-06 phải DONE
- **Trạng thái:** `[~] IN PROGRESS` — GCF-06 vẫn chưa DONE; phần lifecycle đã
  được triển khai và build/flash test, còn runtime owner/rollback và HIL.
- **Quy tắc hoàn thành:** chỉ chuyển phase tiếp theo khi toàn bộ checklist + test bắt buộc + acceptance của file này PASS.


## 1. Vấn đề cần giải quyết

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

## 14. Device lifecycle refactor

### 14.1 Delete phải quiesce device trước destructive purge

```text
DELETE device
    ↓
block scheduler device
    ↓
quiesce_device(timeout)
    ├─ cancel queued jobs exact-once
    ├─ cancel inflight through DCS
    ├─ revoke lease
    └─ wait bounded until scheduler state idle
    ↓
settings_forget
schema_forget
state_forget
MCP exposure purge/rebuild
BLE forget peer
store delete
    ↓
publish DEVICE_REMOVED
```

Không purge runtime memory trong khi callback inflight còn có thể truy cập old lifecycle state.

#### 14.2 Failure rollback

Nếu destructive step fail trước store delete và device vẫn tồn tại:

```text
scheduler_unblock_device(device)
publish DEVICE_CHANGED/degraded
```

Không để device bị block vĩnh viễn.

Nếu store đã delete nhưng BLE forget fail, result có thể DEGRADED nhưng scheduler/device runtime phải ở trạng thái deterministic và không nhận command cho inventory đã xóa.

#### 14.3 Targeted vs destructive cancel

- State revision replacement -> `cancel_source()`;
- user cancel một operation -> `cancel_job()`;
- delete -> `block + quiesce/cancel_device()`.

#### 14.4 Device management hooks

Bổ sung hooks:

```c
esp_err_t (*settings_forget)(const char *device_id);
esp_err_t (*scheduler_block)(const char *device_id);
esp_err_t (*scheduler_unblock)(const char *device_id);
esp_err_t (*scheduler_quiesce)(const char *device_id, uint32_t timeout_ms);
```

Direct `device_command_service_cancel_device` không còn là lifecycle API chính sau migration.

---

## 15. Gateway readiness refactor

### 15.1 State model

Không map:

```text
WIFI_CONNECTED -> BOARD_STATUS_READY
```

Target:

```text
BOOTING
  ↓
WIFI_CONNECTING / PROVISIONING
  ↓
GATEWAY_INITIALIZING
  ↓
all mandatory runtime initialized
  ↓
OTA finalize
  ├─ fail -> ERROR/rollback/reboot
  └─ success
       ↓
READY
```

#### 15.2 Mandatory/optional readiness matrix

Mandatory STA runtime:

- Wi-Fi connected;
- Device Store;
- Gateway Events;
- Device Schema;
- Device State;
- Device Settings actor;
- DCS;
- Control Scheduler;
- Schema Control Adapter;
- MCP Tool Exposure;
- BLE Central;
- reconnect supervisor;
- Web server;
- local MCP endpoint registration.

Optional/degraded:

- external Xiaozhi MCP WebSocket bridge, nếu config/product policy cho phép.

#### 15.3 READY ordering

Bắt buộc:

```text
runtime init complete
      ↓
gateway_ota_finalize("sta")
      ↓ ESP_OK
board_io_set_status(BOARD_STATUS_READY)
```

Không set READY trước OTA final gate vì `gateway_ota_finalize()` có thể rollback/reboot.

#### 15.4 Failure handling

Mọi mandatory init failure:

1. set ERROR;
2. rollback/deinit các module đã start theo reverse order;
3. không để orphan worker/service tiếp tục chạy;
4. không READY.

---

## 16. Init/deinit order mới

`gateway_runtime.c` là owner của STA runtime lifecycle.

### 16.1 Init

```text
NVS                      # app_main/base
 ↓
OTA validate             # app_main/base
 ↓
Wi-Fi                    # app_main/base
 ↓
Gateway Runtime START
   ├─ Device Store
   ├─ Gateway Events
   ├─ Device Schema
   ├─ Device State
   ├─ Device Settings + actor
   ├─ DCS
   ├─ Control Scheduler
   ├─ Schema Control Adapter
   ├─ MCP Tool Exposure
   ├─ BLE Central
   ├─ BLE lifecycle callbacks
   ├─ Reconnect Supervisor
   ├─ Web Server
   ├─ local MCP endpoint
   └─ external MCP bridge optional
 ↓
OTA finalize
 ↓
READY
```

Rationale:

- event bus trước publisher;
- DCS trước scheduler;
- scheduler/adapter trước BLE ready kích schema discovery;
- Settings actor trước schema commit events;
- control backend trước Web/MCP entry points.

### 16.2 Reverse-order rollback/deinit

Nếu init fail tại bước N, deinit N-1..1:

```text
external MCP bridge
local MCP/Web
reconnect supervisor
BLE
MCP exposure
schema adapter
scheduler
DCS
settings actor
state
schema
gateway events
device store runtime resources
```

Mỗi deinit phải idempotent hoặc runtime layer track `started` flags.

---

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

### 17.5 State / Settings / Web / MCP

- State add scheduler + shared command types;
- Settings add scheduler + shared command types;
- Web/MCP đổi DCS dependency sang scheduler/shared command types nếu không còn service API usage.

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

### 17.8 Test project

Update `test/CMakeLists.txt` `TEST_COMPONENTS` cho:

- `device_command_types` nếu cần;
- `device_control_scheduler`;
- `device_schema_control_adapter` tests nếu có.

Build phải chứng minh dependency graph không cycle.

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


## 3. Implementation plan của phase

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

- [x] Device quiesce before purge.
- [ ] Late ACK lifecycle safe.
- [x] Failure unblocks surviving device.
- [x] Settings/schema/state purge complete.
- [ ] Runtime rollback reverse-order implemented.
- [x] OTA finalize precedes READY.
- [ ] Local MCP included in ready gate.
- [ ] Provisioning regression pass.
- [x] Plan doc updated.

#### Kết quả delete regression — 2026-09-10

- `device_settings_forget()` đã idempotent khi thiết bị không có Settings
  record; `ESP_ERR_NOT_FOUND` không còn chặn schema/state/BLE/store purge.
- Root firmware và test project build PASS trên target `esp32s3`.
- Firmware production đã flash và test qua REST với một inventory record không
  có BLE/Settings: precondition trả `404 settings_unsupported`, DELETE trả
  HTTP 200/status 0 với toàn bộ cờ cleanup `true`, record không còn trong
  inventory sau delete.
- Late-ACK/inflight delete và full HIL matrix vẫn là gate mở; phase chưa DONE.

---


## 4. Test cases chi tiết

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


## 5. Acceptance / static analysis / build-hardware gate

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


## 6. Checklist đóng phase

- [ ] Tất cả file **ADD** trong phase đã được thêm vào CMake dependency graph.
- [ ] Tất cả file **EDIT** trong phase đã được cập nhật đúng contract mới.
- [ ] Không còn caller/flow legacy bị phase này yêu cầu loại bỏ.
- [ ] Unit test của phase PASS trên test project.
- [ ] Firmware root project build PASS cho `esp32s3`.
- [ ] Không xuất hiện warning mới liên quan ownership, queue, task stack hoặc lifetime.
- [ ] Static grep/acceptance của phase PASS.
- [ ] Hardware test bắt buộc của phase PASS nếu phase yêu cầu BLE/Wi-Fi thực.
- [ ] Memory checkpoint không regression ngoài budget đã định nghĩa.
- [ ] Checklist chi tiết trong phần Implementation plan đã được đánh `[x]` toàn bộ.
- [ ] Heading phase được cập nhật `✅ DONE (YYYY-MM-DD)` trước khi bắt đầu phase kế tiếp.
