# ESP32 BLE Gateway — MCP `device_control` Fix Implementation Plan v1.0

**Repository:** `hailp-vn38/esp-ble-gateway`  
**Target branch:** `dev-ws`  
**Reviewed HEAD:** `f09321d2e010e3fbea19d071bc8c83bd90bfbcb8`  
**Reference:** `docs/ESP32_BLE_GATEWAY_MCP_COMPACT_SEMANTIC_CONTROL_IMPLEMENTATION_PLAN_v1.3.md`  
**Date:** 2026-09-04

> Không chuyển sang step tiếp theo cho đến khi test của step hiện tại pass. Kết quả `347/347 pass` hiện tại chưa đủ vì test suite chưa chạy end-to-end `device_control describe/read/set`.

---

# 1. Mục tiêu

Compact MCP phải có đúng:

```text
get_status
list_devices
device_control
```

Flow đích:

```text
tools/call
  -> device_control
      -> describe : local semantic result
      -> read     : device_state cache
      -> set
          -> resolve device/feature
          -> MCP write policy
          -> device_schema validation
          -> device_command_service_submit()
          -> BLE
          -> ACK
          -> semantic MCP CallToolResult
```

Không được còn:

```text
(cJSON *)-1 sentinel
raw BLE command leakage
hidden dynamic tools trong compact mode
missing exposure => allow
NEEDS_REVIEW tự chuyển ENABLED
first-match khi device/feature alias trùng
device_control tự dựng JSON-RPC envelope
```

---

# 2. Thứ tự sửa

```text
Step 1  Sửa async execution contract
Step 2  Chuẩn hóa MCP CallToolResult
Step 3  Sửa semantics của describe
Step 4  Sửa resolver ambiguity
Step 5  Sửa inputSchema
Step 6  Sửa semantic exposure record identity
Step 7  Centralize write policy và fail closed
Step 8  Sửa reconcile + durable disable
Step 9  Sửa Web exposure API
Step 10 Khôi phục schema validation tại device_command_service
Step 11 Đóng hidden dynamic executable surface
Step 12 Chọn đúng compact build profile
Step 13 Thêm integration/conformance tests
Step 14 Test trên ESP32-S3 + BLE device thật
```

Không bật `set` thực tế trước khi Step 1, 2, 6, 7, 8 và 10 pass.

---

# 3. Step 1 — Bỏ async sentinel `(cJSON *)-1`

## Lỗi hiện tại

File:

```text
components/mcp_endpoint/mcp_device_control.c
```

`mcp_device_control_execute()` trả:

```c
return (cJSON *)(intptr_t)-1;
```

cho `set`, nhưng `mcp_core.c` chỉ nhận async khi:

```c
result == NULL
```

=> pointer `-1` có thể bị truyền vào cJSON/`send_result()`.

## Files cần sửa

```text
components/mcp_endpoint/mcp_device_control.c
components/mcp_endpoint/mcp_endpoint_internal.h
components/mcp_endpoint/mcp_core.c
```

## Cách sửa

Không dùng pointer sentinel.

Tạo execution plan explicit:

```c
typedef enum {
    MCP_DEVICE_CONTROL_EXEC_LOCAL = 0,
    MCP_DEVICE_CONTROL_EXEC_ASYNC_SET,
    MCP_DEVICE_CONTROL_EXEC_ERROR,
} mcp_device_control_exec_kind_t;

typedef struct {
    mcp_device_control_exec_kind_t kind;
    cJSON *local_result;
    device_command_request_t request;
    mcp_rpc_error_t error;
} mcp_device_control_plan_t;
```

API:

```c
esp_err_t mcp_device_control_resolve(
    const cJSON *params,
    const mcp_request_context_t *protocol,
    mcp_device_control_plan_t *out);
```

Mapping:

```text
describe -> LOCAL
read     -> LOCAL
set      -> ASYNC_SET
error    -> ERROR
```

`mcp_core.c` là owner duy nhất của responder clone/release và async lifetime.

Xóa toàn bộ:

```c
return (cJSON *)(intptr_t)-1;
```

## Test đúng

Gọi valid `set`.

Expected trước ACK:

```text
- không gửi JSON result ngay
- không crash
- request được queue đúng một lần
```

Sau mock ACK:

```text
- exactly one JSON-RPC response
- no invalid access
- no double completion
```

## Checklist

- [ ] Không còn sentinel pointer.
- [ ] Async kind là enum explicit.
- [ ] `mcp_core` sở hữu responder lifecycle.
- [ ] Valid `set` không crash.
- [ ] Completion đúng một lần.

---

# 4. Step 2 — Chuẩn hóa `CallToolResult`

## Lỗi hiện tại

`describe/read` trả raw semantic object.  
`set_completion()` tự dựng JSON-RPC envelope.

Ba operation đang dùng ba response path khác nhau.

## Files

```text
components/mcp_endpoint/mcp_device_control.c
components/mcp_endpoint/mcp_tools.c
components/mcp_endpoint/mcp_endpoint_internal.h
components/mcp_endpoint/mcp_core.c
```

## Cách sửa

Tạo formatter chung:

```c
cJSON *mcp_device_control_format_result(
    const cJSON *semantic_payload,
    bool is_error,
    const mcp_request_context_t *protocol,
    mcp_rpc_error_t *error);
```

Expected MCP `result`:

```json
{
  "content": [
    {
      "type": "text",
      "text": "{...}"
    }
  ],
  "isError": false,
  "structuredContent": {
    "device_id": "dev1",
    "feature_id": "led_main"
  }
}
```

MCP 2026 phải giữ thêm `resultType`, `_meta/serverInfo` theo formatter hiện có.

`mcp_device_control.c` không được tự tạo:

```json
{
  "jsonrpc": "2.0",
  "id": ...
}
```

JSON-RPC envelope chỉ do `mcp_core` tạo.

## Test đúng

Với `describe`, `read`, `set` đều assert:

```text
response.result.content is array
response.result.isError exists
response.result.structuredContent is object
```

Failure thuộc tool-domain:

```text
result.isError == true
```

Malformed RPC mới dùng JSON-RPC error.

## Checklist

- [ ] Một formatter cho cả 3 operations.
- [ ] `device_control` không tự dựng JSON-RPC envelope.
- [ ] Xiaozhi/2024 result hợp lệ.
- [ ] MCP 2026 metadata hợp lệ.
- [ ] Không có raw command trong compact response.

---

# 5. Step 3 — Sửa `describe`

## Lỗi hiện tại

Code resolve `feature` trước khi switch operation. Vì vậy:

```json
{
  "device": "Lamp",
  "operation": "describe"
}
```

bị `Feature not found`.

## File

```text
components/mcp_endpoint/mcp_device_control.c
```

## Semantics đúng

### describe

```text
device  : required
feature : optional
```

Không có feature => trả toàn bộ trusted semantic features.

Có feature => filter một feature.

### read

```text
device  : required
feature : required
```

### set

```text
device  : required
feature : required
exactly one typed value required
```

Describe result đề xuất:

```json
{
  "device_id": "dev1",
  "features": [
    {
      "feature_id": "led_main",
      "semantic_name": "light",
      "type": "on_off_light",
      "property": "on_off",
      "value_type": "bool",
      "writable": true
    }
  ]
}
```

Không trả:

```text
raw command
raw peripheral label
dynamic tool_name
```

## Test đúng

- `describe(device)` => toàn bộ features.
- `describe(device, feature_id)` => đúng 1 feature.
- Device disconnected nhưng có committed schema => vẫn success.
- Không có committed schema => `capabilities_not_ready`.
- Max 12 features không vượt WS TX budget.

## Checklist

- [ ] `describe` không cần feature.
- [ ] Feature filter optional.
- [ ] Không lộ raw command.
- [ ] Disconnected committed schema vẫn describe được.

---

# 6. Step 4 — Fail closed khi alias bị trùng

## Lỗi hiện tại

Resolver trả match đầu tiên khi:

```text
2 device có cùng configured name
2 feature có cùng semantic name
```

## File

```text
components/mcp_endpoint/mcp_device_control.c
```

## Cách sửa

Resolver status:

```c
typedef enum {
    MCP_SEM_RESOLVE_OK = 0,
    MCP_SEM_RESOLVE_NOT_FOUND,
    MCP_SEM_RESOLVE_AMBIGUOUS,
    MCP_SEM_RESOLVE_INVALID,
} mcp_sem_resolve_status_t;
```

Device priority:

```text
1. exact device_id
2. unique exact configured name
3. >1 match => AMBIGUOUS
```

Feature priority:

```text
1. exact feature_id
2. unique trusted semantic alias
3. >1 match => AMBIGUOUS
```

## Test đúng

Device store:

```text
dev-a -> Lamp
dev-b -> Lamp
```

`device="Lamp"` => `ambiguous_device`.

Features:

```text
ceiling -> semantic light
desk    -> semantic light
```

`feature="light"` => `ambiguous_feature`.

`feature="desk"` => success.

## Checklist

- [ ] Không còn first-match alias.
- [ ] Exact ID ưu tiên.
- [ ] Ambiguous luôn deny.

---

# 7. Step 5 — Sửa `device_control.inputSchema`

## File

```text
components/mcp_endpoint/mcp_registry.c
```

## Cách sửa

Phải có:

```json
{
  "required": [
    "device",
    "operation"
  ],
  "additionalProperties": false
}
```

Description:

```text
feature:
  optional for describe
  required for read/set

bool_value:
  BOOL set only

int_value:
  INT set only
```

Không tạo dynamic enums từ device/features.

## Test đúng

Từ `tools/list` assert:

```text
required == {device, operation}
operation enum == {describe, read, set}
additionalProperties == false
```

## Checklist

- [ ] Required fields đúng.
- [ ] Schema cố định, không tăng theo device count.

---

# 8. Step 6 — Sửa semantic exposure record identity

## Lỗi hiện tại

Semantic record có thể:

```text
feature_id != ""
flags = 0
```

nhưng `mcp_tool_exposure_get_feature()` lại yêu cầu:

```text
MCP_EXP_FLAG_FEATURE_BOUND
```

Flag này đang bị trộn với ý nghĩa legacy/raw command hiding.

## Files

```text
components/mcp_tool_exposure/mcp_tool_exposure.c
components/mcp_tool_exposure/mcp_tool_exposure_internal.h
components/mcp_tool_exposure/include/mcp_tool_exposure.h
```

## Cách sửa

Semantic record identity:

```text
device_id + non-empty feature_id
```

Không phụ thuộc `FEATURE_BOUND`.

Helper:

```c
static bool is_semantic_feature_record(
    const mcp_exposure_persisted_record_t *rec)
{
    return rec->feature_id[0] != '\0';
}
```

`mcp_tool_exposure_get_feature()` phải lookup bằng:

```text
device_id
feature_id
```

và trả:

```text
control_enabled
state
reason
capability_digest
```

## Test đúng

Record:

```text
device_id=dev1
feature_id=led_main
flags=0
```

Expected:

```c
mcp_tool_exposure_get_feature(...) == ESP_OK
```

Set `USER_DISABLED`:

```text
control_enabled == false
```

health state vẫn độc lập.

## Checklist

- [ ] Feature lookup không phụ thuộc raw hide flag.
- [ ] User intent độc lập với health.
- [ ] NVS layout giữ compatibility nếu có thể.

---

# 9. Step 7 — Centralize feature write policy và fail closed

## Lỗi hiện tại

`mcp_device_control.c` duplicate policy.

Nếu `mcp_tool_exposure_get_feature()` trả NOT_FOUND thì current code tiếp tục allow.

## Files

```text
components/mcp_endpoint/mcp_policy.c
components/mcp_endpoint/mcp_device_control.c
components/mcp_endpoint/mcp_endpoint_internal.h
```

## Cách sửa

Chỉ dùng:

```c
mcp_policy_check_feature_control(...)
```

Check order:

```text
1. device exists
2. committed schema exists
3. feature exists
4. writable_tool_index valid
5. exposure record exists
6. control_enabled == true
7. state == MCP_EXPOSURE_ENABLED
8. digest matches
9. destructive policy passes
```

Critical:

```text
NO EXPOSURE RECORD => DENY
```

Không còn default allow.

## Test đúng

Mỗi case phải kiểm tra **BLE send count == 0**:

```text
no exposure record
USER_DISABLED
NEEDS_REVIEW
ORPHANED
digest mismatch
destructive denied
```

Healthy enabled record mới được submit BLE.

## Checklist

- [ ] Chỉ có một feature-control policy authority.
- [ ] Missing record deny.
- [ ] Policy denial không gửi BLE.

---

# 10. Step 8 — Sửa reconcile và durable disable

## Lỗi hiện tại

Semantic reconcile có thể:

```text
NEEDS_REVIEW -> ENABLED
```

tự động.

Legacy disable delete persisted record, reconcile sau đó có thể recreate enabled.

## Files

```text
components/mcp_tool_exposure/mcp_tool_exposure.c
components/mcp_tool_exposure/mcp_tool_exposure_store.c
```

## Rule đúng

Hai dimension độc lập:

```text
health:
  ENABLED
  NEEDS_REVIEW
  ORPHANED

user intent:
  enabled
  disabled
```

User intent dùng:

```text
MCP_EXP_FLAG_USER_DISABLED
```

Digest change:

```text
state = NEEDS_REVIEW
giữ USER_DISABLED nguyên trạng
```

Reconcile không được tự chuyển `NEEDS_REVIEW -> ENABLED`.

Disable feature dùng:

```c
mcp_tool_exposure_set_feature_enabled(
    device_id,
    feature_id,
    false);
```

Không delete semantic record.

## Test đúng

### disable + reconcile

Expected:

```text
control_enabled == false
```

### disable + simulated reboot

Expected:

```text
control_enabled == false
```

### digest change

Expected:

```text
state == NEEDS_REVIEW
set denied
```

### reconcile lại với same digest

Expected:

```text
vẫn NEEDS_REVIEW
```

cho đến explicit review/enable.

## Checklist

- [ ] Disable survive reconcile.
- [ ] Disable survive reboot.
- [ ] NEEDS_REVIEW không auto-enable.
- [ ] NVS state được preserve.

---

# 11. Step 9 — Sửa Web exposure API

## Lỗi hiện tại

Feature-mode Web PUT resolve feature -> raw command rồi gọi legacy:

```text
mcp_tool_exposure_enable()
mcp_tool_exposure_disable()
```

## File

```text
components/web_server/web_exposure_api.c
```

## Cách sửa

Request:

```json
{
  "device_id": "dev1",
  "feature_id": "led_main",
  "enabled": false
}
```

phải gọi trực tiếp:

```c
mcp_tool_exposure_set_feature_enabled(
    device_id,
    feature_id,
    enabled);
```

Compact GET dùng:

```text
policy_revision
```

không dùng catalog revision làm authority.

Sửa capacity bug:

```text
max_records = capacity.max_records
```

không phải `capacity.max_enabled`.

## Test đúng

PUT disable -> GET:

```json
"control_enabled": false
```

Reconcile -> GET vẫn false.

Simulated reboot -> GET vẫn false.

## Checklist

- [ ] Feature PUT dùng feature API.
- [ ] Không translate feature toggle thành legacy raw command toggle.
- [ ] `max_records` đúng.
- [ ] policy revision tăng khi policy thay đổi.

---

# 12. Step 10 — Khôi phục schema validation tại `device_command_service`

## Lỗi hiện tại

`DEVICE_CMD_ORIGIN_CONTROL` hiện chỉ check command không rỗng.

Điều này yếu hơn dispatcher cũ.

## Files

```text
components/device_command_service/device_command_service.c
components/device_command_service/include/device_command_service.h
```

## Cách sửa

CONTROL request:

```text
1. build gw_message_t
2. device_schema_validate_command()
3. map validation status
4. only then allocate pending/send BLE
```

Mapping:

```text
VALID                -> continue
VALID_ARGUMENT       -> INVALID_ARGUMENT
UNSUPPORTED_COMMAND  -> UNSUPPORTED_COMMAND
UNKNOWN              -> fail closed
```

INT phải được final validator enforce:

```text
minimum
maximum
step
```

BOOL phải reject wrong/missing type.

## Test đúng

Unknown command:

```text
status = UNSUPPORTED_COMMAND
BLE send count = 0
```

BOOL command + INT only:

```text
INVALID_ARGUMENT
BLE send count = 0
```

INT schema:

```text
min=0 max=100 step=10
```

value `15`:

```text
INVALID_ARGUMENT
send=0
```

value `20`:

```text
send=1
```

## Checklist

- [ ] CONTROL luôn qua schema validator.
- [ ] Web raw command không bypass schema.
- [ ] MCP semantic set không bypass final validation.
- [ ] Invalid request không gửi BLE.

---

# 13. Step 11 — Đóng hidden dynamic tools trong compact mode

## Lỗi hiện tại

Compact `tools/list` hide dynamic catalog nhưng `tools/call` resolver vẫn có thể lookup dynamic tool name.

`mcp_registry_find()` cũng có thể resolve `device_control` ở dynamic mode dù không list.

## Files

```text
components/mcp_endpoint/mcp_registry.c
components/mcp_endpoint/mcp_tools.c
components/mcp_tool_exposure/mcp_tool_exposure.c
```

## Cách sửa

Compact:

```text
registry executable:
  get_status
  list_devices
  device_control
```

Dynamic:

```text
registry executable:
  get_status
  list_devices
  dynamic catalog tools
```

Trong compact:

```c
#if CONFIG_MCP_TOOL_SURFACE_COMPACT
    /* no dynamic catalog lookup */
#else
    /* dynamic catalog lookup */
#endif
```

Exposure compact path không được phụ thuộc executable catalog cho policy persistence/Web control.

## Test đúng

Compact mode, gọi known legacy dynamic tool:

```text
unknown tool
BLE send=0
```

Dynamic mode, gọi `device_control` khi không list:

```text
unknown tool
```

## Checklist

- [ ] `tools/list` surface == executable surface.
- [ ] Compact không có hidden dynamic tools.
- [ ] Dynamic không có hidden `device_control`.

---

# 14. Step 12 — Chọn đúng compact build profile

## Files

```text
components/mcp_endpoint/Kconfig.projbuild
sdkconfig.defaults
test/sdkconfig.defaults
```

## Cách sửa

Nếu production là compact/Xiaozhi:

```text
CONFIG_MCP_TOOL_SURFACE_COMPACT=y
# CONFIG_MCP_TOOL_SURFACE_DYNAMIC is not set
```

Test compact cũng phải set explicit.

Giữ một dynamic compatibility build riêng.

## Test đúng

Clean build:

```bash
rm -rf build sdkconfig
idf.py build
grep MCP_TOOL_SURFACE sdkconfig
```

Expected:

```text
CONFIG_MCP_TOOL_SURFACE_COMPACT=y
```

Runtime:

```text
tools/list count == 3
```

## Checklist

- [ ] Clean build mặc định ra compact.
- [ ] Test profile giống production.
- [ ] Dynamic compatibility build vẫn compile.

---

# 15. Step 13 — Thêm test thật cho `device_control`

## Files

Create:

```text
components/mcp_endpoint/test/test_mcp_device_control.c
```

Update:

```text
components/mcp_endpoint/test/CMakeLists.txt
components/mcp_endpoint/test/test_mcp_xiaozhi.c
components/mcp_endpoint/test/test_mcp_conformance.c
test/sdkconfig.defaults
```

## Test matrix bắt buộc

### Tool surface

- [ ] compact `tools/list` exactly 3.
- [ ] Exact names: `get_status`, `list_devices`, `device_control`.
- [ ] Không dynamic tool.

### Describe

- [ ] `describe(device)` -> all features.
- [ ] `describe(device, feature_id)` -> one feature.
- [ ] disconnected + committed schema -> success.
- [ ] no schema -> deterministic error.
- [ ] no raw commands.

### Read

- [ ] BOOL property lấy `value_bool`.
- [ ] INT property lấy `value_int`.
- [ ] no cache -> `state_not_available`.
- [ ] unknown property type -> `unsupported_property`.

### Set BOOL

- [ ] semantic resolve.
- [ ] policy enabled.
- [ ] schema validation.
- [ ] exactly one BLE send.
- [ ] ACK completes.
- [ ] exactly one MCP response.
- [ ] no raw command in result.

### Set INT

- [ ] min accepted.
- [ ] max accepted.
- [ ] valid step accepted.
- [ ] below min rejected.
- [ ] above max rejected.
- [ ] bad step rejected.
- [ ] float rejected.

### Policy

- [ ] no record denied.
- [ ] disabled denied.
- [ ] NEEDS_REVIEW denied.
- [ ] ORPHANED denied.
- [ ] digest mismatch denied.
- [ ] destructive denied as configured.
- [ ] all denied cases have BLE send count 0.

### Resolver ambiguity

- [ ] duplicate device name -> ambiguous.
- [ ] duplicate semantic feature alias -> ambiguous.
- [ ] exact device_id wins.
- [ ] exact feature_id wins.

### Async lifecycle

- [ ] success response exactly once.
- [ ] timeout exactly once.
- [ ] disconnect exactly once.
- [ ] late ACK after timeout ignored.
- [ ] late ACK after disconnect ignored.
- [ ] notification sends no response.
- [ ] responder released exactly once.
- [ ] no context leak.

### Compact isolation

- [ ] known dynamic tool rejected.
- [ ] executable surface matches listed surface.

## Sửa Xiaozhi test

Thay:

```c
TEST_ASSERT_GREATER_THAN(0, cJSON_GetArraySize(tools));
```

bằng exact:

```text
array size == 3
```

và assert exact names.

Thêm Xiaozhi:

```text
device_control describe
device_control read
device_control set + mocked ACK
```

## Checklist

- [ ] Có `test_mcp_device_control.c`.
- [ ] Test check response shape, không chỉ return code.
- [ ] Test check BLE send count.
- [ ] Test check async cleanup.
- [ ] Test compact và dynamic riêng.

---

# 16. Step 14 — Test trên ESP32-S3 thật

## Setup

```text
Gateway:
  esp-ble-gateway dev-ws fixed branch

Peripheral:
  esp-ble-device reference/main

Client:
  Xiaozhi MCP WS bridge hoặc MCP client tương đương
```

## A. Boot

Expected:

```text
device_schema init
device_command_service init
MCP exposure init
BLE ready
MCP ready
```

## B. Connect device

Expected:

```text
device ready
describe_capabilities
schema committed
state seed
```

## C. tools/list

Expected exactly:

```text
get_status
list_devices
device_control
```

## D. describe

Request:

```json
{
  "device": "<device>",
  "operation": "describe"
}
```

Expected:

```text
features returned
no raw command names
```

## E. read

Known BOOL feature:

```text
cached value returned
```

## F. set

Request:

```json
{
  "device": "<device>",
  "operation": "set",
  "feature": "<feature_id>",
  "bool_value": true
}
```

Expected log:

```text
[SEND]
[ACK]
```

Expected:

```text
one valid MCP CallToolResult
physical device state changed
```

## G. Disable

Disable feature qua Web API.

Gọi lại set.

Expected:

```text
denied
không có [SEND]
device không đổi
```

## H. Reboot

Reboot gateway.

Gọi set lại.

Expected:

```text
vẫn denied
```

## I. Capability digest change

Thay capability metadata bên peripheral và rediscover.

Expected:

```text
health=needs_review
set denied
```

## J. Memory/stress

Run:

```text
100 describe
100 read
100 successful set
20 timeout/disconnect cases
```

Record:

```text
internal free heap
minimum free heap
largest internal block
PSRAM free
task stack high-water marks
```

Pass khi:

```text
không progressive heap loss
không stack overflow
không reset
không responder/context leak
không queue saturation bất thường
```

---

# 17. Commit sequence đề xuất

```text
1. fix(mcp): replace device_control async sentinel with explicit execution plan
2. fix(mcp): normalize device_control results to CallToolResult
3. fix(mcp): support device-level describe
4. fix(mcp): fail closed on ambiguous semantic resolution
5. fix(mcp): correct device_control input schema
6. fix(exposure): identify semantic records by feature id
7. fix(mcp): centralize feature write policy and deny missing grants
8. fix(exposure): preserve disabled and needs-review across reconcile
9. fix(web): use feature-level MCP write-control API
10. fix(device-command): restore schema validation at service boundary
11. fix(mcp): close hidden dynamic surface in compact mode
12. config(mcp): make compact profile explicit
13. test(mcp): add device_control and Xiaozhi integration coverage
14. test(hw): qualify device_control on ESP32-S3
```

---

# 18. Definition of Done

## Surface

- [ ] Compact `tools/list` exactly 3.
- [ ] Chỉ 3 tool đó executable.
- [ ] Dynamic mode không hidden `device_control`.

## Describe

- [ ] `describe(device)` trả toàn bộ semantic features.
- [ ] Không raw command leakage.

## Read

- [ ] BOOL/INT cache typing đúng.
- [ ] Missing state deterministic.

## Set

- [ ] Không pointer sentinel.
- [ ] Không tự build JSON-RPC envelope trong `mcp_device_control.c`.
- [ ] Final schema validation trước BLE send.
- [ ] Exactly one async owner.
- [ ] Exactly one completion.

## Policy

- [ ] Missing exposure deny.
- [ ] Disabled deny.
- [ ] NEEDS_REVIEW deny.
- [ ] Digest mismatch deny.
- [ ] Denied write gửi zero BLE packet.
- [ ] Disable survive reboot/reconcile.

## Resolver

- [ ] Duplicate names fail closed.
- [ ] Exact IDs deterministic.

## Web

- [ ] Feature toggle dùng feature-level API.
- [ ] Web/MCP cùng một policy state.

## Tests

- [ ] Dedicated device_control test.
- [ ] Xiaozhi exact 3 tools.
- [ ] Xiaozhi gọi được describe/read/set.
- [ ] Host/unit pass.
- [ ] Hardware pass.

---

# 19. Thứ tự file nên chỉnh

```text
1.  components/mcp_endpoint/mcp_endpoint_internal.h
2.  components/mcp_endpoint/mcp_device_control.c
3.  components/mcp_endpoint/mcp_core.c
4.  components/mcp_endpoint/mcp_tools.c
5.  components/mcp_endpoint/mcp_registry.c

6.  components/mcp_tool_exposure/mcp_tool_exposure.c
7.  components/mcp_tool_exposure/mcp_tool_exposure_internal.h
8.  components/mcp_endpoint/mcp_policy.c

9.  components/web_server/web_exposure_api.c

10. components/device_command_service/device_command_service.c

11. sdkconfig.defaults
12. test/sdkconfig.defaults

13. components/mcp_endpoint/test/test_mcp_device_control.c
14. components/mcp_endpoint/test/test_mcp_xiaozhi.c
15. components/mcp_endpoint/test/CMakeLists.txt
```

**Ưu tiên tuyệt đối:** Step 1.  
Không test BLE `set` thật trước khi Step 1, 2, 6, 7, 8 và 10 hoàn tất.
