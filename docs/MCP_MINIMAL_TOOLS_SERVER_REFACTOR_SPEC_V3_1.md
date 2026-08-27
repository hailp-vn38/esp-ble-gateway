# ESP32 BLE Gateway — Minimal MCP Tools Server Refactor Specification

**Document version:** 3.1  
**Status:** Implementation-ready  
**Target repository:** `hailp-vn38/esp-ble-gateway`  
**Target component:** `components/mcp_endpoint`  
**Target MCP revision:** `2026-07-28`  
**Target ESP-IDF:** `>= 6.1.0-rc1, < 6.2.0`

---

## 1. Mục tiêu

Refactor `components/mcp_endpoint` thành một **MCP Tools Server tối thiểu nhưng tương thích chuẩn MCP 2026-07-28**, phục vụ cho:

- AI agent.
- AI voice host.
- MCP client tiêu chuẩn.
- Web/MCP cùng điều khiển một tập device qua `command_executor`.
- Gateway chỉ đóng vai trò deterministic execution layer.
- Không triển khai đầy đủ toàn bộ MCP ecosystem.

Mục tiêu cuối cùng:

```text
AI / Voice / MCP Client
        |
        | Streamable HTTP
        | POST /mcp
        v
+------------------------------+
|        mcp_endpoint          |
|------------------------------|
| HTTP validation              |
| MCP routing headers          |
| required request _meta       |
| JSON-RPC validation          |
| server/discover              |
| tools/list                   |
| tools/call                   |
| MCP policy                   |
+--------------+---------------+
               |
               v
       command_executor
               |
               v
      command_dispatcher
               |
               v
          BLE Central
               |
               v
          BLE Device
```

Nguyên tắc chính:

> Implement the MCP wire contract required by the gateway use case, not the entire MCP ecosystem.

---

# 2. Phạm vi triển khai

## 2.1 Bắt buộc hỗ trợ

Gateway MUST hỗ trợ:

```text
POST /mcp

server/discover
tools/list
tools/call
JSON-RPC errors
MCP 2026-07-28 request metadata
Mcp-Method
Mcp-Name khi method yêu cầu
MCP-Protocol-Version
Bearer authentication
Host / Origin validation
rate limiting
async device execution
```

## 2.2 Không triển khai

Không implement:

```text
initialize
notifications/initialized
Mcp-Session-Id
resources
prompts
sampling
roots
logging
tasks
subscriptions
subscriptions/listen
MRTR
input_required
SSE response streaming
dynamic MCP extensions
trace propagation
custom MCP metadata
```

Nếu sau này có use case rõ ràng thì mở rộng riêng.

---

# 3. Quyết định về `_meta`

## 3.1 `_meta` có cần hay không?

**Có.**

Với MCP `2026-07-28`, request metadata là một phần của protocol.

Mỗi request MUST mang:

```json
{
  "_meta": {
    "io.modelcontextprotocol/protocolVersion": "2026-07-28",
    "io.modelcontextprotocol/clientCapabilities": {}
  }
}
```

`clientInfo`:

```json
"io.modelcontextprotocol/clientInfo": {
  "name": "some-client",
  "version": "1.0.0"
}
```

là optional/SHOULD và gateway không được phụ thuộc vào nó.

## 3.2 Gateway chỉ xử lý `_meta` tại transport/protocol layer

Không propagate `_meta` xuống:

```text
mcp_tools
command_executor
command_dispatcher
cbor_codec
ble_central
device
```

Boundary:

```text
HTTP request
    |
    v
mcp_protocol / mcp_endpoint
    |
    +-- validate required _meta
    |
    +-- discard metadata not needed
    |
    v
normalized MCP operation
    |
    v
domain command
```

## 3.3 Không dùng metadata để authorization

Không dùng:

```text
clientInfo.name
clientInfo.version
serverInfo
```

để:

- cấp quyền;
- chọn device;
- thay đổi command behavior;
- bypass policy;
- xác định trust.

Authentication/authorization phải dựa vào gateway policy và Bearer token.

---

# 4. Kiến trúc component sau refactor

Đề xuất:

```text
components/mcp_endpoint/
|
+-- CMakeLists.txt
+-- Kconfig.projbuild
+-- README.md
|
+-- include/
|   +-- mcp_endpoint.h
|
+-- mcp_endpoint.c
+-- mcp_protocol.c
+-- mcp_rpc.c
+-- mcp_registry.c
+-- mcp_tools.c
+-- mcp_policy.c
+-- mcp_auth.c
+-- mcp_transport.c
|
+-- mcp_endpoint_internal.h
|
+-- test/
    +-- test_mcp_protocol.c
    +-- test_mcp_endpoint.c
    +-- test_mcp_registry.c
    +-- test_mcp_policy.c
    +-- test_mcp_async.c
    +-- test_mcp_security.c
    +-- test_mcp_stress.c
```

Không bắt buộc phải tách tất cả file trong một commit. Có thể refactor dần.

---

# 5. Responsibility của từng module

## 5.1 `mcp_endpoint.c`

Chỉ làm orchestration:

```text
request
  -> auth/security gate
  -> transport metadata
  -> body read
  -> JSON parse
  -> protocol validation
  -> route operation
  -> response
```

Không chứa:

- tool schema builder;
- allowlist parsing;
- BLE execution;
- business policy;
- device capability logic.

---

## 5.2 `mcp_protocol.c`

Chịu trách nhiệm:

```text
MCP-Protocol-Version
Mcp-Method
Mcp-Name
request params._meta
header/body consistency
server/discover payload
cache hints
```

API đề xuất:

```c
typedef struct {
    char method[32];
    char name[64];

    char protocol_version[16];

    bool has_name;
    bool has_client_info;
} mcp_request_context_t;

typedef enum {
    MCP_PROTOCOL_OK = 0,
    MCP_PROTOCOL_INVALID_REQUEST,
    MCP_PROTOCOL_UNSUPPORTED_VERSION,
    MCP_PROTOCOL_HEADER_MISMATCH,
    MCP_PROTOCOL_INVALID_META,
} mcp_protocol_status_t;

mcp_protocol_status_t mcp_protocol_validate(
    httpd_req_t *req,
    const cJSON *root,
    mcp_request_context_t *ctx);

cJSON *mcp_protocol_build_discovery(void);
```

Không lưu toàn bộ `_meta` tree trong context.

---

## 5.3 `mcp_transport.c`

Wrapper duy nhất quanh `esp_http_server`.

Responsibilities:

```text
recv
send
send status
headers
async_begin
async_complete
```

Giữ test seam để unit test không cần chạy HTTP server thật.

---

## 5.4 `mcp_rpc.c`

Chỉ phụ trách JSON-RPC envelope.

Ví dụ:

```json
{
  "jsonrpc": "2.0",
  "id": 10,
  "result": {}
}
```

hoặc:

```json
{
  "jsonrpc": "2.0",
  "id": 10,
  "error": {
    "code": -32602,
    "message": "Invalid params"
  }
}
```

Không tự động nhét custom `_meta` vào top-level response.

---

## 5.5 `mcp_registry.c`

Single source of truth cho MCP tools.

Registry quyết định:

- tên tool;
- description;
- input schema;
- annotations;
- MCP exposure.

Không được có hidden fallback cho command ngoài registry.

---

## 5.6 `mcp_tools.c`

Responsibilities:

```text
tools/list
tools/call argument parsing
MCP -> gw_message_t normalization
domain result -> MCP CallToolResult
```

Không chịu trách nhiệm authentication.

Không gọi BLE trực tiếp.

---

## 5.7 `mcp_policy.c`

Policy độc lập với protocol.

Responsibilities:

```text
tool exposed?
command allowed?
destructive command allowed?
voice/control profile?
device capability permits command?
```

MCP capability advertisement không phải authorization.

---

# 6. MCP transport contract

Endpoint:

```text
POST /mcp
```

Content type:

```text
Content-Type: application/json
```

Protocol:

```text
MCP-Protocol-Version: 2026-07-28
```

Response transport policy của gateway:

```text
Content-Type: application/json
```

Gateway **không chọn**:

```text
Content-Type: text/event-stream
```

cho response trong scope refactor này. Đây là quyết định implementation hợp lệ: gateway chỉ hỗ trợ JSON responses và không triển khai SSE streaming.

Mỗi request:

```text
Mcp-Method: <JSON-RPC method>
```

`Mcp-Name` bắt buộc với operations có named target.

Trong scope hiện tại:

```text
tools/call -> Mcp-Name = params.name
```

Không cần `Mcp-Name` cho:

```text
server/discover
tools/list
```

---

# 7. Header/body consistency

Gateway MUST reject mismatch.

Trước khi so sánh `Mcp-Name`, gateway MUST normalize header value. MCP cho phép header value dùng sentinel Base64 khi tên không header-safe.

Flow:

```text
Mcp-Name
   |
   +-- plain header-safe value -> use directly
   |
   +-- encoded sentinel value  -> decode Base64
                                  |
                                  v
                              normalized name
```

Sau normalize:

```text
normalized Mcp-Name == params.name
```

Nếu decode lỗi, output vượt buffer, hoặc normalized value không khớp body:

```text
HTTP 400 Bad Request
JSON-RPC -32020 HeaderMismatch
```

Không được `strlcpy()` rồi vô tình so sánh giá trị đã bị truncate.

Ví dụ body:

```json
{
  "method": "tools/call",
  "params": {
    "name": "device_command"
  }
}
```

Headers:

```text
Mcp-Method: tools/list
```

=> reject.

Hoặc:

```text
Mcp-Method: tools/call
Mcp-Name: list_devices
```

body:

```json
{
  "method": "tools/call",
  "params": {
    "name": "device_command"
  }
}
```

=> reject.

Recommended HTTP:

```text
400 Bad Request
```

---

# 8. Required `_meta` validation

Request:

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "tools/list",
  "params": {
    "_meta": {
      "io.modelcontextprotocol/protocolVersion": "2026-07-28",
      "io.modelcontextprotocol/clientCapabilities": {}
    }
  }
}
```

Validate:

```text
params exists
params is object
params._meta exists
params._meta is object

protocolVersion exists
protocolVersion is string
protocolVersion == "2026-07-28"

clientCapabilities exists
clientCapabilities is object
```

Optional:

```text
clientInfo
logLevel
unknown vendor metadata
```

Policy:

- valid optional metadata -> ignore;
- unknown non-reserved metadata -> ignore;
- malformed required metadata -> reject;
- malformed `clientInfo` nếu client gửi -> reject hoặc strict validation theo wire schema;
- không copy metadata vào domain structures.

Header version MUST match `_meta.protocolVersion`.

---

# 9. Legacy mode

## 9.1 Hiện trạng

Current component có:

```text
CONFIG_MCP_LEGACY_MODE
```

và cho request thiếu `MCP-Protocol-Version` chạy project-specific legacy wire format.

## 9.2 Target architecture

Khuyến nghị:

### Phase migration

Trong một release transition:

```text
CONFIG_MCP_LEGACY_MODE=y
```

nhưng:

- document rõ đây là project legacy;
- không gọi nó là MCP compatibility mode;
- thêm deprecation warning.

Sau khi MCP client mới đã hoạt động ổn:

```text
CONFIG_MCP_LEGACY_MODE=n
```

Sau một release ổn định nữa:

- xóa code legacy;
- xóa NVS legacy override;
- xóa aliases `list_tools` và `call_tool`.

Target final:

```text
server/discover
tools/list
tools/call
```

chỉ dùng MCP `2026-07-28`.

---

# 10. `server/discover`

Gateway phải implement `server/discover`.

Request:

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "server/discover",
  "params": {
    "_meta": {
      "io.modelcontextprotocol/protocolVersion": "2026-07-28",
      "io.modelcontextprotocol/clientCapabilities": {}
    }
  }
}
```

Response target:

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "resultType": "complete",
    "supportedVersions": [
      "2026-07-28"
    ],
    "capabilities": {
      "tools": {
        "listChanged": false
      }
    },
    "_meta": {
      "io.modelcontextprotocol/serverInfo": {
        "name": "esp32-ble-gateway",
        "version": "1.0.0"
      }
    },
    "instructions": "Controls BLE devices managed by this ESP32 gateway.",
    "ttlMs": 60000,
    "cacheScope": "private"
  }
}
```

Rules:

- không advertise resources/prompts nếu không hỗ trợ;
- không advertise capability giả;
- `supportedVersions` MUST chứa `2026-07-28`;
- `resultType` MUST là `complete`;
- `cacheScope` dùng `private`;
- `ttlMs` có thể bắt đầu ở 60 giây;
- `instructions` ngắn và static;
- `serverInfo` là informational, không dùng để authorization;
- nếu server phát `serverInfo`, nên dùng helper chung để giữ shape nhất quán.

---

# 11. `tools/list`

Response phải deterministic và đúng `CacheableResult` contract.

Minimum target response:

```json
{
  "jsonrpc": "2.0",
  "id": 2,
  "result": {
    "resultType": "complete",
    "tools": [
      {
        "name": "get_status",
        "description": "Get gateway and BLE status",
        "inputSchema": {
          "type": "object",
          "properties": {},
          "additionalProperties": false
        },
        "annotations": {
          "readOnlyHint": true,
          "destructiveHint": false,
          "idempotentHint": true
        }
      }
    ],
    "ttlMs": 60000,
    "cacheScope": "private"
  }
}
```

Required fields cho project target:

```text
resultType = complete
tools
ttlMs
cacheScope = private
```

Không phụ thuộc:

- connected clients;
- session;
- request history.

Tool list nên ổn định để MCP client cache được.

Target tool surface:

```text
list_devices
list_device_capabilities
device_command
get_status
```

Khi có `device_state`:

```text
get_device_state
```

Admin CRUD không expose mặc định qua MCP control profile.

---

# 12. Loại bỏ admin tools khỏi MCP voice/control surface

Current registry có:

```text
add_device
edit_device
delete_device
list_devices
get_status
list_device_capabilities
device_command
```

Target MCP control surface:

```text
list_devices
get_status
list_device_capabilities
device_command
```

Future:

```text
get_device_state
```

Admin operations:

```text
add_device
edit_device
delete_device
scan
restart
wifi configuration
```

đưa về Web UI / REST admin.

Nếu sau này cần MCP admin, tạo profile riêng:

```text
MCP_PROFILE_CONTROL
MCP_PROFILE_ADMIN
```

Không expose admin commands cho voice agent mặc định.

---

# 13. Tool schemas

Tất cả object schema nên:

```json
{
  "type": "object",
  "properties": {},
  "additionalProperties": false
}
```

## 13.1 `list_devices`

```json
{
  "type": "object",
  "properties": {},
  "additionalProperties": false
}
```

---

## 13.2 `get_status`

```json
{
  "type": "object",
  "properties": {},
  "additionalProperties": false
}
```

---

## 13.3 `list_device_capabilities`

```json
{
  "type": "object",
  "properties": {
    "device_id": {
      "type": "string",
      "minLength": 1
    }
  },
  "required": [
    "device_id"
  ],
  "additionalProperties": false
}
```

---

## 13.4 `device_command`

Target:

```json
{
  "type": "object",
  "properties": {
    "device_id": {
      "type": "string",
      "minLength": 1
    },
    "command": {
      "type": "string",
      "minLength": 1
    },
    "int_value": {
      "type": "integer"
    },
    "bool_value": {
      "type": "boolean"
    }
  },
  "required": [
    "device_id",
    "command"
  ],
  "additionalProperties": false
}
```

Không cố encode toàn bộ dynamic capability vào schema của `device_command`.

Device-specific validation phải dùng `device_capabilities`.

---

# 14. Không tạo dynamic MCP tools theo device

Không làm:

```text
living_room_light_turn_on
living_room_light_set_brightness
fan_01_set_speed
...
```

Lý do:

- tools/list tăng theo device count;
- tốn heap JSON;
- tool catalog thay đổi liên tục;
- giảm cacheability;
- tăng token usage của AI;
- duplicate domain semantics;
- khó policy;
- khó test.

Giữ flow:

```text
list_devices
       |
       v
list_device_capabilities
       |
       v
device_command
```

---

# 15. Device capability không phải authorization

Current device protocol có capability advertisement.

Rule bắt buộc:

```text
device advertises command
AND
gateway policy permits command
AND
MCP control profile permits risk class
```

mới được execute.

Không được:

```text
device advertises command
=> automatically allowed over MCP
```

---

# 16. MCP command policy

Đề xuất function:

```c
typedef enum {
    MCP_POLICY_ALLOW = 0,
    MCP_POLICY_DENY_COMMAND,
    MCP_POLICY_DENY_DESTRUCTIVE,
    MCP_POLICY_DEVICE_UNAVAILABLE,
    MCP_POLICY_CAPABILITY_UNKNOWN,
} mcp_policy_result_t;

mcp_policy_result_t mcp_policy_check_device_command(
    const char *device_id,
    const char *command);
```

Policy sequence:

```text
device exists?
   |
device connected / usable?
   |
capabilities ready?
   |
command advertised?
   |
global allow policy?
   |
destructive?
   |
ALLOW
```

---

# 17. Destructive command policy

`device_capabilities` đã có:

```text
idempotent
destructive
```

MCP control/voice profile:

```text
destructive == true
=> DENY
```

Không cần MRTR confirmation trong phase này.

Nếu cần destructive action:

- Web UI admin;
- hoặc future ADMIN MCP profile.

---

# 18. Command allowlist

Current:

```text
CONFIG_MCP_DEVICE_COMMAND_ALLOWLIST
```

default empty.

Có thể giữ trong Phase 1.

Semantic:

```text
empty => deny device commands
```

Sau đó có thể nâng cấp policy nhưng không cần ngay.

Recommended evaluation:

```text
if not capability_advertised:
    deny

if not config_allowlisted:
    deny

if capability.destructive:
    deny for control profile

allow
```

---

# 19. `tools/call` parsing

Expected request:

```json
{
  "jsonrpc": "2.0",
  "id": 5,
  "method": "tools/call",
  "params": {
    "name": "device_command",
    "arguments": {
      "device_id": "fan_01",
      "command": "set_speed",
      "int_value": 60
    },
    "_meta": {
      "io.modelcontextprotocol/protocolVersion": "2026-07-28",
      "io.modelcontextprotocol/clientCapabilities": {}
    }
  }
}
```

Headers:

```text
MCP-Protocol-Version: 2026-07-28
Mcp-Method: tools/call
Mcp-Name: device_command
```

Validation order:

```text
1. HTTP gate
2. protocol header
3. body length
4. JSON parse
5. JSON-RPC envelope
6. required _meta
7. version match
8. Mcp-Method match
9. normalize/decode Mcp-Name khi có
10. Mcp-Name match params.name
11. registry lookup
12. tool arguments
13. MCP policy
14. execution
```

---

# 20. Không fallback async -> synchronous

Đây là P0.

Current implementation có flow:

```text
async_begin fail
or context allocation fail
or executor unavailable
       |
       v
fallback synchronous execution
```

Điều này phải loại bỏ.

Lý do:

- HTTPD task có thể block chờ BLE ACK;
- latency BLE có thể tới timeout seconds;
- request concurrency giảm mạnh;
- resource-pressure path trở thành path nguy hiểm nhất;
- behavior khác giữa normal load và memory pressure.

Target:

```text
async_begin fail
=> error

context malloc fail
=> error

command_executor_submit queue full
=> 503

executor unavailable
=> 503
```

Không chạy BLE command synchronously từ HTTP handler.

---

# 21. Async ownership

Đề xuất context:

```c
typedef struct {
    httpd_req_t *request;

    cJSON *request_id;

    bool notification;
} mcp_async_context_t;
```

Không cần copy toàn bộ MCP `_meta`.

Nếu response cần serverInfo thì builder tự thêm static server info.

Ownership:

```text
endpoint
  |
  +-- cJSON request root
  |
  +-- duplicate id only when async submit needed
  |
  +-- async context owns duplicate id
  |
  +-- executor callback owns async context
```

Sau successful submit:

```text
HTTP handler MUST NOT:
- access async request
- free async context
- free copied id
```

Callback:

```text
format result
send response
free id
async_complete
free context
```

---

# 22. Queue-full behavior

Current `command_executor` đã có bounded queue.

MCP phải map:

```text
ESP_ERR_NO_MEM / queue full
```

thành:

```text
HTTP 503 Service Unavailable
```

JSON-RPC message ví dụ:

```json
{
  "jsonrpc": "2.0",
  "id": 10,
  "error": {
    "code": -32000,
    "message": "Gateway busy"
  }
}
```

Không retry bên trong ESP32 MCP endpoint.

Client/AI host quyết định retry.

---

# 23. JSON-RPC error contract

Sử dụng chuẩn JSON-RPC cho protocol errors:

```text
-32700 Parse error
-32600 Invalid Request
-32601 Method not found
-32602 Invalid params
-32603 Internal error
```

MCP protocol-defined error codes cần giữ đúng nghĩa:

```text
-32020 HeaderMismatch
-32021 MissingRequiredClientCapability
-32022 UnsupportedProtocolVersion
```

**Không được dùng `-32021` cho authentication/authorization.**

Transport/security failures dùng HTTP semantics:

```text
missing/invalid bearer token -> HTTP 401
forbidden Host/Origin       -> HTTP 403
rate limited                -> HTTP 429
unsupported content type    -> HTTP 415
body too large              -> HTTP 413
```

Unknown MCP method:

```text
HTTP 404 Not Found
JSON-RPC -32601 Method not found
```

`UnsupportedProtocolVersion` nên trả `data` hữu ích:

```json
{
  "jsonrpc": "2.0",
  "id": null,
  "error": {
    "code": -32022,
    "message": "Unsupported protocol version",
    "data": {
      "supported": [
        "2026-07-28"
      ],
      "requested": "2025-11-25"
    }
  }
}
```

Nếu project cần gateway-specific execution codes, dùng khoảng:

```text
-32000 .. -32019
```

và không đè lên protocol-defined:

```text
-32020
-32021
-32022
```

Suggested project-local codes:

```text
-32000 Gateway busy
-32001 Device unavailable
-32002 Command denied
-32003 Capability unavailable
```

Không tạo quá nhiều custom codes.

---

# 24. Tool execution error vs JSON-RPC error

Phải phân biệt rõ hai lớp lỗi.

## 24.1 Protocol / request-shape error

Ví dụ:

```text
invalid JSON
root không phải object
missing jsonrpc
missing method
unknown MCP method
params không phải object
tools/call thiếu name
arguments không phải object
```

=> JSON-RPC `error`.

Examples:

```text
malformed request     -> -32600
unknown method        -> HTTP 404 + -32601
invalid call envelope -> -32602
```

## 24.2 Tool invocation / input-validation error

Khi request `tools/call` hợp lệ và tool đã được resolve nhưng arguments không phù hợp với semantics của tool, ưu tiên trả `CallToolResult` với:

```json
{
  "resultType": "complete",
  "content": [
    {
      "type": "text",
      "text": "set_speed requires an integer between 0 and 100"
    }
  ],
  "isError": true
}
```

Ví dụ:

```text
missing required device_id trong resolved tool input
unknown argument
wrong typed value
value outside advertised capability range
device offline
BLE ACK timeout
command rejected
policy-denied safe tool invocation
```

=> `CallToolResult.isError = true`.

Điều này cho phép AI client/model đọc lỗi và tự sửa tool invocation.

Rule:

> JSON-RPC error = request/tool invocation không thể được resolve hợp lệ ở protocol layer.  
> Tool error = tool đã được resolve nhưng input/business execution thất bại.

Unknown tool name trong `tools/call` nên được coi là invalid call/tool resolution error và trả JSON-RPC `-32602`.

---

# 25. `CallToolResult`

Success example:

```json
{
  "jsonrpc": "2.0",
  "id": 10,
  "result": {
    "resultType": "complete",
    "content": [
      {
        "type": "text",
        "text": "Command executed successfully"
      }
    ],
    "structuredContent": {
      "success": true,
      "device_id": "fan_01",
      "command": "set_speed"
    },
    "isError": false
  }
}
```

Failure example:

```json
{
  "jsonrpc": "2.0",
  "id": 10,
  "result": {
    "resultType": "complete",
    "content": [
      {
        "type": "text",
        "text": "Device is offline"
      }
    ],
    "structuredContent": {
      "success": false,
      "code": "device_offline"
    },
    "isError": true
  }
}
```

`structuredContent` nên nhỏ và deterministic.

---

# 26. Tool annotations

Annotations chỉ là hint cho client.

Không dùng annotations để enforce policy.

Example:

```json
{
  "readOnlyHint": true,
  "destructiveHint": false,
  "idempotentHint": true
}
```

Suggested:

```text
list_devices
readOnly=true
destructive=false
idempotent=true

get_status
readOnly=true
destructive=false
idempotent=true

list_device_capabilities
readOnly=true
destructive=false
idempotent=true

device_command
readOnly=false
destructive=false
idempotent=false
```

Giá trị `destructiveHint=false` chỉ đúng cho **CONTROL profile** khi `mcp_policy` bảo đảm destructive capabilities luôn bị chặn.

Nếu tương lai dùng chung tool definition cho ADMIN profile có thể chạy destructive command, nên:

```text
omit destructiveHint
omit idempotentHint
```

thay vì phát hint sai.

`device_command` generic không thể tự mô tả chính xác destructive/idempotent semantics của mọi dynamic command.

Do not infer authorization from these annotations.

---

# 27. Schema bugs cần sửa

## 27.1 `edit_device`

Nếu tool vẫn tồn tại trong legacy/admin profile:

Current schema cần đảm bảo:

```text
device_id required
AND
(name OR device_type)
```

Không chỉ require `name` hoặc `device_type`.

## 27.2 BLE address type

Không hardcode:

```text
maximum = 1
```

nếu `device_store` chấp nhận `0..3`.

Tạo single source of truth hoặc bỏ schema này khỏi control profile.

## 27.3 `additionalProperties`

Bổ sung:

```json
"additionalProperties": false
```

cho schemas để tránh typo im lặng.

---

# 28. Authentication

Current Bearer token approach có thể giữ.

Request:

```text
Authorization: Bearer <token>
```

Rule:

```text
production:
token MUST be non-empty

development:
empty token allowed only explicitly
```

Không log token.

Constant-time comparison giữ lại.

---

# 29. LAN security model

Endpoint hiện dùng plaintext HTTP.

Threat model:

```text
trusted LAN only
```

Không expose `/mcp` trực tiếp ra Internet.

Cloud AI voice architecture:

```text
Cloud AI
   |
 HTTPS/Auth
   |
Local MCP Bridge
   |
 LAN
   |
ESP32 Gateway
```

Bridge có thể chạy trên:

```text
Raspberry Pi
NAS
Mac mini
Linux home server
```

ESP32 không cần xử lý OAuth/cloud auth.

---

# 30. Host / Origin protection

Giữ:

```text
Host allowlist
Origin validation
```

để giảm DNS-rebinding risk.

Không xem `Host` là authentication.

Recommended default:

```text
gateway.local
192.168.4.1
configured LAN hostname/IP
```

Production provisioning nên update allowlist phù hợp deployment.

---

# 31. Rate limiting

Giữ token bucket.

Suggested defaults:

```text
10 requests/second
burst 10
```

Sau integration test AI voice có thể tune.

Rate limiting nên chạy trước body allocation nếu possible.

---

# 32. Request size

Current maximum:

```text
4096 bytes
```

Có thể giữ.

Không tăng lên PSRAM chỉ để MCP request lớn hơn.

Tool interface phải được giữ nhỏ.

Reject:

```text
HTTP 413
```

nếu vượt limit.

---

# 33. Memory policy

MCP endpoint nên ưu tiên internal SRAM cho:

- small request context;
- executor callback context;
- HTTP server structures.

JSON body:

```text
<= 4096 bytes
```

lifetime ngắn.

Không cache full JSON `tools/list` trừ khi profiling chứng minh có lợi.

Tool registry compile-time/static tốt hơn dynamic allocation lâu dài.

Không giữ:

```text
client sessions
client metadata history
request traces
tool invocation history
logs
```

trong MCP component.

---

# 34. `mcp_request_meta_t` refactor

Current:

```c
typedef struct {
    bool mcp_2026;
    char mcp_method[24];
    char mcp_name[64];
} mcp_request_meta_t;
```

Target:

```c
typedef struct {
    char method[32];
    char name[64];

    bool has_name;
} mcp_route_info_t;
```

Protocol version không cần copy nếu gateway chỉ support một version.

Hoặc:

```c
typedef struct {
    mcp_route_info_t route;
    bool notification;
} mcp_request_context_t;
```

Không gọi routing headers là `_meta`.

---

# 35. Response `_meta`

Không dùng custom metadata.

Project policy:

```text
Every successful MCP result SHOULD contain:
result._meta["io.modelcontextprotocol/serverInfo"]
```

Shape:

```json
"_meta": {
  "io.modelcontextprotocol/serverInfo": {
    "name": "esp32-ble-gateway",
    "version": "1.0.0"
  }
}
```

Implement bằng một helper chung, ví dụ:

```c
bool mcp_result_add_server_info(cJSON *result);
```

Không rải logic server identity vào từng tool.

`serverInfo` là informational.

Không thêm:

```text
gateway memory
BLE state
trace ID
device name
request timestamp
firmware internals
```

vào `_meta`.

Những dữ liệu application-level phải nằm trong tool result.

Nếu sau profiling project quyết định bỏ `serverInfo` khỏi một số result để tiết kiệm payload, phải document rõ đây là quyết định không thực hiện một `SHOULD`, không được mô tả là requirement của spec.

---

# 36. `tools/list` cache hints

Với `2026-07-28`, list result cần cache hints.

Use:

```json
{
  "ttlMs": 60000,
  "cacheScope": "private"
}
```

Nếu tool surface hoàn toàn compile-time static, có thể tăng TTL sau.

Không dùng `"mcp-endpoint"` làm cacheScope.

Valid scope cho use case này:

```text
private
```

---

# 37. `tools/list` deterministic order

Tool order phải cố định.

Example:

```text
get_status
list_devices
list_device_capabilities
device_command
```

Không order theo:

- registration timing;
- connected device;
- hash map order;
- request history.

Deterministic order cải thiện MCP/client caching và prompt stability.

---

# 38. MCP + Web architecture

Web REST và MCP phải dùng cùng execution layer.

Không:

```text
Web -> command_executor
MCP -> BLE directly
```

Target:

```text
Web REST -----+
              |
              v
      command_executor
              |
MCP ----------+
              |
              v
     command_dispatcher
              |
              v
           BLE
```

Mọi mutation qua shared command path.

---

# 39. AI voice responsibility

ESP32 gateway không làm:

```text
STT
LLM
TTS
semantic natural-language resolution
conversation state
```

AI host chịu trách nhiệm:

```text
"bật quạt phòng khách lên 60%"
       |
       v
resolve device
       |
       v
resolve command
       |
       v
MCP tools/call
```

Gateway chỉ nhận:

```text
device_id
command
typed value
```

---

# 40. Device semantic model

Không bắt buộc trong MCP refactor P0 nhưng nên chuẩn bị.

Future device metadata:

```text
device_id
name
device_type
area
aliases[]
tags[]
```

Example:

```text
device_id: fan_01
name: Quạt phòng khách
device_type: fan
area: living_room
aliases:
  - quạt lớn
  - quạt phòng khách
```

AI host dùng metadata để resolve natural language.

Gateway vẫn execute bằng exact `device_id`.

---

# 41. `device_state` future component

Recommended future:

```text
components/device_state/
    device_state.c
    include/device_state.h
```

Store last-known state RAM only.

Example:

```c
typedef struct {
    char device_id[...];
    char state_key[...];
    int64_t updated_at_us;
    bool valid;
    ...
} device_state_entry_t;
```

Future MCP tool:

```text
get_device_state
```

Không cần persistent log.

---

# 42. Current implementation issues cần xử lý

## P0-1 — Missing strict `Mcp-Method` enforcement

Current codec đọc header nhưng chưa bắt buộc header tồn tại và khớp body.

Target:

```text
all MCP 2026 HTTP requests require Mcp-Method
```

---

## P0-2 — Missing strict `Mcp-Name` enforcement

For:

```text
tools/call
```

require:

```text
decoded/normalized Mcp-Name == params.name
```

Gateway phải hỗ trợ Base64 sentinel decoding trước khi compare.

---

## P0-3 — `_meta` chưa được validate đầy đủ

Current implementation chủ yếu chọn wire mode bằng HTTP header.

Target phải validate:

```text
params._meta.protocolVersion
params._meta.clientCapabilities
```

và version/header match.

---

## P0-4 — `server/discover` shape chưa đúng target

Current output:

```text
name
version
protocolVersion
capabilities
```

Target:

```text
resultType
supportedVersions
capabilities
_meta.serverInfo
instructions
ttlMs
cacheScope
```

---

## P0-5 — server metadata placement

Không stamp protocol/server identity vào JSON-RPC top-level custom `_meta`.

Server identity nếu phát ra phải nằm trong MCP result metadata.

---

## P0-6 — synchronous BLE fallback

Remove hoàn toàn synchronous fallback trong `tools/call`.

---

## P0-7 — error-code conflict

Không dùng:

```text
-32021 Authorization denied
```

Vì `-32021` là:

```text
MissingRequiredClientCapability
```

Auth dùng HTTP 401/403.

---

## P0-8 — unknown method HTTP status

Unknown method phải trả:

```text
HTTP 404
JSON-RPC -32601
```

Không trả HTTP 200.

---

## P0-9 — `tools/list` incomplete cacheable contract

Bắt buộc verify:

```text
resultType
tools
ttlMs
cacheScope
```

---

## P0-10 — wrong error classification

Không map tất cả tool argument validation thành JSON-RPC errors.

Phải phân biệt:

```text
protocol/request-shape error -> JSON-RPC error
resolved tool input/business error -> CallToolResult.isError=true
```

---

## P0-11 — conformance tests đang self-validating old contract

Unit tests phải đổi expectation theo official MCP wire spec.

---

# 43. Implementation sequence

## Phase 0 — Freeze behavior

Before refactor:

- chạy current tests;
- lưu baseline;
- ghi nhận heap;
- ghi nhận stack high-water mark;
- ghi nhận request latency;
- ghi nhận command executor saturation behavior.

Không thay đổi BLE protocol.

---

## Phase 1 — Protocol validator

Implement:

```text
MCP-Protocol-Version required
required params._meta
protocolVersion match
clientCapabilities required
Mcp-Method required
Mcp-Method body match
Mcp-Name tools/call required
Mcp-Name params.name match
```

Không thay tool registry trong phase này.

---

## Phase 1B — Transport behavior

Implement:

```text
JSON-only response mode
unknown method -> HTTP 404
header decoding helper
notification handling policy
```

Minimal server không cần client->server notifications.

Nếu future extension notification được chấp nhận:

```text
HTTP 202 Accepted
empty body
```

Không dùng `204 No Content` cho accepted MCP notification.

---

## Phase 2 — Discovery correction

Refactor:

```text
mcp_codec_build_discovery()
```

hoặc chuyển sang:

```text
mcp_protocol_build_discovery()
```

Output đúng target contract.

---

## Phase 3 — RPC metadata correction

Remove automatic top-level:

```text
_meta.protocolVersion
_meta.server
```

Nếu serverInfo được phát:

```text
result._meta["io.modelcontextprotocol/serverInfo"]
```

---

## Phase 4 — Async execution hardening

Remove:

```text
async fail -> synchronous execute
```

Replace with fail-fast.

Tests:

```text
async_begin failure
context OOM
queue full
executor unavailable
client disconnect
BLE timeout
```

---

## Phase 5 — Registry cleanup

Default MCP tools:

```text
get_status
list_devices
list_device_capabilities
device_command
```

Move admin tools khỏi control profile.

Add:

```text
additionalProperties:false
```

---

## Phase 6 — MCP policy

Implement:

```text
capability
AND allowlist
AND destructive guard
```

Không allow based on tool annotations.

---

## Phase 7 — Legacy deprecation

First release:

```text
legacy available
default optional depending migration need
```

Target next release:

```text
legacy default n
```

Then remove.

---

## Phase 8 — Official SDK interoperability

Black-box test với ít nhất:

```text
TypeScript MCP SDK 2026-07-28
```

Optional:

```text
Python MCP SDK
Go MCP SDK
```

---

# 44. Unit test matrix

## Protocol version

```text
PASS correct header + correct _meta version
FAIL missing header
FAIL unsupported header version
FAIL missing _meta
FAIL missing _meta protocolVersion
FAIL header/body version mismatch
FAIL missing clientCapabilities
FAIL clientCapabilities wrong type
```

## Routing headers

```text
PASS tools/list + Mcp-Method tools/list
PASS tools/call + Mcp-Method tools/call
PASS tools/call + correct Mcp-Name

FAIL missing Mcp-Method
FAIL Mcp-Method mismatch
FAIL tools/call missing Mcp-Name
FAIL tools/call Mcp-Name mismatch
PASS Base64-encoded Mcp-Name decodes and matches
FAIL malformed Base64 Mcp-Name
FAIL decoded Mcp-Name exceeds buffer
```

## JSON-RPC

```text
FAIL invalid JSON
FAIL non-object root
FAIL jsonrpc != 2.0
FAIL missing method
FAIL invalid id type
FAIL unknown method -> HTTP 404 + JSON-RPC -32601
```

## Discovery

```text
PASS supportedVersions contains 2026-07-28
PASS capabilities.tools exists
PASS no unsupported capability advertised
PASS resultType complete
PASS ttlMs
PASS cacheScope private
PASS serverInfo shape when emitted
```

## tools/list

```text
PASS deterministic order
PASS valid inputSchema
PASS no hidden tools
PASS additionalProperties false
PASS cache hints
```

## tools/call

```text
PASS known tool
FAIL unknown tool
FAIL invalid arguments
FAIL extra arguments
FAIL missing device_id
FAIL missing command
PASS resolved tool invalid business input -> CallToolResult.isError=true
PASS resolved tool value outside capability range -> CallToolResult.isError=true
```

## Policy

```text
FAIL device command not allowlisted
FAIL capability not advertised
FAIL destructive command
PASS safe advertised allowlisted command
```

## Async

```text
PASS executor accepted
FAIL queue full -> 503
FAIL context allocation -> no sync execution
FAIL async_begin -> no sync execution
FAIL executor unavailable -> no sync execution
```

## Transport semantics

```text
PASS successful response uses application/json
PASS unknown method uses HTTP 404
PASS accepted future notification uses HTTP 202 + empty body
FAIL accepted notification returns HTTP 204
```

## Security

```text
FAIL wrong token
FAIL missing token production
FAIL disallowed Host
FAIL disallowed Origin
FAIL invalid content type
FAIL body > 4096
FAIL rate limit
```

---

# 45. Black-box interoperability test

Không chỉ test C component tự viết.

Test actual running gateway:

```text
Official MCP client
        |
        v
POST /mcp
        |
        v
ESP32
```

Minimum scenario:

```text
1. server/discover
2. tools/list
3. tools/call get_status
4. tools/call list_devices
5. tools/call list_device_capabilities
6. tools/call device_command
```

Success criteria:

- SDK không cần custom transport patch;
- không cần custom JSON-RPC wrapper;
- không cần legacy alias;
- không cần bypass metadata validation.

---

# 46. Stress test

Run:

```text
repeated tools/list
repeated get_status
parallel device_command
invalid request burst
rate-limit burst
queue saturation
```

Observe:

```text
heap leak
minimum free heap
largest free block
HTTPD stack high-water mark
command_executor stack
BLE task responsiveness
watchdog
socket leaks
```

No response path được giữ pointer tới freed request body/cJSON root.

---

# 47. Kconfig target

Suggested:

```text
MCP_AUTH_TOKEN
MCP_HOST_ALLOWLIST
MCP_DEVICE_COMMAND_ALLOWLIST
MCP_RATE_LIMIT_RPS
MCP_LEGACY_MODE
```

Future:

```text
MCP_ENABLE_ADMIN_TOOLS
```

default:

```text
n
```

Không cần Kconfig cho:

```text
resources
prompts
sampling
tasks
MRTR
SSE
```

---

# 48. CMake dependencies

Component dependencies chỉ giữ những gì cần:

```text
esp_http_server
command_dispatcher
command_executor
espressif__cjson
nvs_flash
esp_timer
freertos
```

Nếu `cbor_codec` không trực tiếp được sử dụng trong MCP endpoint sau refactor thì remove dependency.

MCP không nên biết BLE CBOR wire format.

---

# 49. Public API

Giữ public surface nhỏ:

```c
int mcp_endpoint_register(httpd_handle_t server);
```

Không expose internal protocol types ra application.

Application startup:

```c
httpd_handle_t server = web_server_start();

mcp_endpoint_register(server);
```

Web và MCP dùng cùng HTTP server.

---

# 50. Internal naming

Recommended:

```text
mcp_endpoint      = HTTP route orchestration
mcp_protocol      = MCP wire contract
mcp_rpc           = JSON-RPC response helpers
mcp_registry      = exposed tools
mcp_tools         = tool argument/result mapping
mcp_policy        = authorization for operations
mcp_auth          = transport authentication
mcp_transport     = ESP HTTP wrapper/test seam
```

Tránh tên ambiguous như:

```text
codec
meta
handler_utils
common
misc
```

nếu responsibility cụ thể hơn.

---

# 51. Definition of Done

Refactor chỉ được coi là hoàn thành khi:

## Protocol

- [ ] chỉ target MCP `2026-07-28`;
- [ ] `MCP-Protocol-Version` required;
- [ ] request `_meta.protocolVersion` required;
- [ ] request `_meta.clientCapabilities` required;
- [ ] header/meta version match;
- [ ] `Mcp-Method` required và match;
- [ ] `Mcp-Name` required cho `tools/call` và match;
- [ ] `server/discover` đúng shape;
- [ ] `tools/list.resultType == "complete"`;
- [ ] `tools/list.ttlMs` tồn tại;
- [ ] `tools/list.cacheScope == "private"`;
- [ ] unknown method trả HTTP 404 + JSON-RPC `-32601`;
- [ ] `Mcp-Name` Base64 sentinel được decode trước khi compare;
- [ ] không dùng `-32021` cho auth;
- [ ] `-32022` response có `data.supported` và `data.requested` khi applicable.

## Architecture

- [ ] MCP không gọi BLE trực tiếp;
- [ ] mutation dùng `command_executor`;
- [ ] không synchronous fallback;
- [ ] no session state;
- [ ] no dynamic tool generation;
- [ ] `_meta` không đi vào domain layer.

## Security

- [ ] Bearer auth;
- [ ] constant-time compare;
- [ ] Host allowlist;
- [ ] Origin validation;
- [ ] request size limit;
- [ ] rate limit;
- [ ] destructive device commands denied mặc định.

## Quality

- [ ] unit tests pass;
- [ ] stress tests pass;
- [ ] official MCP SDK client connect được;
- [ ] no heap leak;
- [ ] no HTTPD blocking BLE ACK;
- [ ] no hidden MCP command surface.

---

# 52. Recommended PR breakdown

## PR-1 — MCP protocol validator

Files:

```text
mcp_protocol.c
mcp_endpoint_internal.h
test_mcp_protocol.c
```

Implement strict headers + `_meta`.

Bao gồm:

```text
Mcp-Method agreement
Mcp-Name agreement
Base64 sentinel decode
protocol-defined -32020/-32021/-32022 semantics
```

---

## PR-2 — Discovery + response contract

Fix:

```text
server/discover
response metadata
cache hints
```

---

## PR-3 — Async fail-fast

Remove synchronous fallback.

Add resource-pressure tests.

---

## PR-4 — Registry/schema cleanup

Reduce exposed tools.

Add strict JSON schemas.

---

## PR-5 — MCP policy

Capability + allowlist + destructive guard.

---

## PR-6 — Legacy deprecation

Disable legacy default after compatibility verification.

---

## PR-7 — Official SDK interoperability

Add host-side integration test/scripts.

---

# 53. AI coding-agent instructions

Khi giao spec này cho AI coding agent:

1. Không rewrite toàn bộ component trong một patch.
2. Mỗi PR chỉ giải quyết một concern.
3. Không thay BLE protocol v3.
4. Không thay `command_dispatcher` public contract nếu không bắt buộc.
5. Không tạo dynamic tool per-device.
6. Không thêm resources/prompts/tasks.
7. Không implement MRTR.
8. Không expose admin tools mặc định.
9. Không fallback synchronous BLE execution.
10. Không dùng `_meta` cho business logic.
11. Mọi new allocation phải có ownership rõ.
12. Mọi error path phải free cJSON/buffer/context đúng owner.
13. Test phải fail trước khi fix và pass sau fix.
14. Interoperability phải được kiểm tra bằng official MCP SDK.
15. Không coi internal test suite là bằng chứng duy nhất về standards conformance.

---

# 54. Expected final behavior

AI hỏi:

```text
"Bật quạt phòng khách lên 60%"
```

AI host:

```text
list_devices
```

Gateway trả:

```json
{
  "devices": [
    {
      "device_id": "fan_01",
      "name": "Quạt phòng khách",
      "device_type": "fan",
      "connected": true
    }
  ]
}
```

AI:

```text
list_device_capabilities(fan_01)
```

Gateway:

```json
{
  "device_id": "fan_01",
  "commands": [
    {
      "name": "set_speed",
      "value_type": "int",
      "min": 0,
      "max": 100,
      "unit": "%"
    }
  ]
}
```

AI:

```text
device_command(
  device_id = fan_01,
  command = set_speed,
  int_value = 60
)
```

Gateway execution:

```text
MCP
 |
 v
mcp_policy
 |
 v
command_executor
 |
 v
command_dispatcher
 |
 v
BLE Central
 |
 v
fan_01
 |
 v
ACK
 |
 v
CallToolResult
```

ESP32 không cần hiểu câu tự nhiên.

---

# 55. Final target

Sau refactor, `mcp_endpoint` phải có tính chất:

```text
small
stateless
deterministic
standards-compliant
bounded-memory
safe under load
easy to test
easy to integrate with AI clients
```

MCP là external control protocol.

`command_dispatcher` vẫn là domain routing core.

BLE Protocol v3 vẫn là device wire protocol.

Ba layer độc lập:

```text
MCP / REST
    |
    v
Gateway Domain
    |
    v
BLE Protocol
```

Không để MCP-specific concepts leak xuống BLE/device implementation.

---

# 56. Tóm tắt quyết định kiến trúc

| Vấn đề | Quyết định |
|---|---|
| MCP version | `2026-07-28` |
| Full MCP | Không |
| MCP Tools | Có |
| `_meta` | Chỉ required protocol metadata |
| `clientInfo` | Optional, không dùng cho policy |
| `serverInfo` | SHOULD, informational |
| Sessions | Không |
| initialize | Không |
| SSE | Không |
| Resources | Không |
| Prompts | Không |
| Tasks | Không |
| MRTR | Không |
| Dynamic device tools | Không |
| Admin MCP tools | Không mặc định |
| Device command | Generic `device_command` |
| Capability validation | Có |
| Destructive voice commands | Deny |
| BLE execution | `command_executor` |
| Sync fallback | Cấm |
| Request body | <= 4096 bytes |
| Auth | Bearer token |
| Internet exposure | Không |
| SDK interoperability test | Bắt buộc |

---

# 57. Reference implementation state reviewed

Các file hiện tại đã được review khi viết spec:

```text
components/mcp_endpoint/mcp_endpoint.c
components/mcp_endpoint/mcp_endpoint_internal.h
components/mcp_endpoint/mcp_codec.c
components/mcp_endpoint/mcp_rpc.c
components/mcp_endpoint/mcp_registry.c
components/mcp_endpoint/mcp_auth.c
components/mcp_endpoint/Kconfig.projbuild
components/mcp_endpoint/test/test_mcp_conformance.c
```

Các vấn đề chính được xác nhận trong implementation hiện tại:

```text
- MCP 2026 header parser exists.
- Mcp-Method/Mcp-Name are read but not strictly enforced.
- request required _meta is not fully validated.
- server/discover uses an outdated/non-target shape.
- response metadata is stamped in a custom top-level shape.
- device command has async -> synchronous fallback paths.
- current conformance tests validate the current project contract,
  not the final strict MCP 2026-07-28 contract.
```

---

# 58. Official MCP references

Target standard:

- Model Context Protocol 2026-07-28 specification:
  https://modelcontextprotocol.io/specification/2026-07-28

- Official release notes:
  https://blog.modelcontextprotocol.io/posts/2026-07-28/

- Official schema:
  https://github.com/modelcontextprotocol/modelcontextprotocol/blob/main/schema/2026-07-28/schema.ts

- Streamable HTTP transport:
  https://github.com/modelcontextprotocol/modelcontextprotocol/blob/main/docs/specification/2026-07-28/basic/transports/streamable-http.mdx

- Tools:
  https://github.com/modelcontextprotocol/modelcontextprotocol/blob/main/docs/specification/2026-07-28/server/tools.mdx

- Discovery:
  https://github.com/modelcontextprotocol/modelcontextprotocol/blob/main/docs/specification/2026-07-28/server/discover.mdx

---

# 59. Final implementation directive

Dev nên ưu tiên theo thứ tự:

```text
P0
|
+-- required _meta.protocolVersion
+-- required _meta.clientCapabilities
+-- MCP-Protocol-Version agreement
+-- Mcp-Method agreement
+-- Mcp-Name normalize/Base64 decode + agreement
+-- -32020/-32021/-32022 correct semantics
+-- UnsupportedProtocolVersionError.data
+-- unknown method => HTTP 404 + -32601
+-- correct server/discover
+-- complete tools/list CacheableResult
+-- distinguish JSON-RPC error vs CallToolResult.isError
+-- remove synchronous fallback
+-- strict schemas
+-- JSON-only transport policy
+-- official SDK test
|
P1
|
+-- MCP control policy
+-- remove admin tools
+-- legacy deprecation
|
P2
|
+-- device_state
+-- semantic device metadata
+-- voice host integration
```

Không mở rộng MCP surface trước khi P0 hoàn tất.

---

# 60. V3.1 review corrections integrated

V3.1 tích hợp các correction sau so với V3.0:

```text
1. Reserve -32021 for MissingRequiredClientCapability.
2. Use HTTP 401/403 for auth/security failures.
3. Unknown MCP method => HTTP 404 + JSON-RPC -32601.
4. Normalize/decode Base64-sentinel Mcp-Name before body comparison.
5. Make tools/list contract explicit:
   resultType + tools + ttlMs + cacheScope.
6. Distinguish protocol errors from resolved-tool validation/execution errors.
7. Use CallToolResult.isError=true for tool-level input/business failures.
8. Document UnsupportedProtocolVersionError.data.
9. Clarify successful transport uses application/json only.
10. Future accepted notifications use HTTP 202 with empty body, never 204.
11. Clarify serverInfo policy and helper ownership.
12. Tighten Definition of Done and test matrix accordingly.
```

This version supersedes V3.0 and should be used as the source-of-truth for the MCP refactor.
