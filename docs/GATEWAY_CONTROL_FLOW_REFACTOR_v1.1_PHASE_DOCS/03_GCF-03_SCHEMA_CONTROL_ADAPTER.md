# GCF-03 — Schema Control Adapter; No Dependency Cycle ✅ DONE (2026-09-09)
> Bộ tài liệu này được tách từ `GATEWAY_CONTROL_FLOW_REFACTOR_PLAN_v1.1.md`.
> Mỗi file phase là tài liệu triển khai độc lập và là nguồn checklist của phase tương ứng.
> Khi phase hoàn thành, đánh `[x]` toàn bộ checklist của phase và thêm `✅ DONE (YYYY-MM-DD)` vào heading phase theo rule của `AGENTS.md`.

← Trước: `02_GCF-02_DEVICE_CONTROL_SCHEDULER.md` | Tiếp: `04_GCF-04_SEQUENTIAL_STATE_SEED.md` →

## 0. Phase gate

- **Phase:** `GCF-03`
- **Phụ thuộc:** GCF-02 phải DONE
- **Trạng thái:** `[x] DONE (2026-09-09)`
- **Quy tắc hoàn thành:** chỉ chuyển phase tiếp theo khi toàn bộ checklist + test bắt buộc + acceptance của file này PASS.


## 1. Vấn đề cần giải quyết

#### 2.10 Schema migration có nguy cơ dependency cycle

DCS hiện dùng `device_schema_validate_command()`. Nếu `device_schema` lại `REQUIRES device_control_scheduler`, graph trở thành:

```text
device_schema -> scheduler -> DCS -> device_schema
```

Schema phải giữ submitter abstraction và scheduler bridge nằm ở adapter component độc lập.


## 2. Thiết kế, file và code contract cần triển khai

##### INV-12: Dependency graph không cycle

Đặc biệt cấm:

```text
device_schema -> device_control_scheduler -> device_command_service -> device_schema
```

Schema phải giữ transport abstraction và được nối scheduler bằng adapter component độc lập.

---

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

## 11. Schema migration

Schema hiện đã có transport abstraction `device_schema_set_submitter()`; v1.1 **giữ abstraction này** để tránh dependency cycle.

Target:

```text
Device Schema
   │ submitter interface
   ▼
Device Schema Control Adapter
   │
   ▼
Control Scheduler NORMAL
   │
   ▼
DCS
```

#### 11.1 Không add scheduler dependency vào `device_schema`

Cấm implementation:

```text
device_schema REQUIRES device_control_scheduler
```

vì DCS hiện depend schema validation:

```text
device_schema -> scheduler -> DCS -> device_schema   // cycle
```

#### 11.2 Adapter component

ADD:

```text
components/device_schema_control_adapter/device_schema_control_adapter.c
components/device_schema_control_adapter/include/device_schema_control_adapter.h
components/device_schema_control_adapter/CMakeLists.txt
```

Responsibilities:

- register itself qua `device_schema_set_submitter()`;
- map `gw_message_t` schema submit request sang `device_control_job_t`;
- priority NORMAL;
- source SCHEMA;
- bounded queue deadline;
- map scheduler terminal/command result về `device_schema_submit_result_t`;
- completion bridge cực ngắn.

Main chỉ gọi:

```c
device_schema_control_adapter_init();
```

Không còn `capability_submit_bridge_t` trong `main.c`.

#### 11.3 Global schema serialization

Giữ current schema global owner trong phase đầu. Sau soak mới benchmark concurrency 2; không mở cùng refactor.

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


## 3. Implementation plan của phase

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

- [x] Adapter component added.
- [x] `device_schema` has no scheduler dependency.
- [x] Main schema bridge removed.
- [x] Schema requests pass scheduler.
- [x] Dependency graph build pass.
- [x] Existing schema mock submitter tests preserved.
- [x] Plan doc updated.

---


## 4. Test cases chi tiết

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
