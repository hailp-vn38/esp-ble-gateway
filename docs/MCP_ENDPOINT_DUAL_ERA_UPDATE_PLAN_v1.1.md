# MCP Endpoint Dual-Era Update Plan

**Project:** ESP32 BLE Gateway  
**Repository:** `hailp-vn38/esp-ble-gateway`  
**Component:** `components/mcp_endpoint`  
**Target:** ESP32-S3 / ESP-IDF v6.1-rc1  
**Document version:** 1.1  
**Date:** 2026-08-27  
**Status:** Implementation-ready specification — reviewed and corrected

---

## 1. Mục tiêu tài liệu

Tài liệu này định nghĩa kế hoạch refactor `components/mcp_endpoint` theo **Phương án B**:

1. **MCP 2026-07-28 là protocol chính**, hoạt động stateless theo per-request metadata.
2. **MCP 2025-11-25 được hỗ trợ như compatibility era**, có `initialize` và `notifications/initialized` để tương thích với client/SDK vẫn dùng handshake 2025.
3. **Không tạo `MCP-Session-Id`**, không duy trì session table, không lưu client state giữa các HTTP request.
4. Giữ gateway đúng vai trò **MCP adapter / command executor**; không mở rộng thành MCP application server đầy đủ.
5. Sửa các lỗi conformance và lỗi HTTP/wire-format đang tồn tại trong implementation hiện tại.
6. Giữ `tools/list` và `tools/call` dùng chung tool registry, policy và `command_dispatcher` hiện có.

Tài liệu này là specification cho dev/AI agent thực hiện refactor. Không yêu cầu triển khai resources, prompts, sampling, elicitation, SSE subscription, Tasks hay protocol-level session.

---

## Revision 1.1 — review corrections

Version 1.1 preserves **Phương án B** but corrects the implementation contract after a second protocol/conformance review. The important changes are:

- MCP 2025 `initialize` uses **version negotiation / counter-offer**, not immediate rejection when the client proposes another version.
- Protocol-era detection distinguishes a **malformed modern request** from a truly unknown/legacy request so a modern request cannot silently fall back to the 2025 path.
- `server/discover.supportedVersions` reflects both supported eras when `CONFIG_MCP_COMPAT_2025=y`.
- MCP 2026 `-32022 UnsupportedProtocolVersion` includes required structured `error.data` (`supported`, `requested`).
- MCP 2025 `structuredContent` is emitted only when the dispatcher JSON is an **object**; JSON arrays/scalars remain available through `TextContent` fallback.
- `notifications/initialized` accepts a missing version header for interoperability; if the header is present it must match `2025-11-25`.
- `Accept` checking is treated as a transport policy, not a mandatory server-side conformance gate; strict mode defaults off.
- MCP 2026 notification handling is not generalized to arbitrary id-less methods. Only known/supported notifications may return `202 Accepted`.
- `Mcp-Name` raw and decoded storage is separated so a Base64-encoded 128-byte decoded name cannot be truncated.
- GET/DELETE `405` responses advertise `Allow: POST` only.
- RPC error-data ownership and cleanup rules are explicit.

These corrections are normative for implementation; where v1.0 conflicts with this document, **v1.1 wins**.

---

## 2. Quyết định kiến trúc

### 2.1 Kiến trúc được chọn

```text
                         POST /mcp
                            |
                            v
                 +-----------------------+
                 | HTTP / security gate  |
                 +-----------------------+
                            |
                            v
                 +-----------------------+
                 | JSON-RPC parse        |
                 | protocol-era detect   |
                 +-----------------------+
                      |             |
            +---------+             +----------+
            |                                |
            v                                v
   MCP 2026-07-28                    MCP 2025-11-25
      PRIMARY                           COMPAT
      stateless                         handshake-compatible
            |                                |
   server/discover                     initialize
   tools/list                          notifications/initialized
   tools/call                          tools/list
                                       tools/call
            |                                |
            +---------------+----------------+
                            |
                            v
                    +---------------+
                    | Tool registry |
                    | policy        |
                    +---------------+
                            |
                            v
                  +-------------------+
                  | command_executor  |
                  +-------------------+
                            |
                            v
                  +--------------------+
                  | command_dispatcher |
                  +--------------------+
                            |
                   Gateway / BLE device
```

### 2.2 Không triển khai protocol session

Không thêm:

- `mcp_session.c`;
- session table;
- session ID generator;
- session TTL;
- session cleanup task;
- session persistence trong NVS;
- sticky HTTP connection;
- client identity cache;
- capability cache theo client;
- `Last-Event-ID`;
- resumable SSE.

MCP 2026-07-28 đã bỏ protocol-level session. MCP 2025-11-25 cho phép server **không cấp session ID**, nên compatibility handshake có thể chạy mà không cần `MCP-Session-Id`.

### 2.3 Ý nghĩa của compatibility 2025

Compatibility 2025 trong tài liệu này là:

```text
initialize
    -> InitializeResult
    -> KHÔNG MCP-Session-Id

notifications/initialized
    -> HTTP 202

MCP-Protocol-Version: 2025-11-25
    -> tools/list
    -> tools/call
```

Server **không cố chứng minh** rằng một `tools/call` đến từ đúng HTTP client đã gửi `initialize` trước đó. Đây là deliberate stateless compatibility mode.

---

## 3. Hiện trạng component

Implementation hiện tại đã có các lớp tốt và nên giữ:

```text
mcp_endpoint.c
    route /mcp
    body receive
    JSON-RPC dispatch

mcp_auth.c
    Content-Type
    Bearer token
    Host / Origin
    global rate limit

mcp_codec.c
    MCP-Protocol-Version
    2026 _meta validation
    Mcp-Method / Mcp-Name validation
    server/discover builder

mcp_registry.c
    single source of truth cho tool definitions

mcp_tools.c
    tools/list
    tools/call normalization
    device policy
    result formatting

mcp_rpc.c
    JSON-RPC envelope

command_executor
    asynchronous device command

command_dispatcher
    shared command execution layer
```

Điểm cần thay đổi lớn nhất: current `legacy mode` là **custom JSON-RPC compatibility**, không phải MCP 2025-11-25. Vì vậy không được dùng current legacy branch làm implementation cuối cho Phương án B.

---

## 4. Protocol support matrix sau refactor

| Chức năng | MCP 2026-07-28 | MCP 2025-11-25 |
|---|---:|---:|
| `POST /mcp` | Có | Có |
| `server/discover` | Có | Không cần |
| `initialize` | Không | Có |
| `notifications/initialized` | Không | Có |
| `tools/list` | Có | Có |
| `tools/call` | Có | Có |
| `_meta.protocolVersion` | Bắt buộc/request | Không dùng |
| `_meta.clientCapabilities` | Bắt buộc/request | Negotiated trong initialize |
| `MCP-Protocol-Version` | Bắt buộc mỗi POST | Bắt buộc sau initialize |
| `Mcp-Method` | Bắt buộc request | Không yêu cầu |
| `Mcp-Name` | Bắt buộc khi applicable | Không yêu cầu |
| `MCP-Session-Id` | Không dùng | Không cấp |
| GET SSE | Không | Không; trả 405 |
| DELETE session | Không | Không; trả 405 |
| SSE response | Không trong phase này | Không trong phase này |
| resources/prompts | Không | Không |
| server-to-client request | Không | Không |

---

## 5. Thay đổi khái niệm `legacy mode`

### 5.1 Vấn đề hiện tại

Current project có:

```text
CONFIG_MCP_LEGACY_MODE
NVS namespace: mcp
NVS key: legacy
```

và khi legacy mode hoạt động, wire result sử dụng project-specific shape:

```json
{
  "success": true,
  "status": 0,
  "message": "...",
  "data": {}
}
```

Đây không phải `CallToolResult` của MCP 2025-11-25.

### 5.2 Quyết định mới

Thay:

```text
CONFIG_MCP_LEGACY_MODE
```

bằng:

```text
CONFIG_MCP_COMPAT_2025
```

đề xuất default:

```text
default y
```

Ý nghĩa:

```text
y = hỗ trợ initialize + MCP 2025-11-25 tools
n = chỉ hỗ trợ MCP 2026-07-28
```

### 5.3 NVS key `legacy`

Không dùng NVS runtime flag để thay đổi protocol semantics per-request nữa.

Khuyến nghị:

- bỏ logic đọc `mcp/legacy` khỏi `mcp_codec.c`;
- giữ migration cleanup nếu cần, nhưng không để key cũ ảnh hưởng runtime;
- protocol support được quyết định bằng firmware Kconfig để behavior deterministic.

Nếu vẫn cần custom JSON-RPC API cho web/internal client, đưa nó sang endpoint riêng như `/api/command`; **không giữ custom wire format trong `/mcp`**.

---

## 6. Request processing pipeline mới

Current pipeline kiểm tra `MCP-Protocol-Version` trước khi parse body. Điều này ngăn `initialize` 2025 vì initialize request đầu tiên chưa có negotiated version header.

Pipeline mới:

```text
1. HTTP method / endpoint
2. Security + transport gate
   - Content-Type
   - Accept
   - rate limit
   - Authorization
   - Host
   - Origin
3. Receive body
4. Parse JSON
5. Generic JSON-RPC validation
6. Detect protocol era
7. Era-specific validation
8. Route method
9. Build era-specific result/error
10. Send response
```

Không được gọi `mcp_codec_parse_meta()` theo kiểu hiện tại trước bước 4.

---

## 7. Protocol-era detection

### 7.1 Kiểu dữ liệu mới

Không dùng `bool mcp_2026`. Era phải là enum và request context phải phân biệt dữ liệu raw HTTP header với dữ liệu đã decode/normalize:

```c
#define MCP_PROTOCOL_VERSION_MAX 16
#define MCP_METHOD_MAX           32
#define MCP_NAME_DECODED_MAX     128

typedef enum {
    MCP_ERA_UNKNOWN = 0,
    MCP_ERA_2025_11_25,
    MCP_ERA_2026_07_28,
} mcp_protocol_era_t;

typedef struct {
    mcp_protocol_era_t era;

    bool initialize_request;
    bool notification;

    char protocol_version[MCP_PROTOCOL_VERSION_MAX];
    char mcp_method[MCP_METHOD_MAX];

    /* Decoded logical name only. Raw header must not be stored here. */
    char mcp_name[MCP_NAME_DECODED_MAX + 1];

    bool has_protocol_header;
    bool has_method_header;
    bool has_name_header;
} mcp_request_context_t;
```

`Mcp-Name` raw header may be longer than 128 bytes because Base64 expands the payload. Read it into a temporary allocation or a dedicated raw buffer with an explicit hard limit, decode, validate decoded length `<= MCP_NAME_DECODED_MAX`, then copy only the decoded logical value into `mcp_request_context_t`.

Không có `session` field.

### 7.2 Detection rules — body-aware, no silent fallback

Era detection happens **after generic JSON-RPC parsing**, because the body is needed to distinguish a malformed modern request from a real 2025 compatibility request.

Definitions:

```text
modern_marker =
    MCP-Protocol-Version header is present
    OR params._meta.io.modelcontextprotocol/protocolVersion is present
    OR Mcp-Method / Mcp-Name standard 2026 header is present
```

Normative detection flow:

```c
if (method == "initialize") {
    if (!CONFIG_MCP_COMPAT_2025) {
        reject_unsupported_or_method_not_found();
    }

    /* initialize is the 2025 compatibility entry point. */
    era = MCP_ERA_2025_11_25;
    initialize_request = true;
}
else if (header_version != NULL) {
    if (header_version == "2026-07-28") {
        era = MCP_ERA_2026_07_28;
    }
    else if (header_version == "2025-11-25") {
        if (!CONFIG_MCP_COMPAT_2025)
            reject_unsupported_version_with_data();
        era = MCP_ERA_2025_11_25;
    }
    else {
        reject_unsupported_version_with_data();
    }
}
else if (body_has_2026_protocol_meta || has_mcp_method || has_mcp_name) {
    /* Clearly a modern request, but malformed at the HTTP/header layer. */
    era = MCP_ERA_2026_07_28;
    reject_header_mismatch(-32020, "Missing MCP-Protocol-Version");
}
else {
    /* No reliable era signal. Do not guess from connection history. */
    reject_unsupported_version_or_compatibility_error();
}
```

Important rules:

1. A request carrying 2026 `_meta.protocolVersion` **must never fallback** to `initialize`/2025 behavior just because the HTTP protocol-version header is missing.
2. Unsupported explicit protocol header values use `-32022` with structured `error.data`.
3. Missing required 2026 standard headers on a request already identified as modern use `-32020 HeaderMismatch`.
4. Generic malformed JSON-RPC validation (`jsonrpc`, `method`, `id`) happens before era-specific routing where possible.
5. `id:null` is rejected for MCP requests; absence of `id` means notification.

### 7.3 Không dùng connection để detect era

Không được dựa vào:

- socket ID;
- source IP;
- HTTP keep-alive connection;
- previous request;
- Bearer token;
- clientInfo;
- whether this client previously sent `initialize`.

Mỗi request tự xác định era. Đây là điều kiện để 2026 path thực sự stateless và để mixed-era requests không làm nhiễu nhau.

## 8. MCP 2025 handshake compatibility

### 8.1 `initialize` — version negotiation, không reject ngay

Request hợp lệ tối thiểu:

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "initialize",
  "params": {
    "protocolVersion": "2025-11-25",
    "capabilities": {},
    "clientInfo": {
      "name": "client-name",
      "version": "1.0.0"
    }
  }
}
```

Validate:

- root object;
- `jsonrpc == "2.0"`;
- `id` string hoặc integer, không null;
- `method == "initialize"`;
- `params` object;
- `params.protocolVersion` string;
- `params.capabilities` object;
- `params.clientInfo` object;
- `clientInfo.name` string;
- `clientInfo.version` string.

Compatibility implementation này chỉ thực thi MCP 2025 era ở version:

```text
2025-11-25
```

Nhưng negotiation rule **không** được viết thành "version khác -> JSON-RPC error". Nếu client đề xuất một version khác, server counter-offer version legacy mà server support:

```c
const char *mcp_2025_negotiate(const char *client_version)
{
    (void)client_version;
    return "2025-11-25";
}
```

Ví dụ client gửi:

```json
{"protocolVersion":"2025-06-18"}
```

server vẫn trả `InitializeResult.protocolVersion = "2025-11-25"`. Client chịu trách nhiệm quyết định tiếp tục hay ngắt nếu không support counter-offer đó.

Không route một `initialize` request sang MCP 2026 path.

### 8.2 InitializeResult

Response:

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "protocolVersion": "2025-11-25",
    "capabilities": {
      "tools": {
        "listChanged": false
      }
    },
    "serverInfo": {
      "name": "esp32-ble-gateway",
      "version": "1.0.0"
    },
    "instructions": "Controls BLE devices managed by this ESP32 gateway."
  }
}
```

HTTP response **không** được thêm:

```text
MCP-Session-Id
```

Không lưu `clientInfo`, `clientCapabilities`, negotiated version hoặc initialized state vào session table.

### 8.3 `notifications/initialized`

Client gửi:

```json
{
  "jsonrpc": "2.0",
  "method": "notifications/initialized"
}
```

Interoperability policy:

- notification không có `id`;
- `MCP-Protocol-Version` **có thể vắng** trên notification này;
- nếu header có mặt, value phải là `2025-11-25`;
- nếu header có mặt nhưng unsupported/mismatch, reject ở transport/protocol gate;
- server không lưu initialized state.

Accepted response:

```text
HTTP/1.1 202 Accepted
Content-Length: 0
```

### 8.4 Subsequent 2025 requests

Các request thực sự sau handshake như `tools/list` và `tools/call` phải khai báo:

```text
MCP-Protocol-Version: 2025-11-25
```

Không yêu cầu 2026-specific headers:

```text
Mcp-Method
Mcp-Name
```

và không yêu cầu 2026 per-request `_meta`.

### 8.5 Stateless limitation được chấp nhận

Server không thể enforce theo từng client rằng:

```text
initialize -> notifications/initialized -> tools/call
```

vì không có session state. Đây là trade-off chủ đích của Phương án B.

Server vẫn validate mỗi request độc lập và không được dùng source IP/socket để giả lập lifecycle state.

## 9. MCP 2025 `tools/list`

Response MCP 2025 phải là MCP result thực, không phải project legacy format.

```json
{
  "jsonrpc": "2.0",
  "id": 2,
  "result": {
    "tools": [
      {
        "name": "get_status",
        "description": "...",
        "inputSchema": {
          "type": "object"
        }
      }
    ]
  }
}
```

Không trả các field project-only như:

```text
success
status
message
tool_names
```

`nextCursor` chỉ thêm nếu thực sự implement pagination.

Tool list tiếp tục lấy duy nhất từ `mcp_registry.c`.

---

## 10. MCP 2025 `tools/call`

Request:

```json
{
  "jsonrpc": "2.0",
  "id": 3,
  "method": "tools/call",
  "params": {
    "name": "get_status",
    "arguments": {}
  }
}
```

Success result:

```json
{
  "jsonrpc": "2.0",
  "id": 3,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "..."
      }
    ],
    "isError": false
  }
}
```

### 10.1 Text fallback bắt buộc trong adapter

Nếu dispatcher trả `DISPATCH_RESULT_JSON`, MCP adapter phải serialize payload đó vào `TextContent.text`; không được tạo text item rỗng.

Ví dụ:

```json
{
  "content": [
    {"type":"text","text":"{\"status\":\"ok\"}"}
  ],
  "isError": false
}
```

### 10.2 `structuredContent` 2025 chỉ emit khi JSON root là object

MCP 2025 compatibility path không được đặt JSON array/scalar trực tiếp vào `structuredContent`.

Allowed:

```json
{
  "structuredContent": {
    "status": "ok"
  }
}
```

Not allowed for 2025:

```json
{
  "structuredContent": [
    {"device_id":"a"},
    {"device_id":"b"}
  ]
}
```

Current `list_devices` dispatcher trả JSON array, vì vậy MCP adapter phải chọn một trong hai policy sau; **v1.1 chọn policy A để không thay contract của `command_dispatcher`:**

**Policy A — recommended:**

- luôn trả serialized JSON trong `content[0].text`;
- chỉ thêm `structuredContent` nếu parsed root `cJSON_IsObject(root)`;
- nếu root là array/string/number/bool/null, omit `structuredContent`.

**Policy B — không dùng trong phase này:** wrap adapter-specific object như `{ "devices": [...] }`. Policy này làm đổi semantic shape và có thể gây khác biệt giữa MCP và Web API.

Pseudo-code:

```c
cJSON *parsed = cJSON_Parse(result->payload);
add_text_content(result->payload);

if (ctx->era == MCP_ERA_2025_11_25) {
    if (cJSON_IsObject(parsed))
        add_structured_content(parsed);  /* ownership transferred */
    else
        cJSON_Delete(parsed);
}
```

Tool execution failure vẫn dùng `CallToolResult.isError=true`, không biến thành protocol error.

## 11. MCP 2026 behavior giữ lại

MCP 2026 tiếp tục support:

```text
server/discover
tools/list
tools/call
```

Mỗi modern request phải tự chứa:

```json
"_meta": {
  "io.modelcontextprotocol/protocolVersion": "2026-07-28",
  "io.modelcontextprotocol/clientCapabilities": {}
}
```

`clientInfo` optional nhưng client SHOULD gửi.

HTTP header:

```text
MCP-Protocol-Version: 2026-07-28
Mcp-Method: <method>
Mcp-Name: <decoded/logical name>  # khi applicable
```

Không tạo hoặc đọc protocol session. Nếu request 2026 có `MCP-Session-Id`, ignore header và không echo.

### 11.1 `server/discover.supportedVersions` phải phản ánh config thật

Khi:

```text
CONFIG_MCP_COMPAT_2025=y
```

`server/discover` advertise:

```json
{
  "supportedVersions": [
    "2026-07-28",
    "2025-11-25"
  ]
}
```

Khi compatibility tắt:

```text
CONFIG_MCP_COMPAT_2025=n
```

advertise:

```json
{"supportedVersions":["2026-07-28"]}
```

`mcp_codec_build_discovery()` không được hard-code một list khác với compile/runtime protocol capability thực tế.

### 11.2 Modern malformed request không fallback legacy

Nếu body đã khai báo:

```json
"io.modelcontextprotocol/protocolVersion": "2026-07-28"
```

nhưng thiếu required `MCP-Protocol-Version` header, xử lý như modern header mismatch (`-32020`), không như unknown-era `-32022` và không thử 2025.

### 11.3 Notification policy 2026

Không tạo rule "mọi JSON-RPC object không có id -> HTTP 202".

- known/supported client notification: validate rồi `202 Accepted`;
- unknown id-less method: đi qua explicit unknown-notification policy, không giả vờ thành công;
- current core use-case của gateway không cần generic 2026 client notifications.

# 12. Conformance fixes bắt buộc

Các mục dưới đây là bug/compatibility issue đã phát hiện trong code hiện tại và phải được sửa trong cùng refactor.

---

## 12.1 FIX-P0: notification đang trả 204 thay vì 202

### Hiện trạng

`mcp_rpc_send_no_content()`:

```c
set_status(request, "204 No Content");
```

### Yêu cầu

Đổi semantics thành:

```text
202 Accepted
```

Tạo API rõ nghĩa:

```c
esp_err_t mcp_rpc_send_accepted(httpd_req_t *request);
```

Không dùng tên `send_no_content` vì HTTP 202 mới là contract chính.

### Test

- 2025 `notifications/initialized` -> 202, body empty.
- notification được chấp nhận -> 202, body empty.

---

## 12.2 FIX-P0: MCP 2026 chấp nhận `id:null`

### Hiện trạng

Generic validation hiện cho phép:

```c
cJSON_IsNull(id)
```

### Yêu cầu

Đối với MCP request:

```text
id = string | number
id != null
```

Notification:

```text
id absent
```

### Implementation

Tách helper:

```c
bool mcp_rpc_valid_request_id(const cJSON *id)
{
    return id != NULL &&
           (cJSON_IsString(id) || cJSON_IsNumber(id));
}
```

Không coi `id:null` là notification.

### Test

```json
{"jsonrpc":"2.0","id":null,"method":"tools/list",...}
```

-> `-32600 Invalid Request`.

---

## 12.3 FIX-P0: `Mcp-Name` Base64 sentinel sai

### Hiện trạng

Current decoder dùng sentinel custom tương đương:

```text
\x00b64:
```

### Yêu cầu MCP 2026

Sentinel chính xác:

```text
=?base64?<Base64>?=
```

Ví dụ:

```text
Mcp-Name: =?base64?SGVsbG8=?=
```

### Decoder requirements

- prefix chính xác `=?base64?`;
- suffix chính xác `?=`;
- marker case-sensitive;
- decode UTF-8 bytes;
- reject malformed Base64;
- reject overflow;
- plain ASCII value dùng trực tiếp nếu không matching sentinel;
- nếu plain string tự match sentinel pattern, client phải encode; server luôn decode sentinel.

### Không cần

Không cần generic Base64 library lớn nếu decoder hiện tại đủ an toàn; chỉ sửa framing và validation.

---

## 12.4 FIX-P0: `serverInfo` đang đặt sai tầng

### Hiện trạng

`mcp_rpc.c` đang add metadata lên JSON-RPC envelope:

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "_meta": {
    "io.modelcontextprotocol/serverInfo": {}
  },
  "result": {}
}
```

### Yêu cầu

MCP 2026 server identity nằm trong:

```text
result._meta
```

Đúng:

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "resultType": "complete",
    "_meta": {
      "io.modelcontextprotocol/serverInfo": {
        "name": "esp32-ble-gateway",
        "version": "1.0.0"
      }
    }
  }
}
```

### Refactor

`mcp_rpc.c` **không tự add metadata lên envelope**.

Builder của modern result chịu trách nhiệm gọi:

```c
mcp_result_add_server_info(result);
```

Đảm bảo chỉ add một lần.

### Error response

Không add top-level `_meta` vào JSON-RPC error envelope.

---

## 12.5 FIX-P0: HTTP gate có nguy cơ trả 500 thay vì status đã chọn

### Hiện trạng

Pattern hiện tại:

```c
io->set_status(request, "401 Unauthorized");
...
io->send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, status);
```

`httpd_resp_send_err()` nhận error enum và tự gửi HTTP error tương ứng. Gọi nó với `HTTPD_500_INTERNAL_SERVER_ERROR` sau `set_status()` có thể làm mất status mong muốn.

### Yêu cầu

Không dùng fake 500 để flush custom status.

Tạo helper:

```c
esp_err_t mcp_http_send_plain_status(
    httpd_req_t *req,
    const char *status,
    const char *message,
    bool close_connection);
```

Flow:

```c
set_status(req, status);
set_type(req, "text/plain");
set_hdr(...);
send(req, message, strlen(message));
```

Hoặc production hook sử dụng `httpd_resp_send_custom_err()` nếu test seam được cập nhật phù hợp.

### Expected

| Gate | HTTP |
|---|---:|
| bad Content-Type | 415 |
| bad Accept | 406 hoặc 400 theo project contract |
| unauthorized | 401 |
| forbidden Origin/Host | 403 |
| rate limit | 429 |

Không được biến thành 500.

---

## 12.6 FIX-P1: `Accept` validation là transport policy, không phải hard conformance gate

MCP Streamable HTTP clients thường gửi:

```text
Accept: application/json, text/event-stream
```

Nhưng v1.1 không coi việc server reject mọi request thiếu đủ hai media type là bắt buộc cho interoperability.

### Yêu cầu

Thêm parser:

```c
mcp_accept_state_t mcp_http_parse_accept(httpd_req_t *req);
```

Parser phải:

- case-insensitive media type;
- xử lý comma-separated values;
- xử lý optional parameters như `q=`;
- biết client có chấp nhận `application/json` hay không;
- biết client có advertise `text/event-stream` hay không.

### Default policy

```text
CONFIG_MCP_STRICT_ACCEPT_HEADER=n
```

Behavior mặc định:

| Accept | Default behavior |
|---|---|
| cả `application/json` và `text/event-stream` | accept |
| chỉ `application/json` | accept; optional debug warning |
| header vắng | accept for compatibility |
| không chấp nhận `application/json` | `406 Not Acceptable` |

Strict mode dùng cho conformance/lab testing:

```text
CONFIG_MCP_STRICT_ACCEPT_HEADER=y
```

khi đó header phải advertise cả JSON và event-stream.

Không ghi warning mỗi request gây log spam; nếu warning được bật, rate-limit hoặc log một lần.

## 12.7 FIX-P1: Content-Type parser quá permissive

Current code dùng prefix comparison:

```c
strncasecmp(content_type, "application/json", strlen("application/json")) == 0
```

Điều này có thể chấp nhận chuỗi bắt đầu bằng `application/json` nhưng không thực sự là media type đó.

### Yêu cầu

Chỉ chấp nhận:

```text
application/json
application/json; charset=utf-8
```

và variants hợp lệ của parameter syntax.

Không chấp nhận prefix giả.

---

## 12.8 FIX-P1: project-local error codes đang dùng vùng không nên cấp mới

Current project khai báo:

```text
-32000 Gateway busy
-32001 Device unavailable
-32002 Command denied
-32003 Capability unavailable
```

MCP 2026 phân vùng `-32020..-32099` cho protocol-defined MCP errors và xem `-32000..-32019` là legacy allocation; new implementations không nên tạo nghĩa mới trong vùng này.

### Quyết định

Chỉ giữ MCP-defined:

```text
-32020 HeaderMismatch
-32021 MissingRequiredClientCapability
-32022 UnsupportedProtocolVersion
```

Project overload dùng code ngoài reserved server range, ví dụ:

```c
#define MCP_APP_ERR_GATEWAY_BUSY (-31000)
```

Nhưng ưu tiên **không tạo JSON-RPC error** cho lỗi tool execution.

### Mapping mới

| Tình huống | Wire behavior |
|---|---|
| malformed tools/call | JSON-RPC `-32602` |
| unknown tool | JSON-RPC `-32602` |
| queue/executor unavailable trước execution | HTTP 503 + project error `-31000` |
| device offline | CallToolResult `isError:true` |
| command denied bởi policy | CallToolResult `isError:true` |
| capability chưa ready | CallToolResult `isError:true` |
| BLE timeout/device error | CallToolResult `isError:true` |

Tool execution failure là model-actionable error, không phải protocol failure.

---

## 12.9 FIX-P1: `tools/list` đang expose `tool_names`

Current result thêm cả:

```json
{
  "tools": [],
  "tool_names": []
}
```

`tool_names` là project-specific convenience field.

### Yêu cầu

Bỏ khỏi MCP wire result.

Nếu internal code cần names array, giữ internal only hoặc build từ registry.

---

## 12.10 FIX-P1: 2025 CallToolResult chưa đúng chuẩn

Current non-2026 branch trả project wire format:

```json
{
  "success": true,
  "status": 0,
  "message": "..."
}
```

### Yêu cầu

MCP 2025 phải trả:

```json
{
  "content": [
    {"type":"text","text":"..."}
  ],
  "isError": false
}
```

Rules:

1. `DISPATCH_RESULT_TEXT` -> `TextContent.text = payload`.
2. `DISPATCH_RESULT_JSON` -> `TextContent.text = serialized JSON payload`.
3. `structuredContent` 2025 chỉ emit khi parsed JSON root là object.
4. JSON array/scalar -> text fallback only; không emit invalid 2025 `structuredContent`.
5. operational failure -> same CallToolResult shape với `isError:true`.

Đây là thay đổi bắt buộc để Phương án B thực sự tương thích client MCP.

---

## 12.11 FIX-P0: `UnsupportedProtocolVersion` phải có structured `error.data`

`-32022` không chỉ là một code/message pair. Builder phải tạo data có requested version và danh sách version server support.

Expected shape:

```json
{
  "jsonrpc":"2.0",
  "id":1,
  "error": {
    "code": -32022,
    "message": "Unsupported protocol version",
    "data": {
      "supported": ["2026-07-28", "2025-11-25"],
      "requested": "1900-01-01"
    }
  }
}
```

Nếu `CONFIG_MCP_COMPAT_2025=n`, `supported` chỉ chứa `2026-07-28`.

`requested` là exact explicit unsupported version received. Missing required modern header is **not** represented by fake requested string; once request is identified as modern, missing header uses `-32020` instead.

Required helper:

```c
cJSON *mcp_protocol_build_unsupported_version_data(const char *requested);
```

Error builder ownership must be explicit; see §15.

---

## 12.12 FIX-P1: MCP 2026 unknown/id-less method không được auto-accept

Current generic `notification = id == NULL` handling can encourage a catch-all `202` path.

Requirements:

- only a recognized notification method may return `202 Accepted`;
- `notifications/initialized` belongs only to 2025 compatibility path;
- arbitrary unknown id-less 2026 method must follow explicit unknown-notification behavior and must not call tool execution;
- tests must not encode "all no-id requests succeed".

---

## 12.13 FIX-P1: `Mcp-Name` buffer/ownership must prevent Base64 truncation

Decoded logical tool/resource names may be up to the project-supported limit (128 bytes). Raw encoded header is larger.

Requirements:

- raw header fetched as temporary heap buffer or dedicated raw buffer with a hard cap >= Base64 expansion + sentinel;
- decode `=?base64?<payload>?=` before comparison;
- reject decoded length > `MCP_NAME_DECODED_MAX`;
- copy only decoded logical name into request context;
- always free raw header on every exit path.

---

## 12.14 FIX-P1: README/Kconfig không đồng bộ

Current documentation và Kconfig từng mô tả khác nhau về legacy default.

Sau refactor:

- README chỉ mô tả `MCP_COMPAT_2025`;
- Kconfig help đồng nhất;
- tests không phụ thuộc NVS runtime legacy override;
- xóa mô tả wire format `{success,message,data}` khỏi README MCP.

# 13. HTTP endpoint contract sau refactor

## 13.1 POST `/mcp`

Required request body media type:

```text
Content-Type: application/json
```

Recommended MCP client header:

```text
Accept: application/json, text/event-stream
```

Default gateway policy accepts missing/JSON-only Accept for interoperability as long as JSON is acceptable. `CONFIG_MCP_STRICT_ACCEPT_HEADER=y` may enforce the two-media-type form in lab/conformance builds.

Security:

```text
Authorization: Bearer <token>    # nếu configured
Host                              # validate theo policy hiện có
Origin                            # nếu có, validate
```

### MCP 2026

```text
MCP-Protocol-Version: 2026-07-28
Mcp-Method: ...
Mcp-Name: ... # applicable methods
```

Modern body marker + missing required protocol header -> `-32020`, not legacy fallback.

### MCP 2025 initialize

Protocol header có thể vắng ở request đầu. `initialize.params.protocolVersion` participates in 2025 negotiation.

### MCP 2025 `notifications/initialized`

Version header optional for interoperability. If present it must equal `2025-11-25`.

### MCP 2025 subsequent requests

```text
MCP-Protocol-Version: 2025-11-25
```

Không yêu cầu `Mcp-Method` / `Mcp-Name` cho 2025.

---

## 13.2 GET `/mcp`

Không implement standalone SSE stream.

Trả:

```text
405 Method Not Allowed
Allow: POST
```

Không advertise GET là allowed method chỉ vì có một handler chuyên trả 405.

---

## 13.3 DELETE `/mcp`

Do server không tạo session ID:

```text
405 Method Not Allowed
Allow: POST
```

Không tạo no-op delete session API.

# 14. Module design đề xuất

## 14.1 `mcp_endpoint.c`

Trách nhiệm sau refactor:

- register POST/GET/DELETE route;
- receive body;
- generic JSON-RPC parse;
- call protocol detector;
- route method;
- async handoff cho device command.

Không nên chứa chi tiết validate initialize payload.

---

## 14.2 `mcp_protocol.c` — file mới

Nên thêm file mới để tách lifecycle/protocol-era khỏi codec.

API gợi ý:

```c
int mcp_protocol_detect(
    httpd_req_t *req,
    const cJSON *root,
    mcp_request_context_t *ctx,
    mcp_rpc_error_detail_t *error);

int mcp_protocol_validate_request(
    const cJSON *root,
    const mcp_request_context_t *ctx,
    mcp_rpc_error_detail_t *error);

cJSON *mcp_protocol_build_initialize_result(
    const cJSON *params,
    mcp_rpc_error_detail_t *error);

bool mcp_protocol_is_initialized_notification(const cJSON *root);
```

`mcp_protocol.c` xử lý:

- 2025 vs 2026 detection;
- supported versions;
- 2025 initialize validation;
- 2026 metadata validation;
- standard header/body consistency.

---

## 14.3 `mcp_codec.c`

Sau refactor chỉ giữ wire utilities:

- header read helpers;
- Base64 sentinel decode;
- result `_meta.serverInfo` helper;
- `server/discover` builder;
- small encoding/decoding helpers.

Xóa:

- NVS legacy resolution;
- runtime legacy override.

---

## 14.4 `mcp_rpc.c`

Trách nhiệm:

- JSON-RPC envelope;
- JSON send;
- status override;
- `202 Accepted`;
- application/json response.

Không quyết định protocol era business logic.

API gợi ý:

```c
esp_err_t mcp_rpc_send_result(
    httpd_req_t *req,
    cJSON *result,
    const cJSON *id);

esp_err_t mcp_rpc_send_error_ex(...);

esp_err_t mcp_rpc_send_accepted(httpd_req_t *req);
```

Không auto-add `_meta` lên envelope.

---

## 14.5 `mcp_tools.c`

Tách formatting theo era:

```c
cJSON *mcp_tools_format_dispatch_2025(...);
cJSON *mcp_tools_format_dispatch_2026(...);
```

hoặc một entry point với explicit era:

```c
cJSON *mcp_tools_format_dispatch(
    const dispatch_result_t *result,
    const mcp_request_context_t *ctx,
    mcp_rpc_error_t *err);
```

### 2025

```text
content
isError
structuredContent optional, object-only
```

Rules:

- JSON result always gets serialized text fallback;
- add `structuredContent` only if parsed root is a JSON object;
- array/scalar JSON does not become 2025 `structuredContent`;
- do not wrap arrays into a new adapter-specific schema in this phase.

### 2026

```text
resultType: complete
content
isError
structuredContent optional
_meta.serverInfo
```

For JSON results, `content[0].text` should contain the serialized payload instead of an empty string. 2026 `structuredContent` may preserve supported JSON value shape according to the target 2026 contract.

Không tồn tại branch `{success,status,message,data}` trong MCP code.

## 14.6 `mcp_auth.c`

Thêm:

- strict Content-Type parser;
- Accept parser.

Giữ:

- Bearer token constant-time compare;
- Origin validation;
- Host allowlist;
- rate limit.

Nên đổi tên gate thành:

```c
mcp_http_gate()
```

vì chức năng không chỉ auth.

---

# 15. Internal types và ownership contract đề xuất

```c
typedef enum {
    MCP_ERA_UNKNOWN = 0,
    MCP_ERA_2025_11_25,
    MCP_ERA_2026_07_28,
} mcp_protocol_era_t;

typedef struct {
    mcp_protocol_era_t era;
    bool initialize_request;
    bool notification;

    char protocol_version[16];
    char mcp_method[32];
    char mcp_name[MCP_NAME_DECODED_MAX + 1];

    bool has_protocol_header;
    bool has_method_header;
    bool has_name_header;
} mcp_request_context_t;

typedef struct {
    int rpc_code;
    const char *message;      /* borrowed static/string-lifetime pointer */
    const char *http_status;  /* borrowed pointer or NULL */
    cJSON *data;              /* owned by detail until transferred */
} mcp_rpc_error_detail_t;
```

### 15.1 `cJSON *data` ownership

Normative rule:

```text
producer creates data
        |
        v
mcp_rpc_error_detail_t owns data
        |
        +-- send/build success -> ownership transfers into JSON-RPC error object
        |
        +-- early failure/cancel -> caller deletes data
```

Required helpers:

```c
void mcp_rpc_error_detail_init(mcp_rpc_error_detail_t *detail);
void mcp_rpc_error_detail_clear(mcp_rpc_error_detail_t *detail);
```

`clear()` must `cJSON_Delete(detail->data)` if ownership was not transferred.

No function may both attach `data` to a parent cJSON tree and then free it separately.

### 15.2 Lợi ích

- không còn `bool mcp_2026` rải rác;
- error builder có thể chứa required `supported/requested` data;
- raw `Mcp-Name` is not accidentally truncated into the decoded field;
- future protocol era does not require nested booleans;
- ownership is deterministic on OOM/error paths.

# 16. Router logic đề xuất

```c
static esp_err_t route_request(
    httpd_req_t *req,
    cJSON *root,
    const cJSON *id,
    const mcp_request_context_t *ctx)
{
    const char *method = ...;

    if (ctx->era == MCP_ERA_2025_11_25) {
        if (strcmp(method, "initialize") == 0)
            return handle_initialize_2025(...);

        if (strcmp(method, "notifications/initialized") == 0)
            return mcp_rpc_send_accepted(req);
    }

    if (ctx->era == MCP_ERA_2026_07_28 &&
        strcmp(method, "server/discover") == 0)
        return handle_discover(...);

    if (strcmp(method, "tools/list") == 0)
        return handle_tools_list(...);

    if (strcmp(method, "tools/call") == 0)
        return handle_tools_call(...);

    return method_not_found_for_era(...);
}
```

Không hỗ trợ alias:

```text
list_tools
call_tool
```

trên MCP endpoint sau migration.

Nếu alias còn cần cho client cũ của project, chuyển sang non-MCP API.

---

# 17. Tool execution error policy

## 17.1 Protocol error

Dùng JSON-RPC `error` khi request không thể được hiểu/executed như MCP request:

- malformed JSON;
- invalid JSON-RPC;
- unknown method;
- unknown tool;
- malformed tool arguments schema;
- unsupported protocol version;
- missing 2026 metadata;
- header mismatch;
- server internal failure trước tool execution.

## 17.2 Tool execution error

Dùng `result.isError = true` khi tool hợp lệ nhưng execution thất bại:

- device không connected;
- device không tồn tại tại thời điểm execution;
- capability chưa ready;
- command bị policy deny;
- BLE timeout;
- peripheral trả error;
- dispatcher business validation failure.

Ví dụ:

```json
{
  "jsonrpc": "2.0",
  "id": 7,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "Device is currently unavailable"
      }
    ],
    "isError": true
  }
}
```

2026 thêm:

```json
"resultType": "complete"
```

và `result._meta.serverInfo`.

---

# 18. Async command flow không thay đổi kiến trúc

Giữ:

```text
HTTP handler
    |
command_executor_submit()
    |
HTTP async handler context
    |
command dispatcher
    |
completion callback
    |
JSON-RPC result
```

Context cần lưu:

```c
typedef struct {
    httpd_req_t *request;
    cJSON *id;
    mcp_request_context_t protocol;
    bool notification;
} mcp_command_context_t;
```

Không lưu pointer vào parsed root sau khi handler return.

Không tạo session object.

---

# 19. Kconfig thay đổi

Đề xuất:

```text
menu "MCP Endpoint"

config MCP_COMPAT_2025
    bool "Enable MCP 2025-11-25 initialize compatibility"
    default y
    help
        Accept MCP 2025-11-25 initialize/initialized flow and subsequent
        tools/list and tools/call requests. The server does not create an
        MCP-Session-Id and keeps no per-client protocol session state.

config MCP_STRICT_ACCEPT_HEADER
    bool "Require both MCP Streamable HTTP Accept media types"
    default n

config MCP_AUTH_TOKEN
    ... existing ...

config MCP_HOST_ALLOWLIST
    ... existing ...

config MCP_DEVICE_COMMAND_ALLOWLIST
    ... existing ...

config MCP_RATE_LIMIT_RPS
    ... existing ...

endmenu
```

Xóa:

```text
MCP_LEGACY_MODE
```

và runtime NVS legacy override API.

---

# 20. CMake thay đổi

Nếu thêm `mcp_protocol.c`:

```cmake
idf_component_register(
    SRCS
        "mcp_endpoint.c"
        "mcp_auth.c"
        "mcp_codec.c"
        "mcp_protocol.c"
        "mcp_registry.c"
        "mcp_rpc.c"
        "mcp_tools.c"
    ...
)
```

Không thêm component dependency mới chỉ để xử lý handshake.

---

# 21. Test strategy

Test suite nên tách thành 4 nhóm:

```text
[mcp_http]
[mcp_2025]
[mcp_2026]
[mcp_stress]
```

## 21.1 MCP 2025 tests

Bắt buộc:

1. initialize hợp lệ không cần protocol header.
2. InitializeResult trả `protocolVersion=2025-11-25`.
3. initialize client proposes `2025-06-18` -> server **counter-offers `2025-11-25`**, không trả `-32602` chỉ vì version khác.
4. capabilities chỉ advertise tools.
5. response không có `MCP-Session-Id`.
6. `notifications/initialized` không version header -> HTTP 202 empty body.
7. `notifications/initialized` + `MCP-Protocol-Version: 2025-11-25` -> HTTP 202.
8. initialized notification có `id` -> invalid request.
9. initialized notification có explicit unsupported version header -> reject.
10. `tools/list` với 2025 header -> chuẩn ListToolsResult.
11. `tools/list` không có `tool_names`.
12. `tools/call` success -> `content` + `isError:false`.
13. JSON object dispatcher result -> text fallback + 2025 `structuredContent` object.
14. JSON array dispatcher result (e.g. list devices) -> text fallback, **no `structuredContent`**.
15. tool execution failure -> `isError:true`.
16. 2025 result không có `resultType` requirement.
17. 2025 request không cần `_meta`.
18. 2025 request không cần `Mcp-Method`.
19. 2025 subsequent request without version header -> reject per compatibility contract.
20. 2025 disabled bằng Kconfig -> initialize bị reject/method unavailable according to chosen contract.

## 21.2 MCP 2026 tests

Bắt buộc sửa/viết:

1. supported version works.
2. explicit unsupported version -> `-32022` + HTTP 400 + `error.data.supported` + `error.data.requested`.
3. `supportedVersions` contains `2026-07-28` and, when compat enabled, `2025-11-25`.
4. modern body marker + missing protocol header -> `-32020` + HTTP 400; **must not fallback to 2025**.
5. header/body protocol mismatch -> `-32020` + HTTP 400.
6. missing `_meta.protocolVersion` -> `-32602` + HTTP 400.
7. missing `_meta.clientCapabilities` -> `-32602` + HTTP 400.
8. `id:null` -> `-32600`.
9. missing `Mcp-Method` -> `-32020`.
10. method mismatch -> `-32020`.
11. missing `Mcp-Name` on tools/call -> `-32020`.
12. Mcp-Name plain match works.
13. Mcp-Name encoded sentinel valid works at maximum supported decoded length.
14. raw encoded name longer than decoded buffer is handled without truncation.
15. malformed Base64 -> `-32020`.
16. decoded Mcp-Name mismatch -> `-32020`.
17. `server/discover` result shape đúng.
18. `tools/list` has `resultType:complete`.
19. `tools/list` has `ttlMs/cacheScope`.
20. serverInfo nằm ở `result._meta`, không top-level.
21. error response không có top-level protocol `_meta` custom field.
22. unknown request method -> HTTP 404 + `-32601`.
23. arbitrary unknown id-less modern method is **not silently accepted as 202**.
24. modern request mang `MCP-Session-Id` vẫn xử lý stateless và không echo session.
25. JSON dispatcher payload produces non-empty text fallback.

## 21.3 HTTP gate tests

1. missing Content-Type -> 415.
2. malformed Content-Type prefix -> reject.
3. `application/json; charset=utf-8` -> accept.
4. default mode: missing Accept -> accept.
5. default mode: Accept only `application/json` -> accept.
6. Accept that excludes/cannot receive JSON -> 406.
7. strict mode: missing Accept -> reject.
8. strict mode: only one required media type -> reject.
9. strict mode: both `application/json` and `text/event-stream` -> accept.
10. unauthorized -> HTTP 401 thực, không 500.
11. invalid Origin -> HTTP 403 thực, không 500.
12. rate limited -> HTTP 429 thực, không 500.
13. gate rejection giữ `Connection: close` khi body chưa drain.

## 21.4 Method route tests

1. GET `/mcp` -> 405 + `Allow: POST`.
2. DELETE `/mcp` -> 405 + `Allow: POST`.
3. POST -> registered.
4. legacy alias `list_tools` -> method not found.
5. legacy alias `call_tool` -> method not found.

## 21.5 Stress / memory tests

Giữ các stress test hiện có và thêm dual-era cycles:

```text
100x initialize 2025
100x tools/list 2025
100x server/discover 2026
100x tools/list 2026
100x alternating 2025/2026
```

Verify:

- no session allocation accumulation;
- no raw `Mcp-Name` leak;
- no error.data leak on unsupported-version cycles;
- async command context always frees copied ID/context;
- heap baseline ổn định sau warm-up.

# 22. Test fixtures cần sửa

Current `test_mcp_conformance.c` đang assert serverInfo tại:

```text
response._meta
```

Phải đổi thành:

```text
response.result._meta
```

Current test matrix đang gọi non-versioned mode là `legacy`. Đổi naming:

```text
legacy -> mcp_2025
modern -> mcp_2026
```

Không giữ test xác nhận custom `{success,message,data}` format trong MCP suite.

Nếu project vẫn cần compatibility với custom clients, move test đó sang API component khác.

---

# 23. Implementation phases

## Phase 1 — Conformance cleanup P0

Files:

```text
mcp_rpc.c
mcp_codec.c
mcp_endpoint.c
mcp_protocol.c (new if introduced early)
test_mcp_conformance.c
```

Tasks:

- notification helper returns 202 only for recognized/supported notifications;
- reject null ID;
- Base64 sentinel + raw/decoded length safety;
- `result._meta` placement;
- gate status bug;
- structured `-32022 error.data` builder;
- modern missing-header -> `-32020`, no legacy fallback;
- `server/discover.supportedVersions` derived from actual supported eras.

**Không thêm broad compatibility behavior trước khi Phase 1 xanh test.**

## Phase 2 — Protocol era abstraction

Files:

```text
mcp_endpoint_internal.h
mcp_protocol.c   # new
mcp_codec.c
mcp_endpoint.c
```

Tasks:

- add enum;
- remove `bool mcp_2026` branching;
- move version detection sau body parse;
- add modern-marker detection;
- add supported-version helpers;
- define error-data ownership helpers.

## Phase 3 — MCP 2025 handshake

Tasks:

- initialize validator;
- 2025 version counter-offer negotiation;
- InitializeResult builder;
- initialized notification handler with optional version header;
- no session ID;
- version header for subsequent requests.

## Phase 4 — Real MCP 2025 tools wire format

Tasks:

- 2025 tools/list;
- 2025 tools/call;
- non-empty JSON text fallback;
- 2025 object-only `structuredContent`;
- remove `tool_names`;
- remove `{success,status,message,data}` from `/mcp`.

## Phase 5 — HTTP transport completeness

Tasks:

- compatibility-friendly Accept parser;
- strict Accept optional mode, default off;
- strict Content-Type parser;
- GET 405 + `Allow: POST`;
- DELETE 405 + `Allow: POST`;
- regression tests.

## Phase 6 — Error policy cleanup

Tasks:

- remove project meanings from `-32000..-32019`;
- convert device/policy/capability operational failures to tool errors;
- use a project JSON-RPC code outside MCP reserved/server ranges only where a protocol-level project error is truly needed.

## Phase 7 — Documentation and migration cleanup

Tasks:

- update README;
- update Kconfig help/defaults;
- remove NVS legacy flag APIs;
- update curl examples;
- document protocol matrix;
- note that endpoint is dual-era but sessionless;
- document unsupported-version and structuredContent edge cases.

# 24. File-by-file implementation checklist

## `components/mcp_endpoint/mcp_endpoint.c`

- [ ] Move protocol detect after JSON parse/generic validation.
- [ ] Add modern-marker detection so malformed modern requests never fallback 2025.
- [ ] Add initialize route.
- [ ] Add initialized notification route.
- [ ] Do not catch-all arbitrary id-less methods as successful notifications.
- [ ] Add era-aware unknown method handling.
- [ ] Register GET handler returning 405 + `Allow: POST`.
- [ ] Register DELETE handler returning 405 + `Allow: POST`.
- [ ] Stop handling `list_tools` alias.
- [ ] Stop handling `call_tool` alias.
- [ ] Change async context from `mcp_request_meta_t` to new request context.
- [ ] Reject null request IDs.

## `components/mcp_endpoint/mcp_protocol.c` — new

- [ ] Detect era from header + parsed body markers.
- [ ] Validate initialize.
- [ ] Implement 2025 version counter-offer negotiation.
- [ ] Build InitializeResult.
- [ ] Validate 2026 per-request `_meta`.
- [ ] Validate required modern protocol header/body relation.
- [ ] Validate Mcp-Method.
- [ ] Validate Mcp-Name.
- [ ] Build actual supported-version list from config.
- [ ] Build `-32022` `error.data` with `supported` and `requested`.

## `components/mcp_endpoint/mcp_codec.c`

- [ ] Replace Base64 sentinel.
- [ ] Separate raw/decoded Mcp-Name handling and enforce decoded max length.
- [ ] Keep serverInfo builder with correct `result._meta` ownership.
- [ ] Discovery builder uses dynamic supported-version helper.
- [ ] Remove NVS legacy logic.
- [ ] Remove old legacy override API.

## `components/mcp_endpoint/mcp_rpc.c`

- [ ] Add 202 Accepted helper.
- [ ] Remove top-level `_meta` injection.
- [ ] Preserve exact HTTP status.
- [ ] Support JSON-RPC error `data` ownership transfer.
- [ ] Add init/clear helper or equivalent deterministic cleanup.
- [ ] Ensure error envelopes stay valid JSON-RPC.

## `components/mcp_endpoint/mcp_tools.c`

- [ ] Build 2025 ListToolsResult.
- [ ] Build 2026 ListToolsResult.
- [ ] Remove `tool_names` from wire.
- [ ] Build 2025 CallToolResult.
- [ ] Build 2026 CallToolResult.
- [ ] JSON dispatch result always produces non-empty text fallback.
- [ ] 2025 `structuredContent` emitted only for JSON object root.
- [ ] Convert operational failures to `isError:true`.
- [ ] Remove custom legacy output.

## `components/mcp_endpoint/mcp_auth.c`

- [ ] Strict Content-Type validation.
- [ ] Parse Accept into capability state.
- [ ] Default compatibility policy accepts missing/JSON-only Accept.
- [ ] Optional strict mode enforces both MCP media types.
- [ ] Keep auth/rate/origin behavior.

## `components/mcp_endpoint/mcp_endpoint_internal.h`

- [ ] Add protocol era enum.
- [ ] Add request context with decoded-name max.
- [ ] Add error-detail ownership type/helpers.
- [ ] Update prototypes.
- [ ] Remove old legacy APIs.
- [ ] Remove project allocation of MCP reserved/legacy error codes.

## `components/mcp_endpoint/Kconfig.projbuild`

- [ ] Replace `MCP_LEGACY_MODE` with `MCP_COMPAT_2025`.
- [ ] Add `MCP_STRICT_ACCEPT_HEADER` default `n` if retained.
- [ ] Update help text to distinguish compatibility policy from conformance requirement.

## `components/mcp_endpoint/README.md`

- [ ] Replace legacy/modern wording with 2025/2026 era wording.
- [ ] Document no `MCP-Session-Id`.
- [ ] Document 2025 version counter-offer behavior.
- [ ] Add initialize example.
- [ ] Add server/discover example with dual-era supportedVersions.
- [ ] Add 2025 and 2026 tools/call examples.
- [ ] Document 2025 object-only structuredContent behavior.
- [ ] Add GET/DELETE 405 + `Allow: POST` behavior.
- [ ] Remove custom wire format examples.

## `components/mcp_endpoint/test/`

- [ ] Add `test_mcp_2025.c`.
- [ ] Rename/rework conformance tests for 2026.
- [ ] Add initialize counter-offer test.
- [ ] Add modern-missing-header-no-fallback test.
- [ ] Add `-32022 error.data` tests.
- [ ] Add dynamic supportedVersions tests with compat on/off.
- [ ] Fix serverInfo assertion path.
- [ ] Add Base64 sentinel + max-length/raw-length tests.
- [ ] Add null-ID tests.
- [ ] Add 2025 array structuredContent omission test.
- [ ] Add non-empty JSON text fallback test.
- [ ] Add Accept default/strict policy tests.
- [ ] Add exact HTTP gate status tests.
- [ ] Add unknown-id-less method test.
- [ ] Add mixed-era stress test.

# 25. Manual verification examples

## 25.1 MCP 2025 initialize — exact supported version

```sh
curl -i http://<gateway>/mcp \
  -X POST \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -H 'Authorization: Bearer <token>' \
  -d '{
    "jsonrpc":"2.0",
    "id":1,
    "method":"initialize",
    "params":{
      "protocolVersion":"2025-11-25",
      "capabilities":{},
      "clientInfo":{"name":"curl-test","version":"1.0"}
    }
  }'
```

Expected:

```text
HTTP 200
no MCP-Session-Id
result.protocolVersion = 2025-11-25
result.capabilities.tools exists
```

## 25.2 MCP 2025 initialize — counter-offer

Client intentionally proposes another version:

```sh
curl -i http://<gateway>/mcp \
  -X POST \
  -H 'Content-Type: application/json' \
  -H 'Authorization: Bearer <token>' \
  -d '{
    "jsonrpc":"2.0",
    "id":2,
    "method":"initialize",
    "params":{
      "protocolVersion":"2025-06-18",
      "capabilities":{},
      "clientInfo":{"name":"curl-test","version":"1.0"}
    }
  }'
```

Expected:

```text
HTTP 200
result.protocolVersion = 2025-11-25
no -32602 solely because proposed version differed
```

## 25.3 MCP 2025 initialized — header optional

```sh
curl -i http://<gateway>/mcp \
  -X POST \
  -H 'Content-Type: application/json' \
  -H 'Authorization: Bearer <token>' \
  -d '{"jsonrpc":"2.0","method":"notifications/initialized"}'
```

Expected:

```text
HTTP 202
empty body
```

Run again with `MCP-Protocol-Version: 2025-11-25`; expected result is also 202.

## 25.4 MCP 2025 tools/list

```sh
curl -i http://<gateway>/mcp \
  -X POST \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -H 'MCP-Protocol-Version: 2025-11-25' \
  -H 'Authorization: Bearer <token>' \
  -d '{"jsonrpc":"2.0","id":3,"method":"tools/list","params":{}}'
```

Expected result contains `tools` and does not contain:

```text
success
status
message
tool_names
resultType
```

## 25.5 MCP 2025 list_devices structured output edge case

Call a tool whose dispatcher payload is a JSON array, such as device listing.

Expected:

```text
result.content[0].text contains serialized JSON array
result.structuredContent is absent
```

## 25.6 MCP 2026 server/discover

```sh
curl -i http://<gateway>/mcp \
  -X POST \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -H 'MCP-Protocol-Version: 2026-07-28' \
  -H 'Mcp-Method: server/discover' \
  -H 'Authorization: Bearer <token>' \
  -d '{
    "jsonrpc":"2.0",
    "id":1,
    "method":"server/discover",
    "params":{
      "_meta":{
        "io.modelcontextprotocol/protocolVersion":"2026-07-28",
        "io.modelcontextprotocol/clientCapabilities":{}
      }
    }
  }'
```

Expected when compatibility is enabled:

```text
result.resultType = complete
result.supportedVersions contains 2026-07-28
result.supportedVersions contains 2025-11-25
result._meta.io.modelcontextprotocol/serverInfo exists
response._meta does NOT exist
```

When compatibility is disabled, `supportedVersions` must not claim `2025-11-25`.

## 25.7 MCP 2026 missing required protocol header must not fallback

Send a modern body with `_meta.protocolVersion=2026-07-28` but omit the HTTP `MCP-Protocol-Version` header.

Expected:

```text
HTTP 400
JSON-RPC error.code = -32020
no initialize/2025 path attempted
```

## 25.8 MCP 2026 unsupported explicit version

Send:

```text
MCP-Protocol-Version: 1900-01-01
```

Expected error includes:

```json
{
  "code": -32022,
  "data": {
    "supported": ["2026-07-28", "2025-11-25"],
    "requested": "1900-01-01"
  }
}
```

The 2025 item is present only when compatibility is enabled.

## 25.9 MCP 2026 Base64 Mcp-Name

Body tool name Unicode/non-header-safe phải được so với decoded header:

```text
Mcp-Name: =?base64?<base64>?=
```

Expected:

- valid encoded name + matching body -> accepted;
- malformed encoding -> HTTP 400 / `-32020`;
- decoded mismatch -> HTTP 400 / `-32020`;
- maximum supported decoded name is handled without raw-header truncation.

# 26. RAM / resource impact

Phương án B cố tình tránh session manager.

Expected new persistent memory gần như chỉ gồm:

- protocol enum/context trên stack;
- small temporary header strings;
- current HTTP request body;
- existing async command context.

Không có:

```text
N clients * session struct
session locks
timer task
TTL index
session token strings
```

Mục tiêu sau refactor:

```text
MCP compatibility overhead persistent RAM ~= negligible
```

Temporary allocations phải được release trong cùng request hoặc async completion callback.

---

# 27. Security constraints giữ nguyên

Gateway vẫn là LAN endpoint.

Giữ nguyên nguyên tắc:

- validate Origin;
- Host allowlist chỉ là DNS-rebinding defense, không phải auth;
- Bearer token constant-time compare;
- token không log;
- rate limit;
- không expose plaintext HTTP ra Internet;
- không dùng `clientInfo` để authorization;
- không dùng source IP như session identity.

MCP 2025 compatibility không được làm giảm security gate hiện có.

---

# 28. Definition of Done

Refactor chỉ được xem là hoàn tất khi đạt tất cả điều kiện sau:

### Protocol

- [ ] MCP 2026 `server/discover` hoạt động.
- [ ] MCP 2026 `tools/list` hoạt động.
- [ ] MCP 2026 `tools/call` hoạt động.
- [ ] `server/discover.supportedVersions` phản ánh đúng `CONFIG_MCP_COMPAT_2025`.
- [ ] MCP 2025 `initialize` hoạt động.
- [ ] MCP 2025 initialize version khác được counter-offer `2025-11-25` thay vì reject sai.
- [ ] MCP 2025 `notifications/initialized` trả 202 với header vắng hoặc đúng version.
- [ ] MCP 2025 `tools/list` hoạt động với MCP shape.
- [ ] MCP 2025 `tools/call` hoạt động với MCP CallToolResult.
- [ ] Không tạo `MCP-Session-Id`.
- [ ] Không có session state.

### Conformance fixes

- [ ] Modern body marker + missing protocol header -> `-32020`, không fallback 2025.
- [ ] Explicit unsupported protocol -> `-32022` với `error.data.supported/requested`.
- [ ] `id:null` bị reject.
- [ ] Base64 sentinel là `=?base64?...?=`.
- [ ] Raw encoded `Mcp-Name` không bị truncate vào decoded buffer.
- [ ] serverInfo nằm trong `result._meta`.
- [ ] Chỉ recognized/supported notifications trả 202; unknown id-less method không auto-success.
- [ ] exact HTTP gate statuses đúng.
- [ ] Accept parser có compatibility default và optional strict mode.
- [ ] `CONFIG_MCP_STRICT_ACCEPT_HEADER` default off nếu option được giữ.
- [ ] Content-Type parser strict.
- [ ] không dùng project meanings mới trong `-32000..-32019`.
- [ ] operational tool failures dùng `isError:true`.
- [ ] JSON dispatcher result tạo non-empty text fallback.
- [ ] MCP 2025 `structuredContent` chỉ emit khi JSON root là object.
- [ ] GET/DELETE 405 trả `Allow: POST`.

### Cleanup

- [ ] Không còn `/mcp` response `{success,status,message,data}`.
- [ ] Không còn `tool_names` trên MCP wire.
- [ ] Không còn `list_tools` / `call_tool` alias.
- [ ] Không còn `MCP_LEGACY_MODE`.
- [ ] Không còn NVS `legacy` quyết định protocol mode.
- [ ] RPC error `data` ownership được định nghĩa và test OOM/cleanup.
- [ ] README đồng bộ code/Kconfig.

### Tests

- [ ] 2025 suite pass, bao gồm counter-offer và object-only structuredContent.
- [ ] 2026 suite pass, bao gồm no-fallback và `-32022 error.data`.
- [ ] HTTP gate suite pass ở default và strict Accept modes.
- [ ] async queue-full regression pass.
- [ ] raw-name/error-data stress không leak heap.
- [ ] mixed 2025/2026 requests chạy ổn định.

# 29. Kết luận triển khai

Kiến trúc mục tiêu không phải “MCP session server”. Nó là **dual-era stateless MCP gateway endpoint**:

```text
2026-07-28
    native stateless MCP

2025-11-25
    initialize compatibility
    no session ID
    no server-side session state
```

Version 1.1 xác nhận đây là lựa chọn phù hợp với ESP32 gateway vì:

- tương thích tốt hơn với SDK/client còn dùng handshake 2025;
- giữ đúng MCP 2026 mới;
- không tốn RAM cho session manager;
- không tạo stale session sau reboot;
- không buộc HTTP connection affinity;
- không làm `mcp_tools` phụ thuộc lifecycle;
- giữ toàn bộ device execution trong `command_executor` + `command_dispatcher` hiện tại.

**Thứ tự ưu tiên implementation:**

```text
P0 conformance fixes
    -> protocol-era abstraction
    -> 2025 initialize compatibility
    -> real 2025 tool wire format
    -> HTTP GET/DELETE + Accept cleanup
    -> error policy cleanup
    -> docs/tests finalization
```

Không triển khai `mcp_session.c` trừ khi một use case tương lai thực sự yêu cầu per-client state mà không thể biểu diễn bằng explicit application handle.

---

# 30. Tài liệu tham chiếu

> Ghi chú v1.1: implementation phải kiểm tra lại các schema/transport requirements theo đúng protocol era đang triển khai; không áp dụng một rule của 2026 sang 2025 hoặc ngược lại chỉ vì cùng dùng JSON-RPC/Streamable HTTP.


MCP 2026-07-28:

- https://modelcontextprotocol.io/specification/2026-07-28/basic/index
- https://modelcontextprotocol.io/specification/2026-07-28/basic/transports
- https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/streamable-http

MCP 2025-11-25:

- https://modelcontextprotocol.io/specification/2025-11-25/basic/lifecycle
- https://modelcontextprotocol.io/specification/2025-11-25/basic/transports
- https://modelcontextprotocol.io/specification/2025-11-25/server/tools

Project source reviewed:

- `components/mcp_endpoint/mcp_endpoint.c`
- `components/mcp_endpoint/mcp_codec.c`
- `components/mcp_endpoint/mcp_rpc.c`
- `components/mcp_endpoint/mcp_tools.c`
- `components/mcp_endpoint/mcp_auth.c`
- `components/mcp_endpoint/mcp_endpoint_internal.h`
- `components/mcp_endpoint/Kconfig.projbuild`
- `components/mcp_endpoint/README.md`
- `components/mcp_endpoint/test/test_mcp_conformance.c`

