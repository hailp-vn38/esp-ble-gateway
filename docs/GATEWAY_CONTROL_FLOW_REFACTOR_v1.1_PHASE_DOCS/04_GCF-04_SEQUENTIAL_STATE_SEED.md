# GCF-04 — Sequential State Seed + Targeted Cancellation ✅ DONE (2026-09-09)
> Bộ tài liệu này được tách từ `GATEWAY_CONTROL_FLOW_REFACTOR_PLAN_v1.1.md`.
> Mỗi file phase là tài liệu triển khai độc lập và là nguồn checklist của phase tương ứng.
> Khi phase hoàn thành, đánh `[x]` toàn bộ checklist của phase và thêm `✅ DONE (YYYY-MM-DD)` vào heading phase theo rule của `AGENTS.md`.

← Trước: `03_GCF-03_SCHEMA_CONTROL_ADAPTER.md` | Tiếp: `05_GCF-05_SETTINGS_SINGLE_OWNER_ACTOR.md` →

## 0. Phase gate

- **Phase:** `GCF-04`
- **Phụ thuộc:** GCF-03 phải DONE
- **Trạng thái ban đầu:** `[ ] NOT STARTED`
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


## 2. Thiết kế, file và code contract cần triển khai

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

## 9. State seed refactor

### 9.1 Mục tiêu

Thay bulk submit bằng sequential/background session:

```text
schema committed R
      ↓
seed session owner_token=R/generation
      ↓
feature[0] -> result -> feature[1] -> ... -> DONE
```

Chỉ một seed command/device được scheduler dispatch tại một thời điểm; user HIGH job có thể chạy ở dispatch boundary kế tiếp.

#### 9.2 File split

`device_state.c` giữ cache/apply/snapshot/forget.

`device_state_seed.c` giữ:

- schema commit listener;
- seed sessions;
- cursor;
- scheduler submit;
- targeted cancel;
- generation/stale completion guard.

#### 9.3 Lightweight schema accessor

Không gọi `device_schema_get()` full snapshot cho mỗi feature vì có thể tạo O(N²) copy cost.

ADD preferred API:

```c
typedef struct {
    device_feature_id_t feature_id;
    uint8_t property_id;
    bool readable;
} device_schema_feature_ref_t;

esp_err_t device_schema_get_feature_at(
    const char *device_id,
    uint32_t expected_revision,
    size_t index,
    device_schema_feature_ref_t *out);
```

Accessor:

- copy đúng descriptor nhỏ cần cho seed;
- verify committed revision;
- thread-safe theo schema snapshot contract;
- không expose pointer nội bộ sau unlock.

#### 9.4 Seed session data

```c
typedef struct {
    bool active;
    device_id_t device_id;
    uint32_t schema_revision;
    device_control_owner_token_t owner_token;
    size_t next_feature_index;
    device_control_job_id_t active_job_id;
    uint8_t retry_count;
} device_state_seed_session_t;
```

#### 9.5 Scheduler job policy

```c
job.priority = DEVICE_CTRL_PRIORITY_BACKGROUND;
job.source = DEVICE_CTRL_SOURCE_STATE;
job.owner_token = session->owner_token;
job.max_queue_wait_ms = 0;
job.dedupe = DEVICE_CTRL_DEDUPE_DROP_IF_EXISTS;
```

Dedupe structured equality:

```text
device_id + read_feature_state + feature_id + property_id
```

Hash chỉ giúp lookup, phải verify equality để tránh collision.

#### 9.6 Cancel semantics

Disconnect:

```text
cancel_source(device, STATE, owner_token)
invalidate session generation
```

Schema R2 đến khi R1 đang seed:

```text
cancel_source(device, STATE, owner_token_R1)
ignore stale R1 completion
start R2 owner_token
```

Không dùng `cancel_device()` cho revision replacement.

#### 9.7 Completion contract

Seed completion chỉ:

- validate session generation/owner token;
- advance cursor hoặc bounded retry;
- submit feature kế tiếp;
- không giữ schema snapshot dài hạn.

Nếu job được `SUPERSEDED/CANCELLED`, session tương ứng dừng sạch và không update stale state.

#### 7.8 Targeted cancel

Ba cấp:

1. `cancel_job(job_id)` — đúng một job;
2. `cancel_source(device, source, owner_token)` — ví dụ seed revision cũ;
3. `cancel_device(device)` — lifecycle destructive operation.

State seed revision replacement **không** được gọi `cancel_device()`.

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

- [x] Bulk seed removed.
- [x] Lightweight schema accessor added.
- [x] Sequential seed pass.
- [x] Targeted cancel used, not cancel_device.
- [x] Revision/generation stale completion guard pass.
- [x] Background priority pass.
- [x] State cache behavior preserved.
- [x] Plan doc updated.

---


## 4. Test cases chi tiết

### TC-STATE-001 — Full sequential seed

Schema N readable properties -> exactly N sequential reads, no bulk-submit.

### TC-STATE-002 — Revision replacement

R1 seed active, commit R2.

Expected targeted cancel R1, stale R1 completion ignored, R2 starts; Web/Settings jobs untouched.

### TC-STATE-003 — Lightweight schema accessor

Iterate N features using `device_schema_get_feature_at()`.

Expected correct revision guard and no full-snapshot-per-feature path.


## 5. Acceptance / static analysis / build-hardware gate

#### After GCF-04 — no bulk state submit

```sh
git grep "device_command_service_submit" -- components/device_state
git grep "device_schema_get(" -- components/device_state/device_state_seed.c
```

Expected direct DCS submit = none; seed must use scheduler and lightweight accessor rather than full snapshot each feature.

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
