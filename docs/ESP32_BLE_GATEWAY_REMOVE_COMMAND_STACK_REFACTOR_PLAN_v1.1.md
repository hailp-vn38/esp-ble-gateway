# ESP32 BLE Gateway - Remove Legacy Command Stack Refactor Plan v1.1

**Repository:** `hailp-vn38/esp-ble-gateway`
**Target branch:** `dev-ws`
**Reviewed baseline:** `f09321d2e010e3fbea19d071bc8c83bd90bfbcb8`
**Date:** 2026-09-04
**Target platform:** ESP32-S3 / ESP-IDF
**Supersedes:** `ESP32_BLE_GATEWAY_REMOVE_COMMAND_STACK_REFACTOR_PLAN_v1.0.md`

---

## 1. Mục tiêu

Tài liệu này là bản cập nhật execution-ready để loại bỏ hoàn toàn hai component legacy:

```text
components/command_executor/
components/command_dispatcher/
```

và các phần phụ thuộc liên quan:

```text
command_registry
device_request_manager
dispatch_result_t
generic gw_message_t gateway routing
legacy ACK fallback
CONFIG_CMD_EXEC_*
```

Mục tiêu cuối cùng là đưa gateway về typed-domain architecture, trong đó mỗi concern có đúng một owner và không còn flow generic nhiều tầng:

```text
Transport
  -> gw_message_t
  -> executor
  -> dispatcher
  -> registry
  -> handler
  -> dispatch_result_t
  -> parse/format lại
```

Thay bằng:

```text
MCP / Web
   -> typed API
   -> domain owner
   -> BLE / store / schema / state
```

Mục tiêu cụ thể:

- bỏ toàn bộ worker pool của `command_executor`;
- bỏ toàn bộ registry/router của `command_dispatcher`;
- bỏ semaphore-based `device_request_manager`;
- bỏ `dispatch_result_t` 4 KB khỏi production path;
- chỉ `device_command_service` được sở hữu request-id / pending / ACK / timeout;
- `device_command_service` trở thành authoritative validation boundary cho device command;
- Web CRUD dùng typed `device_management` trực tiếp;
- MCP compact giữ đúng 3 tools: `get_status`, `list_devices`, `device_control`;
- common MCP control đạt flow phổ biến `list_devices -> device_control(set)` trong 2 tool calls;
- repeated control có thể chỉ còn 1 tool call nếu agent đã giữ `device_id + feature_id`;
- không duplicate semantic/exposure/policy resolution giữa `list_devices` và `device_control`;
- không thay monolith cũ bằng một monolith mới.

---

## 2. Các vấn đề của v1.0 đã được sửa trong v1.1

Bản này xử lý các điểm review quan trọng sau:

1. **Schema/type/range validation được đưa xuống `device_command_service`.** Web và MCP không còn có trust boundary khác nhau.
2. **Thêm shared device types độc lập protocol.** Domain API không còn phụ thuộc `cbor_codec.h` chỉ để lấy `GW_MSG_*_LEN`.
3. **Thêm shared MCP semantic-control helper.** `list_devices` và `device_control` không duplicate resolver/policy logic.
4. **Làm rõ `describe`: feature optional.** `describe(device)` phải hoạt động mà không cần feature.
5. **INT controls có `minimum/maximum/step` trong `list_devices`.** Common INT SET vẫn đạt 2-call flow.
6. **Làm rõ delete semantics và degraded result.** Không vô tình đổi behavior trong quá trình refactor.
7. **Thêm latency/non-blocking contract cho Web CRUD.** Không tái tạo executor dưới tên khác.
8. **Sửa thứ tự cleanup/delete phases.** `dispatch_result_t` và consumer phải biến mất trước khi xóa directory.
9. **Sửa zero-reference gate.** Trước delete chỉ kiểm tra external consumers; sau delete mới grep toàn repo.
10. **Thêm phase hardening `device_command_service` trước mọi consumer migration.**

---

## 3. Kiến trúc đích

### 3.1 Ownership cuối cùng

| Concern | Owner |
|---|---|
| BLE command lifecycle | `device_command_service` |
| Device capability/schema | `device_schema` |
| Runtime feature state cache | `device_state` |
| Persistent device records | `device_store` |
| Device lifecycle CRUD/inventory | `device_management` |
| Gateway health/status | `gateway_status` |
| MCP semantic resolution/hints | `mcp_semantic_control` trong `mcp_endpoint` |
| MCP authorization/health | `mcp_tool_exposure` + MCP policy |
| MCP RPC/tool formatting | `mcp_endpoint` |
| HTTP parse/format | `web_server` |
| BLE transport/protocol DTO | `ble_central`, `cbor_codec` |

### 3.2 Flow MCP cuối cùng

```text
                         MCP CORE
                            |
             +--------------+--------------+
             |              |              |
             v              v              v
        get_status      list_devices   device_control
             |              |              |
             v              v              |
      gateway_status  device_management    |
                            |              |
                            +-> mcp_semantic_control
                                           |
                      +--------------------+-------------------+
                      |                    |                   |
                   describe              read                set
                      |                    |                   |
                 device_schema       device_state             |
                                                              v
                                                    mcp_semantic_control
                                                              |
                                                    exposure/policy check
                                                              |
                                                              v
                                                    device_command_service
                                                              |
                                                              v
                                                             BLE
```

### 3.3 Flow Web cuối cùng

```text
GET /api/devices
   -> device_management_snapshot()
   -> serialize JSON once
   -> HTTP

POST/PUT/DELETE /api/devices
   -> parse typed request
   -> device_management_add/edit/delete()
   -> typed result
   -> HTTP

POST /api/command
   -> parse device command
   -> device_command_service_submit()
   -> async completion
   -> HTTP response
```

### 3.4 BLE notify cuối cùng

```text
BLE notify
   |
   +-> device_schema_on_notify()
   +-> device_state_on_notify()
   +-> device_state_on_command_ack()      // observer only
   `-> device_command_service_on_notify() // ACK owner duy nhất
```

Không còn:

```text
command_dispatcher_on_device_notify()
device_request_manager
semaphore wait
fallback ACK owner
```

---

## 4. Architectural invariants bắt buộc

1. `device_command_service` là owner duy nhất của pending command, request-id, ACK, timeout và disconnect completion.
2. `device_command_service` phải tự validate request theo `origin`; không dựa vào caller đã validate.
3. MCP policy là authorization layer, không thay thế schema/type validation.
4. `gw_message_t` chỉ dùng ở protocol/BLE boundary hoặc nơi thật sự serialize xuống device.
5. Web/MCP domain API không được route bằng `msg->type` / `msg->command` generic.
6. Không tạo `gateway_service_execute(command_name, ...)` thay cho dispatcher cũ.
7. Không tạo generic worker/executor mới.
8. `device_management` không chứa cJSON, HTTP, MCP tool formatting hay BLE ACK lifecycle.
9. MCP semantic mapping/exposure filtering phải có shared helper, không viết lại hai lần.
10. `list_devices.controls[]` chỉ chứa AI-safe semantic hints, không raw BLE command/tool name.
11. SET phải revalidate policy + schema dù agent đã lấy hint từ `list_devices` trước đó.
12. Device CRUD không được chờ BLE connect hoặc ACK.
13. Mỗi phase phải build/test độc lập trước phase tiếp theo.
14. Chỉ delete legacy directories sau zero-consumer gate.

---

# PHASE 0 - Baseline Characterization ✅ DONE (2026-09-04)

## Tổng quan

Đóng băng hành vi và resource baseline trước refactor. Không thay đổi production behavior trong phase này.

## File cần sửa

```text
components/mcp_endpoint/test/test_mcp_endpoint.c
components/mcp_endpoint/test/test_mcp_xiaozhi.c
components/device_command_service/test/test_device_command_service.c
components/web_server/test/
test/test_results.txt
docs/reports/
```

## Sửa cái gì

Bổ sung characterization tests cho:

- compact `tools/list` đúng 3 tools;
- `get_status` hiện tại;
- `list_devices` hiện tại;
- `device_control describe/read/set`;
- Web CRUD `/api/devices`;
- Web `/api/command`;
- BLE ACK success/reject/timeout/disconnect;
- current delete ordering và degraded behavior;
- current executor worker/task count;
- current heap/task/stack baseline.

Ghi lại ít nhất:

```text
free internal heap
minimum free heap
largest internal block
PSRAM free
task count
command executor worker count
list_devices payload bytes
```

## Test xác nhận

```bash
cd test
idf.py fullclean
idf.py build
```

Hardware nếu có:

```bash
idf.py flash monitor
```

Pass khi:

```text
baseline tests pass
compact tools = exactly 3
memory/task baseline recorded
```

## Checklist

- [x] Baseline SHA ghi rõ `f09321d2e010e3fbea19d071bc8c83bd90bfbcb8`
- [x] Compact `tools/list` đúng 3 tools
- [x] Web CRUD baseline có test
- [x] MCP `device_control` baseline có test
- [x] ACK success/reject/timeout/disconnect có test
- [x] Delete failure/degraded behavior được characterize
- [x] Heap/task baseline đã lưu
- [x] Phase 0 không đổi production behavior
- [x] Tất cả baseline tests pass

---

# PHASE 1 - Introduce Protocol-Independent Shared Device Types ✅ DONE (2026-09-04)

## Tổng quan

Loại dependency ngược từ domain layer vào protocol DTO. Không để `device_management` phải include `cbor_codec.h` chỉ để lấy `GW_MSG_DEVICE_ID_LEN`, `GW_MSG_NAME_LEN` hoặc feature-id lengths.

## File cần thêm

Khuyến nghị:

```text
components/device_types/CMakeLists.txt
components/device_types/include/device_types.h
components/device_types/test/CMakeLists.txt
components/device_types/test/test_device_types.c
```

Nếu muốn giảm component count, có thể đặt shared header trong `device_store/include/device_types.h`, nhưng **không** đặt trong `cbor_codec`.

## File cần sửa

```text
components/cbor_codec/include/cbor_codec.h
components/device_store/include/device_store.h
components/device_schema/include/device_schema.h
components/device_state/include/device_state.h
components/device_command_service/include/device_command_service.h
CMakeLists REQUIRES liên quan
```

## Sửa cái gì

Định nghĩa shared domain limits/types:

```c
#define DEVICE_ID_MAX_LEN        48
#define DEVICE_NAME_MAX_LEN      64
#define DEVICE_FEATURE_ID_MAX_LEN 48
```

Giá trị thực tế phải lấy từ protocol hiện tại để không đổi wire compatibility.

Sau đó protocol constants có thể alias sang domain constants nếu cần:

```c
#define GW_MSG_DEVICE_ID_LEN DEVICE_ID_MAX_LEN
```

Không đổi layout/wire behavior trong phase này.

## Test xác nhận

- build toàn project;
- compile-size/static asserts nếu protocol struct phụ thuộc length;
- encode/decode CBOR regression;
- device store/schema/state tests không đổi behavior.

## Checklist

- [x] Domain public headers không include `cbor_codec.h` chỉ để lấy length constants
- [x] Protocol wire size không thay đổi
- [x] CBOR tests pass
- [x] Device store/schema/state tests pass
- [x] Không có new dynamic allocation
- [x] Build clean

---

# PHASE 2 - Harden `device_command_service` as Authoritative Command Boundary ✅ DONE (2026-09-04)

## Tổng quan

Đây là phase P0 bắt buộc trước khi tháo legacy consumer. Service mới phải đủ mạnh để thay hoàn toàn legacy `device_command + device_request_manager`.

## File cần sửa

```text
components/device_command_service/device_command_service.c
components/device_command_service/include/device_command_service.h
components/device_command_service/test/test_device_command_service.c
components/device_command_service/CMakeLists.txt
```

Có thể cần:

```text
components/device_schema/include/device_schema.h
components/device_schema/device_schema_validate.c
```

## Sửa cái gì

### 2.1 Authoritative validation theo origin

`DEVICE_CMD_ORIGIN_CONTROL`:

```text
require device_id
require command
require connected
validate command against committed schema
validate typed value
validate feature/property when command requires them
validate INT min/max/step
reject unsupported/raw unadvertised command
```

Quan trọng: **service phải tự gọi schema validator**. Không dựa vào MCP/Web caller.

`DEVICE_CMD_ORIGIN_SCHEMA_DISCOVERY`:

```text
only command == describe_capabilities
```

`DEVICE_CMD_ORIGIN_STATE_READ`:

```text
only command == read_feature_state
feature_id required
property_id required
```

### 2.2 Typed validation result

Bổ sung status đủ rõ:

```c
DEVICE_CMD_STATUS_INVALID_ARGUMENT
DEVICE_CMD_STATUS_SCHEMA_NOT_READY
DEVICE_CMD_STATUS_UNSUPPORTED_COMMAND
DEVICE_CMD_STATUS_TYPE_MISMATCH
DEVICE_CMD_STATUS_RANGE_ERROR
DEVICE_CMD_STATUS_NOT_CONNECTED
DEVICE_CMD_STATUS_BUSY
DEVICE_CMD_STATUS_QUEUE_FULL
DEVICE_CMD_STATUS_TIMEOUT
DEVICE_CMD_STATUS_REJECTED
DEVICE_CMD_STATUS_CANCELLED
DEVICE_CMD_STATUS_INTERNAL
```

### 2.3 Pending ownership

Giữ bounded model:

```text
MAX_PENDING ~= 4
one pending request per device
```

Không semaphore per request.

### 2.4 ACK correlation

Match tối thiểu:

```text
device_id + request_id
```

Có thể additionally validate command nếu protocol ACK có field này.

Late/duplicate ACK không được gọi completion lần hai.

### 2.5 Disconnect/cancel

Service phải có API rõ ràng, ví dụ:

```c
esp_err_t device_command_service_on_disconnect(const char *device_id);
esp_err_t device_command_service_cancel_device(const char *device_id);
```

Nếu API hiện có tương đương thì giữ, không cần tạo duplicate.

### 2.6 Callback exactly once

Mọi pending request phải kết thúc bằng đúng một completion:

```text
ACK success
ACK reject
timeout
disconnect
cancel
shutdown
```

## Test xác nhận

Bắt buộc test:

```text
CONTROL invalid command        -> BLE send = 0
CONTROL wrong value type       -> BLE send = 0
CONTROL INT below min          -> BLE send = 0
CONTROL INT above max          -> BLE send = 0
CONTROL INT bad step           -> BLE send = 0
CONTROL valid                  -> BLE send = 1
SCHEMA wrong command           -> BLE send = 0
STATE_READ missing feature     -> BLE send = 0
STATE_READ missing property    -> BLE send = 0
STATE_READ valid               -> BLE send = 1
second pending same device     -> BUSY
pending table full             -> bounded failure
queue full                     -> bounded failure
timeout                        -> completion exactly once
disconnect                     -> completion exactly once
late ACK after timeout         -> ignored
duplicate ACK                  -> ignored
shutdown with pending          -> completion exactly once
```

## Checklist

- [x] CONTROL path gọi schema validation bên trong service
- [x] Type/range/step validation nằm trong authoritative boundary
- [x] Internal origins có strict allowlist
- [x] Pending table bounded
- [x] One pending/device invariant giữ nguyên
- [x] No semaphore wait
- [x] Completion exactly once được test
- [x] Late/duplicate ACK được test
- [x] Disconnect/cancel được test
- [x] Phase 2 pass trước khi migrate Web/MCP

---

# PHASE 3 - Introduce Typed `device_management` ✅ DONE (2026-09-04)

## Tổng quan

Tách device CRUD + inventory khỏi `gateway_commands.c` sang typed application/domain service nhỏ. Không đưa cJSON, HTTP, MCP RPC hay generic command routing vào component này.

## File cần thêm

```text
components/device_management/CMakeLists.txt
components/device_management/include/device_management.h
components/device_management/device_management.c
components/device_management/device_management_inventory.c      # optional
components/device_management/device_management_mutation.c       # optional
components/device_management/test/CMakeLists.txt
components/device_management/test/test_device_management.c
```

## File cần sửa

```text
test/CMakeLists.txt
```

Chưa migrate consumer production trong phase này.

## Sửa cái gì

### 3.1 Typed status

Ví dụ:

```c
typedef enum {
    DEVICE_MGMT_OK = 0,
    DEVICE_MGMT_INVALID_ARG,
    DEVICE_MGMT_NOT_FOUND,
    DEVICE_MGMT_CONFLICT,
    DEVICE_MGMT_CAPACITY,
    DEVICE_MGMT_BUSY,
    DEVICE_MGMT_DEGRADED,
    DEVICE_MGMT_INTERNAL,
} device_mgmt_status_t;
```

### 3.2 Typed add/edit/delete request/result

Không nhận `gw_message_t`.

Ví dụ:

```c
typedef struct {
    char device_id[DEVICE_ID_MAX_LEN];
    char name[DEVICE_NAME_MAX_LEN];
    bool has_ble_identity;
    uint8_t ble_addr[6];
    uint8_t ble_addr_type;
} device_mgmt_add_request_t;
```

Delete result nên mô tả degraded cleanup:

```c
typedef struct {
    device_mgmt_status_t status;
    bool schema_forgotten;
    bool state_forgotten;
    bool ble_peer_forgotten;
    bool store_deleted;
} device_mgmt_delete_result_t;
```

### 3.3 Inventory typed snapshot

```c
typedef struct {
    char device_id[DEVICE_ID_MAX_LEN];
    char name[DEVICE_NAME_MAX_LEN];
    bool connected;
    bool ready;
    bool has_ble_identity;
    uint8_t ble_addr[6];
    uint8_t ble_addr_type;
    bool schema_available;
    device_schema_state_t schema_state;
    uint32_t schema_revision;
    uint8_t feature_count;
    uint8_t writable_feature_count;
} device_inventory_entry_t;
```

### 3.4 Delete semantics phải explicit

Không copy vô thức behavior cũ. Contract khuyến nghị:

```text
1. resolve current device record
2. cancel pending device commands for device
3. forget schema
4. forget runtime state
5. forget BLE peer best-effort
6. delete store record
7. publish device lifecycle event
```

Nếu schema forget fail:

```text
abort destructive remainder
return INTERNAL
```

Nếu BLE forget fail nhưng store delete success:

```text
return DEGRADED
```

Nếu store delete fail:

```text
return DEGRADED/INTERNAL with store_deleted=false
```

Tài liệu/HTTP caller phải phản ánh kết quả thay vì luôn trả success.

### 3.5 MCP exposure cleanup không nằm trong `device_management`

Không thêm dependency:

```text
device_management -> mcp_tool_exposure
```

Thay vào đó publish lifecycle event sau mutation thành công/partial result:

```text
GW_EVENT_DEVICE_ADDED
GW_EVENT_DEVICE_RENAMED
GW_EVENT_DEVICE_REMOVED
```

`mcp_tool_exposure` đăng listener để cleanup/refresh best-effort.

**Safety rule:** kể cả exposure cleanup chưa chạy hoặc fail, MCP semantic resolver vẫn phải yêu cầu device tồn tại + schema/exposure health hợp lệ trước SET. Stale exposure record không được tự mình authorize control.

### 3.6 Non-blocking contract

`device_management_*()` không được:

```text
wait BLE connect
wait BLE ACK
sleep/retry long
create worker task
```

`ble_central_connect()` chỉ best-effort trigger nếu API hiện tại non-blocking.

## Test xác nhận

Unit tests:

```text
add valid
add duplicate id
add duplicate BLE identity
add capacity full
edit valid
edit missing
snapshot combines store + BLE runtime
schema summary correct
delete valid
delete schema failure aborts
delete BLE forget failure -> degraded
delete store failure -> degraded
repeated delete -> not found
pending command cancelled on delete
lifecycle event published
```

Latency test/mocked assertion:

```text
CRUD does not wait for BLE ACK/connect
```

## Checklist

- [x] Public API typed, không `gw_message_t`
- [x] Không cJSON/HTTP/MCP formatting
- [x] Không depend `mcp_tool_exposure`
- [x] Delete semantics documented + tested
- [x] Degraded result explicit
- [x] Pending command cancel on delete handled
- [x] CRUD non-blocking contract tested
- [x] Inventory typed snapshot pass
- [x] Lifecycle events published

---

# PHASE 4 - Migrate Web `/api/devices` Off Dispatcher/Executor ✅ DONE (2026-09-04)

## Tổng quan

Chuyển toàn bộ GET/POST/PUT/DELETE `/api/devices` sang `device_management` trực tiếp.

## File cần sửa

```text
components/web_server/web_device_api.c
components/web_server/CMakeLists.txt
components/web_server/test/
```

Có thể cần:

```text
components/web_server/web_http.c
components/web_server/web_http.h
```

## Sửa cái gì

### GET

Bỏ:

```text
gw_message_t("list_devices")
command_dispatcher_handle()
dispatch_result_t
cJSON_Parse(result->payload)
```

Thay bằng:

```text
device_management_snapshot()
-> serialize typed entries directly
```

Chỉ serialize một lần.

### POST/PUT/DELETE

Bỏ:

```text
command_executor_submit()
command_dispatcher
```

Thay bằng direct typed calls:

```text
device_management_add()
device_management_edit()
device_management_delete()
```

Map typed status sang HTTP status tại Web layer.

### Response compatibility

Nếu frontend đang phụ thuộc field cũ, giữ JSON contract trong phase này trừ khi đã có test/migration rõ ràng.

## Test xác nhận

```text
GET empty/list multiple
POST add
POST duplicate
PUT rename
PUT missing
DELETE success
DELETE degraded maps correctly
DELETE missing
GET no serialize->parse->serialize roundtrip
```

Search gate sau phase:

```bash
git grep -nE 'command_executor|command_dispatcher|dispatch_result_t' -- components/web_server/web_device_api.c
```

Expected: no match.

## Checklist

- [x] `web_device_api.c` không include legacy headers
- [x] GET serialize một lần
- [x] CRUD direct typed API
- [x] HTTP status mapping explicit
- [x] Existing frontend contract không regression
- [x] Web device tests pass
- [x] Không blocking worker/executor

---

# PHASE 5 - Finalize Web `/api/command` on `device_command_service` Only ✅ DONE (2026-09-04)

## Tổng quan

Xóa mọi legacy gateway/device command branch còn lại trong `web_command_api.c`. Endpoint này chỉ còn device command service path nếu vẫn cần API này.

## File cần sửa

```text
components/web_server/web_command_api.c
components/web_server/CMakeLists.txt
components/web_server/test/
```

## Sửa cái gì

Bỏ helpers/branches gọi:

```text
command_executor_submit
command_dispatcher_handle
legacy gateway_command
```

Web parse input thành `device_command_request_t` typed request và submit:

```text
device_command_service_submit()
```

Không duplicate schema validation ở Web. Có thể validate shape HTTP cơ bản, nhưng authoritative validation nằm trong service.

Async completion context phải bounded và cleanup exactly once.

## Test xác nhận

```text
valid BOOL -> BLE send 1
invalid command -> BLE send 0
wrong type -> BLE send 0
not connected -> correct HTTP error
busy -> correct HTTP error
timeout -> one HTTP completion
ACK success -> one HTTP completion
client disconnect/context cleanup -> no leak
```

Search:

```bash
git grep -nE 'command_executor|command_dispatcher|dispatch_result_t' -- components/web_server/web_command_api.c
```

Expected: no match.

## Checklist

- [x] Web command uses only `device_command_service`
- [x] No legacy gateway command branch
- [x] No schema-validation duplication required for safety
- [x] Completion exactly once
- [x] Timeout/disconnect cleanup tested
- [x] Web command tests pass

---

# PHASE 6 - Introduce Shared MCP Semantic-Control Helper ✅ DONE (2026-09-04)

## Tổng quan

Ngăn `list_devices` và `device_control` duplicate semantic mapping, feature resolution, exposure health và policy checks.

## File cần thêm

```text
components/mcp_endpoint/mcp_semantic_control.c
components/mcp_endpoint/mcp_semantic_control.h    # private/internal preferred
```

Nếu module lớn, có thể tách:

```text
mcp_semantic_resolver.c
mcp_semantic_hints.c
```

nhưng vẫn giữ private trong MCP component nếu không có consumer ngoài MCP.

## File cần sửa

```text
components/mcp_endpoint/CMakeLists.txt
components/mcp_endpoint/mcp_device_control.c
components/mcp_endpoint/test/test_mcp_endpoint.c
```

## Sửa cái gì

### 6.1 Shared resolver

API nội bộ ví dụ:

```c
mcp_sem_status_t mcp_semantic_resolve_device(...);
mcp_sem_status_t mcp_semantic_resolve_feature(...);
mcp_sem_status_t mcp_semantic_check_write(...);
esp_err_t mcp_semantic_get_control_hints(...);
```

Resolver rules:

```text
device_id exact preferred
configured name only fallback when unique
feature_id exact preferred
semantic alias only fallback when unique
ambiguous -> fail closed
```

### 6.2 Shared control-hint type

```c
typedef struct {
    char feature_id[DEVICE_FEATURE_ID_MAX_LEN];
    char semantic_name[24];
    char property[24];
    mcp_value_type_t value_type;
    bool writable;
    bool has_minimum;
    int32_t minimum;
    bool has_maximum;
    int32_t maximum;
    bool has_step;
    uint32_t step;
} mcp_control_hint_t;
```

### 6.3 Hint filtering

Chỉ include control khi:

```text
trusted semantic mapping
writable feature
authorization/exposure record exists
control_enabled == true
exposure health == ENABLED
digest matches
destructive policy allows advertising as usable
```

SET vẫn phải re-check toàn bộ policy.

### 6.4 Bounded output

```text
MAX_CONTROL_HINTS_PER_DEVICE = 4
```

hoặc config tương đương.

Nếu vượt:

```text
controls_truncated = true
```

Không fail toàn `list_devices`.

## Test xác nhận

```text
exact device_id resolution
unique name resolution
duplicate name -> ambiguous
exact feature_id resolution
unique semantic alias
duplicate semantic alias -> ambiguous
disabled exposure omitted
NEEDS_REVIEW omitted
digest mismatch omitted
read-only omitted from controls
BOOL hint
INT hint with min/max/step
truncation deterministic
```

## Checklist

- [x] Resolver/policy logic không duplicate ở 2 MCP handlers
- [x] Exact IDs ưu tiên
- [x] Ambiguity fail closed
- [x] Hints không raw command/tool name
- [x] INT hint có range/step
- [x] Controls bounded/truncatable
- [x] MCP semantic helper tests pass

---

# PHASE 7 - Migrate MCP `get_status` and `list_devices` to Typed Domains ✅ DONE (2026-09-04)

## Tổng quan

Loại `command_dispatcher`/`command_executor` khỏi hai compact tools này và hoàn thiện 2-call discovery/control flow.

## File cần sửa

```text
components/mcp_endpoint/mcp_tools.c
components/mcp_endpoint/mcp_core.c
components/mcp_endpoint/mcp_registry.c
components/mcp_endpoint/CMakeLists.txt
components/mcp_endpoint/test/test_mcp_endpoint.c
components/mcp_endpoint/test/test_mcp_xiaozhi.c
```

## Sửa cái gì

### `get_status`

Flow:

```text
MCP -> gateway_status_get() -> MCP JSON result
```

Không tạo `gw_message_t`.

### `list_devices`

Flow:

```text
device_management_snapshot()
  -> per device: mcp_semantic_get_control_hints()
  -> serialize MCP result
```

Response target:

```json
[
  {
    "device_id": "AC:27:6E:CC:F2:26",
    "name": "TEST",
    "connected": true,
    "ready": true,
    "capabilities": {
      "available": true,
      "state": "ready",
      "revision": 4,
      "feature_count": 2,
      "writable_feature_count": 1
    },
    "controls": [
      {
        "feature_id": "relay_1",
        "semantic_name": "relay",
        "property": "on_off",
        "value_type": "bool"
      }
    ],
    "controls_truncated": false
  }
]
```

INT example bắt buộc có:

```json
{
  "feature_id": "brightness",
  "semantic_name": "light",
  "property": "level",
  "value_type": "int",
  "minimum": 0,
  "maximum": 100,
  "step": 5
}
```

### Tool descriptions

`list_devices` description phải nói rõ:

```text
Use returned device_id and controls[].feature_id with device_control set.
Call describe only when more feature details are required.
```

## Test xác nhận

2-call integration test:

```text
Call 1: list_devices
  -> device_id
  -> controls[0].feature_id
  -> value_type

Call 2: device_control(set)
  -> later phase executes BLE exactly once
```

Phase 7 có thể mock set target nếu Phase 8 chưa hoàn thiện.

Test payload:

```text
1 device / 1 control
4 devices / 4 controls each
max configured devices
truncation case
```

## Checklist

- [x] `get_status` không dùng dispatcher/executor
- [x] `list_devices` không dùng dispatcher/executor
- [x] `controls[]` có safe semantic hints
- [x] INT hint có min/max/step
- [x] `controls_truncated` deterministic
- [x] No raw command leak
- [x] Tool description hướng dẫn 2-call flow
- [x] Payload tests pass

---

# PHASE 8 - Finalize `device_control` Describe/Read/Set

## Tổng quan

Hoàn thiện `device_control` làm device-control tool duy nhất của compact MCP, dùng shared semantic helper và `device_command_service`.

## File cần sửa

```text
components/mcp_endpoint/mcp_device_control.c
components/mcp_endpoint/mcp_policy.c
components/mcp_endpoint/mcp_registry.c
components/mcp_endpoint/test/test_mcp_endpoint.c
components/mcp_endpoint/test/test_mcp_xiaozhi.c
```

## Sửa cái gì

### 8.1 Parse operation trước feature

Flow đúng:

```text
resolve operation
resolve device

if describe:
    feature optional
    absent -> describe all
    present -> describe one

if read:
    feature required

if set:
    feature required
```

Không được resolve feature unconditional trước switch.

### 8.2 `describe`

Local only:

```text
device_schema + semantic helper
```

Không BLE.

### 8.3 `read`

Local/cache v1:

```text
device_state_get/snapshot
```

Không BLE active-read trừ khi có requirement riêng sau này.

### 8.4 `set`

Flow:

```text
resolve device
resolve feature
shared semantic/policy check
build typed device_command_request_t
origin = CONTROL
device_command_service_submit()
```

Không gọi legacy dispatcher/executor.

Không cần duplicate authoritative schema validation; service sẽ validate lại.

### 8.5 No raw command leak

Result chỉ trả semantic fields:

```text
device_id
feature_id
operation
value
status/request_id when useful
```

Không trả raw internal command string.

## Test xác nhận

```text
describe(device) without feature -> success
describe one feature -> success
read without feature -> invalid
set without feature -> invalid
set BOOL success
set INT success
set wrong type -> fail before BLE or service reject, BLE=0
ambiguous device name -> fail closed
ambiguous feature alias -> fail closed
disabled exposure -> BLE=0
NEEDS_REVIEW -> BLE=0
digest mismatch -> BLE=0
not connected -> correct error
ACK success -> one MCP result
timeout -> one MCP result
reject -> isError appropriate
```

Xiaozhi acceptance:

```text
simple relay intent <= 2 MCP tool calls
```

## Checklist

- [ ] `describe` feature optional
- [ ] `read/set` feature required
- [ ] Shared semantic helper reused
- [ ] SET direct to `device_command_service`
- [ ] Policy rechecked on SET
- [ ] Authoritative schema validation remains in service
- [ ] No raw command leak
- [ ] 2-call Xiaozhi control pass
- [ ] Repeated 1-call SET pass when context retained

---

# PHASE 9 - Simplify MCP Execution Model

## Tổng quan

Xóa execution mode/branch chỉ tồn tại để gọi generic executor/dispatcher cho compact tools.

## File cần sửa

```text
components/mcp_endpoint/mcp_core.c
components/mcp_endpoint/mcp_tools.c
components/mcp_endpoint/mcp_endpoint_internal.h
components/mcp_endpoint/CMakeLists.txt
components/mcp_endpoint/test/
```

## Sửa cái gì

Sau Phase 7-8:

```text
get_status      -> local typed handler
list_devices    -> local typed handler
device_control  -> local semantic handler + async device service for set
```

Loại enum/branch kiểu:

```text
MCP_TOOL_EXEC_DISPATCHER
MCP_TOOL_EXEC_EXECUTOR
```

nếu không còn consumer.

Giữ async completion abstraction chỉ ở nơi thật sự cần `device_control(set)`.

## Test xác nhận

```text
tools/list exactly 3
get_status success
list_devices success
describe/read synchronous local
set async completion
unknown tool correct MCP error
```

Search:

```bash
git grep -nE 'command_executor|command_dispatcher|dispatch_result_t' -- components/mcp_endpoint
```

Expected: no production match.

## Checklist

- [ ] MCP endpoint không depend legacy components
- [ ] No dispatcher/executor execution mode
- [ ] Compact surface vẫn đúng 3 tools
- [ ] Async path chỉ còn nơi cần thiết
- [ ] MCP tests pass

---

# PHASE 10 - Remove Legacy Init and ACK Fallback From `main.c`

## Tổng quan

Sau khi Web/MCP consumers đã migrate, `main.c` không còn lý do init legacy stack hoặc fallback ACK.

## File cần sửa

```text
main/main.c
main/CMakeLists.txt
```

Có thể cần BLE disconnect callback wiring.

## Sửa cái gì

Xóa:

```c
command_dispatcher_init();
command_dispatcher_freeze_registry();
command_executor_init();
```

Notify cuối:

```c
static void on_device_notify(const char *device_id, const gw_message_t *msg)
{
    if (device_schema_on_notify(device_id, msg)) {
        return;
    }

    if (device_state_on_notify(device_id, msg)) {
        return;
    }

    device_state_on_command_ack(device_id, msg);
    (void)device_command_service_on_notify(device_id, msg);
}
```

Không fallback:

```text
command_dispatcher_on_device_notify
```

Disconnect callback phải forward đến service để cancel pending request của device.

## Test xác nhận

```text
boot without dispatcher/executor init
schema discovery works
state seed works
MCP set works
Web command works
ACK completes exactly once
disconnect completes pending request exactly once
```

Search:

```bash
git grep -nE 'command_executor|command_dispatcher' -- main
```

Expected: no match.

## Checklist

- [ ] Legacy init removed
- [ ] ACK fallback removed
- [ ] Service is sole ACK owner
- [ ] Disconnect forwarded to service
- [ ] Boot/integration tests pass
- [ ] `main/CMakeLists.txt` no legacy REQUIRES

---

# PHASE 11 - Remove Remaining Legacy Consumers, `dispatch_result_t`, and Build Dependencies

## Tổng quan

Dọn toàn bộ external references trước khi delete directories. Phase này phải chạy **trước** delete, sửa lỗi ordering của v1.0.

## File cần sửa

Tùy search result, tối thiểu:

```text
components/web_server/CMakeLists.txt
components/mcp_endpoint/CMakeLists.txt
main/CMakeLists.txt
test/CMakeLists.txt
sdkconfig.defaults
test/sdkconfig.defaults
README/docs còn mô tả production architecture
```

Và mọi source/test consumer còn lại.

## Sửa cái gì

Loại production/test use của:

```text
command_executor_*
command_dispatcher_*
dispatch_result_t
command_registry_*
device_request_manager_*
CONFIG_CMD_EXEC_*
```

Xóa pattern gateway routing:

```text
gw_message_t gateway_command
-> dispatcher
```

`gw_message_t` vẫn được phép còn ở BLE/protocol path.

### Legacy tests

Không cần migrate tests cho component sắp xóa. Thay bằng tests của new owners:

```text
command_executor tests -> delete
command_dispatcher tests -> delete
request manager tests -> coverage nằm trong device_command_service tests
gateway_commands tests -> coverage nằm trong device_management + Web/MCP tests
```

Nhưng **chưa xóa source directories** cho tới Phase 12.

## Test xác nhận

Build:

```bash
idf.py fullclean
idf.py build
cd test
idf.py fullclean
idf.py build
```

External-consumer grep phải rỗng, bỏ qua chính legacy directories và docs historical:

```bash
git grep -nE \
'command_executor|command_dispatcher|dispatch_result_t|command_registry_|device_request_manager' \
-- \
':!components/command_executor/**' \
':!components/command_dispatcher/**' \
':!docs/**'
```

Expected: no match.

Config consumer grep:

```bash
git grep -nE 'CONFIG_CMD_EXEC_' -- \
':!components/command_executor/**' \
':!docs/**'
```

Expected: no match.

## Checklist

- [ ] No external production consumer
- [ ] No external test consumer
- [ ] No CMake dependency ngoài legacy directory
- [ ] No `dispatch_result_t` external use
- [ ] No generic gateway routing external use
- [ ] No `CONFIG_CMD_EXEC_*` external use
- [ ] Replacement tests cover legacy behavior
- [ ] Clean production/test build pass

---

# PHASE 12 - Zero-Consumer Gate and Delete Legacy Components

## Tổng quan

Chỉ ở phase này mới xóa directory. Đây là corrected delete gate của v1.1.

## File cần xóa

```text
components/command_executor/
components/command_dispatcher/
```

Bao gồm:

```text
command_executor.c/.h
command_dispatcher.c/.h
command_registry.c
gateway_commands.c
device_command.c
device_request_manager.c/.h
legacy tests
legacy Kconfig/README/CMakeLists
```

## File cần sửa

Nếu build system/top-level docs còn list component:

```text
CMakeLists.txt
README.md
AGENTS.md nếu có architecture reference
sdkconfig defaults
```

## Sửa cái gì

### 12.1 Pre-delete gate

Phải pass Phase 11 external-consumer grep.

### 12.2 Delete

Xóa toàn bộ two directories, không để compatibility shim.

Không tạo:

```text
command_dispatcher_compat.c
command_executor_stub.c
```

trừ khi có requirement backward binary compatibility rõ ràng, hiện không có.

### 12.3 Post-delete full-repo gate

Sau delete:

```bash
git grep -nE \
'command_executor|command_dispatcher|dispatch_result_t|command_registry_|device_request_manager|CONFIG_CMD_EXEC_'
```

Production code/config phải không còn match. Historical docs có thể còn text nếu cố ý lưu lịch sử, nhưng implementation docs active nên update.

Build sạch:

```bash
idf.py fullclean
idf.py build
cd test
idf.py fullclean
idf.py build
```

## Test xác nhận

- clean build từ empty build directory;
- clean test build;
- unit/integration suites;
- linker/map không có legacy symbols;
- boot on ESP32-S3;
- MCP 3 tools;
- Web CRUD;
- BLE set/ACK.

## Checklist

- [ ] Pre-delete consumer gate pass
- [ ] `components/command_executor/` deleted
- [ ] `components/command_dispatcher/` deleted
- [ ] No compatibility shim
- [ ] No legacy Kconfig symbols
- [ ] Full repo implementation grep clean
- [ ] Clean production build pass
- [ ] Clean test build pass
- [ ] Hardware boot pass

---

# PHASE 13 - Final Qualification, Flow Verification, and Memory Measurement

## Tổng quan

Xác nhận refactor không chỉ build được mà còn thực sự giảm tầng logic, RAM, task count và hoàn thiện MCP/Web/BLE flows.

## File cần sửa

```text
docs/reports/REMOVE_COMMAND_STACK_QUALIFICATION.md
test/test_results.txt
README.md
docs/MCP_API.md
architecture docs liên quan
```

## Sửa cái gì

Ghi final architecture và metrics.

### 13.1 Required MCP E2E

```text
tools/list
  -> exactly get_status, list_devices, device_control

list_devices
  -> TEST
  -> device_id
  -> controls[]
  -> relay_1 / bool

device_control(set)
  -> policy
  -> device_command_service
  -> BLE send exactly 1
  -> ACK
  -> MCP completion exactly 1
```

Pass condition:

```text
simple control intent <= 2 MCP tool calls
```

Repeated control:

```text
device_control(set)
```

1 call nếu agent giữ context.

### 13.2 Required Web E2E

```text
GET /api/devices
POST /api/devices
PUT /api/devices
DELETE /api/devices
POST /api/command
```

Không legacy component, không double serialization.

### 13.3 Required BLE lifecycle

```text
schema discovery
state seed
control set
ACK reject
ACK timeout
disconnect while pending
late ACK
duplicate ACK
```

### 13.4 Memory comparison

So Phase 0 vs final:

```text
internal free heap
minimum free heap
largest internal block
PSRAM free
task count
static DRAM/BSS if available
```

Expected structural gains:

```text
command_executor worker tasks: 2 -> 0 (default baseline)
worker stacks: removed
worker dispatch_result buffers: removed
device request semaphores: removed
legacy queue/context-switch path: removed
```

Không hard-code exact byte saving làm acceptance nếu chưa đo on-target; ghi measured values.

### 13.5 Payload qualification

Measure `list_devices`:

```text
1 device / 1 control
4 devices / 4 controls
max device count
truncation case
```

Không được fail toàn response chỉ vì control hints nhiều; phải truncate deterministic.

## Test xác nhận

```bash
idf.py fullclean
idf.py build
cd test
idf.py fullclean
idf.py build
```

Hardware:

```bash
idf.py flash monitor
```

Soak/stress nếu có:

```text
repeated MCP set
Web + MCP concurrent use
BLE reconnect loops
WS events while commands complete
```

## Checklist

- [ ] Clean production build pass
- [ ] Clean test build pass
- [ ] ESP32-S3 hardware boot pass
- [ ] Exactly 3 compact MCP tools
- [ ] 2-call first control pass
- [ ] 1-call repeated control pass
- [ ] `describe(device)` without feature pass
- [ ] INT direct set from list hint pass
- [ ] Web CRUD pass
- [ ] Web command pass
- [ ] ACK exactly-once pass
- [ ] Timeout/disconnect/late ACK pass
- [ ] No legacy task/symbol/component
- [ ] Memory before/after recorded
- [ ] `list_devices` payload bounded
- [ ] Active docs updated

---

# PHASE 14 - OPTIONAL Compact-Only MCP Cleanup

## Tổng quan

Chỉ thực hiện nếu sản phẩm quyết định **không còn cần dynamic MCP compatibility**. Phase này không phải điều kiện để xóa command stack.

## File cần sửa/xóa

```text
components/mcp_tool_exposure/mcp_tool_catalog.c
components/mcp_tool_exposure/mcp_tool_name.c
components/mcp_endpoint/Kconfig.projbuild
components/mcp_endpoint/mcp_registry.c
components/web_server/web_exposure_api.c
components/web_server/www_src/dashboard/js/features/mcp_exposure.js
```

## Sửa cái gì

Nếu compact-only thật sự là product contract:

```text
remove dynamic tool catalog
remove generated tool names
remove raw command exposure UI
retain only feature grants + health + digest/policy data needed by device_control
```

Không làm phase này nếu vẫn cần backward compatibility với dynamic surface.

## Test xác nhận

```text
tools/list exactly 3 under all supported configs
Web exposure UI/API still controls feature grants
no raw command/tool-name persistence required
migration from existing exposure records defined
```

## Checklist

- [ ] Product decision compact-only được xác nhận
- [ ] Migration strategy cho stored exposure records có tài liệu
- [ ] Dynamic Kconfig/path removed only when safe
- [ ] MCP/Web tests pass
- [ ] No regression to `device_control`

---

## 5. API contracts đề xuất

### 5.1 `device_command_service`

```c
typedef enum {
    DEVICE_CMD_ORIGIN_CONTROL = 0,
    DEVICE_CMD_ORIGIN_SCHEMA_DISCOVERY,
    DEVICE_CMD_ORIGIN_STATE_READ,
} device_command_origin_t;

typedef struct {
    device_command_origin_t origin;
    char device_id[DEVICE_ID_MAX_LEN];

    /* internal protocol command; callers outside domain/MCP adapter should
       not treat this as a public tool identity */
    char command[DEVICE_COMMAND_MAX_LEN];

    bool has_feature_id;
    char feature_id[DEVICE_FEATURE_ID_MAX_LEN];

    bool has_property_id;
    uint8_t property_id;

    bool has_bool_value;
    bool bool_value;

    bool has_int_value;
    int32_t int_value;
} device_command_request_t;
```

Important:

```text
MCP/Web public contract = semantic/typed
protocol command string = internal service/wire concern
```

### 5.2 `device_management`

```c
device_mgmt_status_t device_management_add(
    const device_mgmt_add_request_t *request,
    device_mgmt_add_result_t *result);

device_mgmt_status_t device_management_edit(
    const device_mgmt_edit_request_t *request,
    device_mgmt_edit_result_t *result);

device_mgmt_status_t device_management_delete(
    const char *device_id,
    device_mgmt_delete_result_t *result);

device_mgmt_status_t device_management_snapshot(
    device_inventory_entry_t *out,
    size_t capacity,
    size_t *count);
```

### 5.3 MCP semantic helper

```c
esp_err_t mcp_semantic_get_control_hints(
    const char *device_id,
    mcp_control_hint_t *out,
    size_t capacity,
    size_t *count,
    bool *truncated);
```

---

## 6. Dependency graph cuối cùng

```text
                 device_types
                      |
        +-------------+-------------+
        |             |             |
  device_store   device_schema   device_state
        |             |             |
        +------- device_management--+
                      |
                      +----> web_server
                      |
                      +----> mcp_endpoint

  device_schema --------------------+
  device_state ---------------------+--> mcp_semantic_control
  mcp_tool_exposure ----------------+

  device_schema ---> device_command_service <--- device_state seed
                           |
                           v
                       ble_central
                           |
                           v
                       cbor_codec
```

Không có dependency node:

```text
command_executor
command_dispatcher
```

---

## 7. Delete/lifecycle safety rules

1. Device removal phải cancel pending command trước khi record bị mất.
2. Schema/state cleanup phải có deterministic order.
3. MCP exposure cleanup là lifecycle subscriber/best-effort cleanup, không phải authorization source duy nhất.
4. MCP SET luôn re-resolve device + schema + exposure health; stale exposure không đủ để cho phép SET.
5. Delete degraded result phải surfaced cho Web test/log; không silently báo success khi persistence delete fail.
6. CRUD event chỉ publish sau khi mutation state đủ rõ để consumer refresh.

---

## 8. Error mapping guidance

### Device command -> MCP

| Device status | MCP behavior |
|---|---|
| INVALID_ARGUMENT | `isError=true` |
| UNSUPPORTED_COMMAND | `isError=true` |
| TYPE_MISMATCH | `isError=true` |
| RANGE_ERROR | `isError=true` |
| NOT_CONNECTED | `isError=true` |
| BUSY | `isError=true`, retryable text |
| TIMEOUT | `isError=true` |
| REJECTED | `isError=true` |
| OK | `isError=false` |

### Device management -> HTTP

| Status | Suggested HTTP |
|---|---:|
| OK | 200/201/204 |
| INVALID_ARG | 400 |
| NOT_FOUND | 404 |
| CONFLICT | 409 |
| CAPACITY | 507 or 409/503 per existing API policy |
| BUSY | 409/503 |
| DEGRADED | 200 with explicit degraded body, or 500 if store mutation failed |
| INTERNAL | 500 |

Giữ API compatibility hiện tại nếu frontend phụ thuộc status code cụ thể; test quyết định final mapping.

---

## 9. Required zero-reference gates

### Gate A - external consumer, trước delete

```bash
git grep -nE \
'command_executor|command_dispatcher|dispatch_result_t|command_registry_|device_request_manager' \
-- \
':!components/command_executor/**' \
':!components/command_dispatcher/**' \
':!docs/**'
```

Expected:

```text
0 matches
```

### Gate B - config consumer, trước delete

```bash
git grep -nE 'CONFIG_CMD_EXEC_' -- \
':!components/command_executor/**' \
':!docs/**'
```

Expected:

```text
0 matches
```

### Gate C - full implementation, sau delete

```bash
git grep -nE \
'command_executor|command_dispatcher|dispatch_result_t|command_registry_|device_request_manager|CONFIG_CMD_EXEC_' \
-- ':!docs/**'
```

Expected:

```text
0 implementation/config matches
```

---

## 10. Recommended commit sequence

Mỗi phase nên là một commit/PR checkpoint có build/test pass:

```text
refactor(types): add protocol-independent device types
refactor(device-cmd): harden authoritative validation and ack lifecycle
refactor(device-mgmt): add typed device lifecycle service
refactor(web): migrate device CRUD off command stack
refactor(web): remove legacy command path
refactor(mcp): add shared semantic-control resolver
refactor(mcp): migrate status/list devices to typed domains
refactor(mcp): finalize device_control direct service flow
refactor(mcp): remove dispatcher/executor execution modes
refactor(main): remove legacy init and ack fallback
refactor(cleanup): remove remaining command-stack consumers
refactor(cleanup): delete command_executor and command_dispatcher
chore(qualification): verify memory and end-to-end flows
```

Không gom toàn bộ refactor thành một commit lớn.

---

## 11. Rollback strategy

Trước Phase 12, rollback đơn giản bằng revert phase gây regression vì legacy directories vẫn còn.

Sau Phase 12, rollback nên revert toàn commit delete + dependent migration commit, không reintroduce compatibility shim thủ công.

Nếu hardware E2E fail ở Phase 13:

```text
1. identify owning domain
2. fix new typed path
3. do not restore executor/dispatcher as workaround
```

---

## 12. Definition of Done cuối cùng

- [ ] `components/command_executor/` không tồn tại
- [ ] `components/command_dispatcher/` không tồn tại
- [ ] `device_request_manager` không tồn tại
- [ ] `dispatch_result_t` không tồn tại trong production
- [ ] `CONFIG_CMD_EXEC_*` không tồn tại trong active config
- [ ] `command_registry` không còn production role
- [ ] `device_command_service` là sole ACK owner
- [ ] CONTROL request được schema/type/range validate trong service
- [ ] Internal schema/state origins strict allowlist
- [ ] Web `/api/devices` direct typed `device_management`
- [ ] Web `/api/command` direct `device_command_service`
- [ ] MCP compact đúng 3 tools
- [ ] MCP `get_status` direct `gateway_status`
- [ ] MCP `list_devices` direct typed inventory + semantic hints
- [ ] Shared semantic helper được reuse bởi `list_devices` và `device_control`
- [ ] `describe(device)` không yêu cầu feature
- [ ] Common BOOL control <= 2 MCP calls
- [ ] Common INT control <= 2 MCP calls với range/step hint
- [ ] Repeated SET có thể 1 MCP call
- [ ] No raw BLE command leak trong compact MCP result
- [ ] No duplicate-name/feature first-match behavior
- [ ] Device CRUD không chờ BLE ACK/connect
- [ ] Delete degraded semantics có test
- [ ] ACK/timeout/disconnect/late/duplicate tests pass
- [ ] Clean ESP-IDF production build pass
- [ ] Clean test build pass
- [ ] ESP32-S3 hardware E2E pass
- [ ] Memory/task count được so sánh trước/sau
- [ ] Active docs phản ánh architecture mới

---

## 13. Kiến trúc cuối cùng cần nhìn thấy trong code

```text
MCP get_status
   -> gateway_status

MCP list_devices
   -> device_management
   -> mcp_semantic_control

MCP device_control
   -> mcp_semantic_control
      -> describe -> device_schema
      -> read     -> device_state
      -> set      -> device_command_service -> BLE

Web /api/devices
   -> device_management

Web /api/command
   -> device_command_service

BLE notify
   -> schema/state observers
   -> device_command_service ACK owner
```

Và không còn bất kỳ runtime flow nào đi qua:

```text
command_executor
command_dispatcher
command_registry
device_request_manager
dispatch_result_t
```

---

**End of Plan v1.1**
