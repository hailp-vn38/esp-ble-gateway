# ESP32 BLE Gateway — MCP Compact 2-Call Control Flow v1.0

**Repository:** `hailp-vn38/esp-ble-gateway`  
**Target branch:** `dev-ws`  
**Target:** ESP32-S3, BLE Protocol v4, Web UI, MCP HTTP/WS, Xiaozhi  
**Date:** 2026-09-04

---

# 1. Mục tiêu

Giữ đúng 3 MCP tools:

```text
get_status
list_devices
device_control
```

Nhưng flow điều khiển phổ biến phải là:

```text
First control:
list_devices -> device_control(set)
```

Tối đa 2 tool calls.

Khi agent đã biết `device_id + feature_id + value_type`:

```text
device_control(set)
```

chỉ còn 1 tool call.

`device_control(describe)` vẫn tồn tại cho discovery chi tiết, nhưng không phải bước bắt buộc trước mọi SET.

---

# 2. Flow đích

## 2.1 First-use control

```text
User: "Bật TEST"
        |
        v
list_devices
        |
        | trả device_id + controls[]
        v
Agent chọn:
  device_id
  feature_id
  value_type
        |
        v
device_control(set)
        |
        v
MCP policy
        |
        v
device_schema_validate_command()
        |
        v
device_command_service
        |
        v
BLE -> ACK
        |
        v
MCP CallToolResult
```

## 2.2 Subsequent control

```text
User: "Tắt TEST"
        |
        v
device_control(set)
        |
        v
BLE
```

Gateway vẫn phải validate lại toàn bộ policy/schema; không tin tuyệt đối context cũ của agent.

---

# 3. Vai trò của `list_devices`

`list_devices` phải làm hai việc:

```text
1. Inventory
2. Control hints
```

Không trả full schema.  
Không trả raw BLE commands.  
Không trả dynamic MCP tool names.

Response đề xuất:

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
        "value_type": "bool",
        "writable": true
      }
    ]
  }
]
```

`controls[]` chỉ chứa semantic write hints đủ để agent gọi `device_control(set)` trực tiếp.

---

# 4. BOOL control hint

```json
{
  "feature_id": "relay_1",
  "semantic_name": "relay",
  "property": "on_off",
  "value_type": "bool",
  "writable": true
}
```

Agent dùng `feature_id` làm primary identity.

---

# 5. INT control hint

Với INT feature, thêm constraint để không cần `describe` trước SET:

```json
{
  "feature_id": "brightness",
  "semantic_name": "light",
  "property": "level",
  "value_type": "int",
  "writable": true,
  "minimum": 0,
  "maximum": 100,
  "step": 5
}
```

---

# 6. Không đưa read-only feature vào `controls[]`

Read-only sensor vẫn được discover qua:

```text
device_control(operation="describe")
```

`controls[]` chỉ biểu diễn:

```text
"AI có thể thử điều khiển feature này"
```

---

# 7. Filtering `controls[]`

Khuyến nghị chỉ đưa feature vào `controls[]` khi:

```text
feature writable
AND semantic mapping trusted
AND exposure record exists
AND control_enabled == true
AND state == MCP_EXPOSURE_ENABLED
AND capability digest matches
AND destructive policy allows
```

Policy vẫn phải được re-check khi SET.

---

# 8. `device_control` input schema

```json
{
  "type": "object",
  "properties": {
    "device": {"type": "string"},
    "operation": {
      "type": "string",
      "enum": ["describe", "read", "set"]
    },
    "feature": {"type": "string"},
    "bool_value": {"type": "boolean"},
    "int_value": {"type": "integer"}
  },
  "required": ["device", "operation"],
  "additionalProperties": false
}
```

---

# 9. Operation semantics

## `describe`

```text
device  : required
feature : optional
```

Không có feature -> trả toàn bộ semantic features.

Có feature -> trả feature cụ thể.

## `read`

```text
device  : required
feature : required
```

Read từ `device_state` cache.

## `set`

```text
device  : required
feature : required
typed value required
```

---

# 10. `describe` response

```json
{
  "device_id": "AC:27:6E:CC:F2:26",
  "name": "TEST",
  "features": [
    {
      "feature_id": "relay_1",
      "semantic_name": "relay",
      "property": "on_off",
      "value_type": "bool",
      "writable": true
    },
    {
      "feature_id": "temperature",
      "semantic_name": "temperature",
      "property": "temperature",
      "value_type": "int",
      "writable": false
    }
  ]
}
```

Không trả raw command/tool_name.

---

# 11. SET flow

```text
device_control(set)
        |
        v
resolve device
        |
        v
resolve feature
        |
        v
writable?
        |
        v
exposure exists?
        |
        v
control_enabled?
        |
        v
health == ENABLED?
        |
        v
digest matches?
        |
        v
destructive policy?
        |
        v
typed value correct?
        |
        v
device_schema_validate_command()
        |
        v
device_command_service_submit()
        |
        v
BLE
```

Bất kỳ lỗi nào trước BLE:

```text
BLE send count == 0
```

---

# 12. Vì sao `list_devices` cần `controls[]`

Nếu chỉ trả:

```json
{
  "device_id": "...",
  "name": "...",
  "connected": true
}
```

agent chưa biết:

```text
feature_id
value_type
range
```

nên phải:

```text
list_devices -> describe -> set
```

3 calls.

Có `controls[]`:

```text
list_devices -> set
```

2 calls.

---

# 13. Vì sao không đưa full schema vào `list_devices`

Không nên trả toàn bộ `features[] + tools[]` cho mọi device vì:

```text
- payload tăng theo device_count x feature_count
- buffer ESP32 giới hạn
- MCP WS payload lớn
- duplicate với describe
- dễ leak raw command
- agent phải parse quá nhiều metadata
```

`controls[]` phải là summary nhỏ.

---

# 14. Payload budget

Khuyến nghị:

```text
MAX_CONTROL_HINTS_PER_DEVICE = 4
```

Nếu device có nhiều hơn:

```json
{
  "controls_truncated": true
}
```

và agent fallback sang `describe`.

Không để toàn bộ `list_devices` fail chỉ vì một device có quá nhiều features.

---

# 15. Deterministic ordering

`controls[]` phải deterministic theo committed feature order.

Không sort theo dynamic catalog/tool name/hash.

---

# 16. Identity rules

Primary identity:

```text
device_id
feature_id
```

Configured `name` và `semantic_name` chỉ là human/LLM hints.

Nếu duplicate configured name hoặc semantic alias:

```text
fail closed
```

không chọn first-match.

---

# 17. Tool descriptions cho Xiaozhi/LLM

## `list_devices`

```text
List devices known by the gateway, including semantic control hints.
Use a returned device_id and controls[].feature_id with device_control
to set a feature directly. Call device_control describe only when more
feature details are required.
```

## `device_control`

```text
Describe, read, or set semantic device features.
For set, prefer device_id and feature_id returned by list_devices.
Call describe with only device to discover all semantic features.
```

---

# 18. Example — relay 2 calls

User:

```text
"Bật TEST"
```

Call 1:

```text
list_devices
```

Response:

```json
[
  {
    "device_id": "AC:27:6E:CC:F2:26",
    "name": "TEST",
    "connected": true,
    "controls": [
      {
        "feature_id": "relay_1",
        "semantic_name": "relay",
        "value_type": "bool"
      }
    ]
  }
]
```

Call 2:

```json
{
  "device": "AC:27:6E:CC:F2:26",
  "operation": "set",
  "feature": "relay_1",
  "bool_value": true
}
```

---

# 19. Example — repeated control 1 call

```json
{
  "device": "AC:27:6E:CC:F2:26",
  "operation": "set",
  "feature": "relay_1",
  "bool_value": false
}
```

Không cần gọi lại `list_devices` nếu agent vẫn giữ context.

---

# 20. Example — dimmable light

`list_devices`:

```json
{
  "device_id": "light-01",
  "name": "Living Room",
  "controls": [
    {
      "feature_id": "main_light",
      "semantic_name": "light",
      "property": "level",
      "value_type": "int",
      "minimum": 0,
      "maximum": 100,
      "step": 5
    }
  ]
}
```

SET:

```json
{
  "device": "light-01",
  "operation": "set",
  "feature": "main_light",
  "int_value": 50
}
```

---

# 21. Example — complex discovery

Nếu:

```json
{
  "controls_truncated": true
}
```

flow:

```text
list_devices
    -> device_control(describe)
    -> device_control(set/read)
```

3 calls chỉ khi thật sự cần thêm discovery.

---

# 22. Offline behavior

Có thể có:

```json
{
  "connected": false,
  "capabilities": {
    "available": true,
    "state": "ready"
  }
}
```

Rules:

```text
describe -> allowed
read     -> cache policy
set      -> NOT_CONNECTED
```

Committed schema không phụ thuộc active BLE link.

---

# 23. Helper API đề xuất

Không duplicate logic exposure/template trong `gateway_commands.c`.

Tạo helper semantic:

```c
typedef struct {
    char feature_id[GW_FEATURE_ID_LEN];
    char semantic_name[24];
    char property_name[24];

    uint8_t value_type;

    bool has_min;
    int32_t min_value;

    bool has_max;
    int32_t max_value;

    bool has_step;
    uint32_t step;
} mcp_control_hint_t;
```

API:

```c
esp_err_t mcp_semantic_control_get_hints(
    const char *device_id,
    mcp_control_hint_t *out,
    size_t capacity,
    size_t *out_count,
    bool *out_truncated);
```

Helper chịu trách nhiệm:

```text
schema + semantic mapping + exposure health + digest + policy
```

`list_devices` chỉ serialize kết quả.

---

# 24. Separation of responsibilities

```text
device_schema
    = capability truth

mcp_tool_exposure
    = write authorization + health

mcp semantic helper
    = AI-safe control hints

list_devices
    = inventory + controls[]

device_control
    = semantic execution
```

---

# 25. Files cần chỉnh

```text
components/command_dispatcher/gateway_commands.c
components/mcp_endpoint/mcp_device_control.c
components/mcp_endpoint/mcp_registry.c
components/mcp_endpoint/mcp_policy.c
components/mcp_tool_exposure/mcp_tool_exposure.c
```

Nếu sau refactor đã có `gateway_admin_service`, chuyển `list_devices` logic sang service đó thay vì dispatcher.

Nên thêm module/helper:

```text
components/mcp_endpoint/mcp_semantic_control.c
```

hoặc một component semantic riêng nếu muốn reuse từ Web.

---

# 26. Test `list_devices`

## BOOL writable

Expected `controls[]` có:

```text
feature_id
semantic_name
property
value_type=bool
```

## INT writable

Expected:

```text
minimum
maximum
step
```

## Read-only

Không xuất hiện trong `controls[]`.

## Disabled / NEEDS_REVIEW / digest mismatch

Không xuất hiện trong usable `controls[]`.

## No committed schema

Expected:

```json
{
  "capabilities": {
    "available": false,
    "feature_count": 0,
    "writable_feature_count": 0
  },
  "controls": []
}
```

---

# 27. Integration test bắt buộc — 2-call flow

Setup:

```text
device name = TEST
device_id = dev-1
feature_id = relay_1
semantic = relay
value_type = bool
writable = true
exposure = enabled + healthy
```

Call 1:

```text
list_devices
```

Assert:

```text
TEST tồn tại
device_id == dev-1
controls.length == 1
controls[0].feature_id == relay_1
controls[0].value_type == bool
```

Call 2:

```json
{
  "device": "dev-1",
  "operation": "set",
  "feature": "relay_1",
  "bool_value": true
}
```

Assert trước ACK:

```text
device_command_service submit count == 1
BLE send count == 1
```

Mock ACK.

Assert:

```text
MCP response count == 1
isError == false
```

---

# 28. Integration test — 1-call repeat

Sau test trên gọi trực tiếp:

```json
{
  "device": "dev-1",
  "operation": "set",
  "feature": "relay_1",
  "bool_value": false
}
```

Không gọi `list_devices`.

Expected success.

---

# 29. Xiaozhi acceptance criteria

Phải chứng minh:

```text
1. tools/list -> exactly 3 tools
2. list_devices -> trả semantic control hints
3. Xiaozhi chọn được device_id + feature_id
4. device_control(set) success
5. simple relay/light/fan không cần mandatory describe
```

Pass:

```text
simple control intent <= 2 MCP tool calls
```

---

# 30. Memory/payload acceptance

Measure:

```text
1 device / 1 control
4 devices / 4 controls
maximum configured devices
```

Record:

```text
list_devices payload bytes
free heap
minimum free heap
largest internal block
PSRAM free
```

Nếu payload vượt budget:

```text
truncate controls deterministically
set controls_truncated=true
```

Không fail toàn bộ list.

---

# 31. Implementation order

```text
Phase 1  Fix device_control describe semantics
Phase 2  Add semantic control-hint helper
Phase 3  Add controls[] to list_devices
Phase 4  Filter controls by exposure/policy health
Phase 5  Update MCP tool descriptions
Phase 6  Add 2-call integration tests
Phase 7  Add Xiaozhi acceptance test
Phase 8  Measure payload + heap on ESP32-S3
```

---

# 32. Definition of Done

- [ ] Compact MCP vẫn đúng 3 tools.
- [ ] `list_devices` có `controls[]`.
- [ ] `controls[]` không chứa raw BLE commands.
- [ ] BOOL control thực hiện được trong 2 calls.
- [ ] INT control thực hiện được trong 2 calls.
- [ ] `describe` không bắt buộc cho common SET.
- [ ] `describe` vẫn trả full semantic feature metadata.
- [ ] Read-only feature không bị quảng bá như writable control.
- [ ] Disabled/NEEDS_REVIEW/digest mismatch không quảng bá là usable control.
- [ ] SET luôn revalidate policy + schema.
- [ ] `device_id` và `feature_id` là primary identity.
- [ ] `controls[]` bounded/truncatable.
- [ ] Xiaozhi simple control hoàn thành trong <= 2 MCP tool calls.
- [ ] Repeated control có thể hoàn thành trong 1 tool call.
- [ ] ESP32-S3 memory/payload tests pass.
