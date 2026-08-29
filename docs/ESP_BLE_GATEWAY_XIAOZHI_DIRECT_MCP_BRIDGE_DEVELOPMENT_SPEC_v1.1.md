# ESP BLE Gateway — Xiaozhi Direct MCP WebSocket Bridge Development Specification

**Document version:** 1.2  
**Date:** 2026-08-29  
**Target repository:** `hailp-vn38/esp-ble-gateway`  
**Validated baseline:** `main` around commit `9cfc0ee5ac626d9da989833d9e2ff8ebf0e73f6e`  
**Reference project:** `78/mcp-calculator`  
**Target platform:** ESP32-S3  
**Target framework:** ESP-IDF `>=6.1.0-rc1,<6.2.0`  
**Status:** Baseline implementation completed for Phase 1–4; hardware/live-endpoint validation and the missing `tools/call`/reconnect captures remain open before production sign-off

**Changes in v1.2:** freezes the observed Xiaozhi `2024-11-05` handshake, makes JSON-RPC `ping` mandatory, separates WebSocket connectivity from MCP readiness, defines async responder lifetime requirements, adds credential-at-rest policy, and closes ESP-IDF build/test/TLS integration gaps.

**Implementation snapshot (2026-08-29):**

- added transport-neutral `mcp_core` and kept HTTP `/mcp` as an adapter;
- added exact MCP `2024-11-05` negotiation, initialized notification handling, and JSON-RPC `ping`;
- added native `mcp_ws_bridge` using `esp_websocket_client`, CA bundle verification, RX assembly, generation guards, readiness state, and reconnect backoff;
- added authenticated Settings API/UI with masked endpoint display and explicit clear semantics;
- selected credential Policy B: the signed endpoint is stored plaintext in NVS, disclosed in the UI/documentation, and never returned raw by GET settings;
- added redacted Xiaozhi fixtures and MCP-core lifecycle tests;
- firmware build passes; physical-device test execution and live Xiaozhi `tools/call` validation are still required.

---

# 1. Mục tiêu

Bổ sung khả năng để ESP BLE Gateway **chủ động kết nối trực tiếp tới external MCP WebSocket endpoint của Xiaozhi**, theo mô hình transport pipe tương tự `78/mcp-calculator`, nhưng chạy hoàn toàn native trong firmware gateway.

Kiến trúc mục tiêu:

```text
User voice
   |
   v
Xiaozhi device / Xiaozhi backend
   |
   | raw MCP JSON-RPC over WebSocket/WSS
   v
ESP BLE Gateway
   |
   | shared MCP core
   v
MCP tool registry / dynamic tool exposure
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
BLE devices
```

Không sử dụng:

- PC trung gian;
- Raspberry Pi;
- Docker;
- Python runtime trên gateway;
- `mcp_pipe.py` chạy ngoài gateway;
- `mcp_proxy` chạy ngoài gateway;
- HTTP localhost loopback từ WebSocket bridge vào `/mcp`.

Gateway phải tiếp tục hoạt động bình thường khi Xiaozhi mất kết nối.

---

# 2. Phạm vi và non-goals

## 2.1 Trong phạm vi

Feature này bao gồm:

- lưu external Xiaozhi MCP WebSocket endpoint;
- kết nối `wss://` từ gateway; `ws://` chỉ có thể bật bằng Kconfig cho môi trường development/LAN;
- reconnect/backoff;
- nhận MCP JSON-RPC qua WebSocket;
- xử lý MCP bằng cùng MCP core với HTTP `/mcp`;
- trả response JSON-RPC qua WebSocket;
- hỗ trợ async BLE command completion;
- hiển thị cấu hình/trạng thái trong Settings Web UI;
- bảo vệ secret endpoint/token nếu endpoint mang credential;
- giữ HTTP MCP và Web UI hoạt động song song.

## 2.2 Ngoài phạm vi

Không implement trong feature này:

- Xiaozhi audio protocol;
- MQTT của Xiaozhi;
- WebSocket device protocol dạng `{ "type": "mcp", "payload": ... }` của `xiaozhi-esp32` nếu external MCP endpoint không sử dụng format đó;
- MCP resources/prompts/sampling nếu gateway hiện không cần;
- WebSocket server trên gateway;
- session broker bên ngoài gateway.

---

# 3. Baseline repository hiện tại

Repo hiện đã có các foundation cần thiết:

```text
components/
├── command_dispatcher/
├── command_executor/
├── device_capabilities/
├── device_store/
├── mcp_endpoint/
├── mcp_tool_exposure/
├── memory_policy/
├── web_auth/
├── web_server/
├── wifi_provisioning/
└── ...
```

Các phần đã sẵn và **không cần viết lại**:

- BLE Central command path;
- `command_executor` async queue/worker;
- dynamic MCP tool exposure;
- tool capability reconciliation;
- Web Settings page;
- modular Web UI source tree;
- Web Settings API;
- Wi-Fi reconnect state machine;
- HTTP MCP endpoint;
- MCP `tools/list` / `tools/call` business logic.

Do đó task mới chủ yếu là:

```text
transport decoupling
+
protocol compatibility
+
WebSocket client bridge
+
Settings integration
```

---

# 4. Reference behavior từ `78/mcp-calculator`

`mcp_pipe.py` về bản chất là transport pipe:

```text
WebSocket RX
   |
   v
stdin của MCP server

stdout của MCP server
   |
   v
WebSocket TX
```

Nó không phải MCP business layer.

Gateway đã có MCP business/tool layer native, nên không port subprocess/stdin/stdout pattern.

Chỉ port các ý tưởng:

- chủ động WebSocket connect;
- raw message forwarding semantics;
- reconnect;
- connection lifecycle;
- error isolation.

Target architecture trên embedded:

```text
Xiaozhi WebSocket
      <-
    raw JSON-RPC
      ->
mcp_ws_bridge
      <->
mcp_core
```

---

# 5. P0 — Phase 0 bắt buộc: Xiaozhi External MCP Wire Contract Capture

Đây là gate bắt buộc trước khi implement protocol adapter.

## 5.1 Lý do

Gateway hiện có compatibility `initialize`, nhưng compatibility đó đang được implement theo MCP era riêng của gateway.

Không được suy luận:

```text
có method initialize
=> chắc chắn compatible Xiaozhi
```

Reference Python MCP SDK `v1.20.0` hỗ trợ các protocol version:

```text
2024-11-05
2025-03-26
2025-06-18
```

Trong khi gateway hiện có compatibility path hướng `2025-11-25` và modern path `2026-07-28`.

Nếu Xiaozhi endpoint/client không chấp nhận server counter-offer `2025-11-25`, handshake sẽ fail dù JSON-RPC shape giống nhau.

## 5.2 Wire contract đã quan sát ngày 2026-08-29

Kiểm tra trực tiếp external endpoint thật đã xác nhận:

```text
transport              raw JSON-RPC trong WebSocket TEXT frame
WebSocket upgrade      HTTP 101 Switching Protocols
WebSocket subprotocol  không có
authentication         signed token trong query string
MCP client             xz-mcp-broker/0.0.1
requested version      2024-11-05
request ID             JSON number
```

Chuỗi message quan sát được:

```text
WS connected
  -> initialize id=0, protocolVersion=2024-11-05
  <- initialize result, protocolVersion=2024-11-05
  -> notifications/initialized, không có params
  -> tools/list id=1, params={}
  <- tools/list result
  -> ping id=2, params={}
  <- expected gateway response: ping result={}
```

Initialize request thực tế:

```json
{
  "id": 0,
  "jsonrpc": "2.0",
  "method": "initialize",
  "params": {
    "protocolVersion": "2024-11-05",
    "capabilities": {
      "sampling": {},
      "roots": {
        "listChanged": false
      }
    },
    "clientInfo": {
      "name": "xz-mcp-broker",
      "version": "0.0.1"
    }
  }
}
```

Các kết luận đã freeze:

- Gateway phải support và ưu tiên trả đúng requested version `2024-11-05`; không được giả định current counter-offer `2025-11-25` sẽ được broker chấp nhận nếu chưa có fixture chứng minh.
- `notifications/initialized` không có `params` và không được nhận response frame.
- `tools/list` có `params: {}`.
- `ping` là JSON-RPC request bắt buộc của Xiaozhi profile, không phải WebSocket control ping.
- Signed URL là credential và không được ghi nguyên văn vào fixture, tài liệu, log hoặc GET API.
- TLS certificate public của endpoint hợp lệ với hostname và public CA bundle; firmware vẫn phải bật certificate verification rõ ràng.

Phase 0 vẫn chưa hoàn tất cho đến khi capture được `tools/call`, reconnect/close behavior và các message phụ khác nếu xuất hiện.

## 5.3 Capture còn cần thực hiện

Chạy reference `mcp_pipe.py` với external Xiaozhi MCP endpoint thật và capture raw TEXT frames.

Cần freeze tối thiểu các fixture:

### Initialize request/response

```json
{
  "jsonrpc": "2.0",
  "id": 0,
  "method": "initialize",
  "params": {
    "protocolVersion": "2024-11-05",
    "capabilities": {
      "sampling": {},
      "roots": {
        "listChanged": false
      }
    },
    "clientInfo": {
      "name": "xz-mcp-broker",
      "version": "0.0.1"
    }
  }
}
```

Các field trên đã được quan sát. Fixture vẫn phải được freeze và response của gateway/reference server phải được xác minh:

- `protocolVersion`;
- `clientInfo` có bắt buộc không;
- `clientInfo` fields;
- `capabilities` shape;
- request ID type.

### Initialized notification

```json
{
  "jsonrpc": "2.0",
  "method": "notifications/initialized"
}
```

Đã xác nhận notification không có `params`.

### tools/list

Request quan sát được:

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "tools/list",
  "params": {}
}
```

Đã xác nhận ID là number và không có protocol metadata trong body. Còn phải xác minh pagination behavior nếu tool catalog yêu cầu cursor.

### tools/call

Xác minh:

```json
{
  "method": "tools/call",
  "params": {
    "name": "...",
    "arguments": {}
  }
}
```

### Các message phụ

`ping` đã được quan sát và trở thành fixture bắt buộc. Capture thêm nếu có:

- cancellation;
- progress;
- notifications;
- WS ping/pong;
- close code/reason.

### JSON-RPC ping

Request quan sát được:

```json
{
  "jsonrpc": "2.0",
  "id": 2,
  "method": "ping",
  "params": {}
}
```

Response bắt buộc:

```json
{
  "jsonrpc": "2.0",
  "id": 2,
  "result": {}
}
```

Không map request này thành WebSocket PONG và không trả `Method not found`.

## 5.4 Output của Phase 0

Tạo trong repo:

```text
test/fixtures/xiaozhi_mcp/
├── initialize_request.json
├── initialize_response.json
├── initialized_notification.json
├── tools_list_request.json
├── ping_request.json
├── ping_response.json
└── README.md
```

Các fixture trên đã được thêm và không chứa token thật. Ba fixture sau vẫn
pending cho đến khi có capture thật, không được tự tạo shape suy đoán:

```text
tools_list_response.json
tools_call_request.json
tools_call_response.json
```

`README.md` phải ghi:

```text
capture date
endpoint type
observed MCP protocolVersion
observed client implementation/version
transport framing
known authentication method
known reconnect behavior
observed ping behavior
close code/reason if observed
```

Fixture và README không được chứa endpoint query/token thật. Dùng placeholder như `token=<redacted>`.

## 5.5 Exit criteria Phase 0

Phase 1 transport decoupling được phép bắt đầu từ các facts đã freeze. Không coi Phase 2/3 hoàn tất hoặc production-ready nếu chưa biết chính xác:

- wire framing;
- handshake sequence;
- negotiated protocol version;
- tool request/response shape;
- JSON-RPC ping behavior;
- endpoint credential handling.

---

# 6. Quyết định kiến trúc chính

## 6.1 Xiaozhi là transport integration, không phải tool subsystem

Không tạo:

```text
xiaozhi_tools
xiaozhi_registry
xiaozhi_dispatcher
xiaozhi_device_policy
```

HTTP MCP và Xiaozhi WS dùng chung:

```text
mcp_registry
mcp_tools
mcp_policy
mcp_tool_exposure
command_executor
command_dispatcher
```

## 6.2 Gateway vẫn là MCP server

Gateway chủ động mở socket, nhưng MCP role vẫn là:

```text
Xiaozhi side = MCP client
Gateway      = MCP server
```

Gateway không chủ động gửi `initialize`.

Gateway phải duy trì MCP lifecycle state riêng cho từng WebSocket generation. Socket open chưa đồng nghĩa MCP đã ready.

## 6.3 Không gọi lại HTTP `/mcp` nội bộ

Không implement:

```text
WS RX
   |
HTTP POST localhost/mcp
   |
HTTP response
   |
WS TX
```

Vì sẽ:

- duplicate serialization;
- duplicate auth/gate;
- tốn socket/buffer;
- tăng latency;
- tạo dependency vòng;
- làm async lifecycle phức tạp hơn.

Phải gọi trực tiếp `mcp_core`.

---

# 7. Kiến trúc mục tiêu

```text
                      Xiaozhi External MCP Endpoint
                                 |
                                 | WSS
                                 v
                      +---------------------+
                      |    mcp_ws_bridge    |
                      |---------------------|
                      | config/NVS          |
                      | WS lifecycle        |
                      | TLS                 |
                      | reconnect           |
                      | RX assembly         |
                      | TX queue            |
                      | generation          |
                      +----------+----------+
                                 |
                                 v
                         +---------------+
                         | mcp_ws_adapter|
                         +-------+-------+
                                 |
                                 v
+------------------+     +------------------+
| mcp_http_adapter |---->|     mcp_core     |
+--------+---------+     +--------+---------+
         ^                        |
         |                        v
     POST /mcp                mcp_tools
                                  |
                           mcp_tool_exposure
                                  |
                           command_executor
                                  |
                         command_dispatcher
                                  |
                                 BLE
```

`mcp_core` không được biết:

```text
httpd_req_t
esp_websocket_client_handle_t
HTTP header APIs
HTTP status code APIs
NVS
Xiaozhi URL
TLS socket lifecycle
```

---

# 8. Refactor `mcp_endpoint`: từ HTTP-bound sang transport-neutral MCP core

## 8.1 Vấn đề hiện tại

Current MCP path sử dụng global transport hooks:

```c
static mcp_transport_t s_transport;
```

với các operation phụ thuộc `httpd_req_t *`:

```text
recv
send
send_err
set_type
set_status
set_hdr
get_header
async_begin
async_complete
```

Đây là test seam hợp lý cho HTTP, nhưng không an toàn khi HTTP và WS cùng hoạt động.

## 8.2 Không được switch global transport runtime

Không làm:

```text
HTTP request -> mcp_transport_set(http)
WS request   -> mcp_transport_set(ws)
```

Async BLE command có thể complete sau khi global transport đã đổi.

Đây là race condition P0.

## 8.3 Target decomposition

Đề xuất:

```text
components/mcp_endpoint/
├── include/
│   ├── mcp_endpoint.h
│   ├── mcp_core.h               NEW
│   ├── mcp_request.h            NEW optional
│   └── mcp_responder.h          NEW optional
├── mcp_endpoint.c               HTTP route registration only
├── mcp_http_adapter.c           NEW
├── mcp_core.c                   NEW
├── mcp_protocol.c
├── mcp_rpc.c
├── mcp_tools.c
├── mcp_registry.c
├── mcp_auth.c                   HTTP-facing auth/gate
├── mcp_codec.c
└── mcp_policy.c
```

Nếu muốn boundary sạch hơn có thể tách `mcp_core` thành component riêng sau Phase 1, nhưng không bắt buộc trong patch đầu.

---

# 9. Request abstraction

## 9.1 Normalized request

Đề xuất:

```c
typedef enum {
    MCP_TRANSPORT_HTTP = 0,
    MCP_TRANSPORT_WS,
} mcp_transport_kind_t;

typedef struct {
    mcp_transport_kind_t transport;

    bool has_protocol_version;
    char protocol_version[32];

    bool has_method_metadata;
    char method_metadata[64];

    bool has_name_metadata;
    char name_metadata[128];

    bool authenticated;
    bool trusted_transport;
} mcp_wire_context_t;
```

HTTP adapter populate từ HTTP headers/body.

WS adapter populate chỉ từ data thực sự tồn tại trên WS contract.

Không giả lập HTTP header cho WebSocket nếu Xiaozhi không gửi thông tin tương đương.

---

# 10. Protocol detection refactor

## 10.1 Vấn đề hiện tại

`mcp_protocol_detect()` hiện nhận `httpd_req_t *` và đọc:

```text
MCP-Protocol-Version
Mcp-Method
Mcp-Name
```

Điều này khiến protocol detection phụ thuộc HTTP.

## 10.2 API mới

Đề xuất:

```c
int mcp_protocol_detect(
    const cJSON *root,
    const mcp_wire_context_t *wire,
    mcp_request_context_t *ctx,
    mcp_rpc_error_detail_t *error);
```

HTTP adapter chịu trách nhiệm extract headers.

WS adapter không truyền metadata giả.

## 10.3 Protocol era trên WS

Không tự động support MCP `2026-07-28` qua WS nếu era này phụ thuộc các HTTP-specific metadata mà Xiaozhi không cung cấp.

WS support matrix phải được quyết định từ Phase 0.

Ví dụ target có thể là:

```text
HTTP:
- MCP 2026-07-28
- MCP 2025-11-25 compatibility

Xiaozhi WS:
- MCP 2025-06-18   if observed
- MCP 2025-03-26   if observed
- MCP 2024-11-05   if observed
```

Không cần expose mọi era trên mọi transport.

---

# 11. Protocol compatibility adapter

## 11.1 Không hard-code `2025-11-25` cho mọi initialize

Current compatibility behavior counter-offer `2025-11-25`.

Sau Phase 0, implement explicit version policy.

Ví dụ:

```c
typedef enum {
    MCP_COMPAT_2024_11_05,
    MCP_COMPAT_2025_03_26,
    MCP_COMPAT_2025_06_18,
    MCP_COMPAT_2025_11_25,
    MCP_PRIMARY_2026_07_28,
} mcp_protocol_era_t;
```

Không nhất thiết phải expose enum public.

## 11.2 Negotiation rule

Server chỉ trả version mà client được biết là hỗ trợ.

Không trả một version cao hơn chỉ vì server hỗ trợ.

Rule:

```text
client requested version
       |
       v
is directly supported?
  yes -> use it
  no  -> choose documented compatible version only if spec allows
  else -> unsupported protocol error
```

## 11.3 Shared tool semantics

Dù protocol era khác nhau:

```text
initialize formatting differs
result formatting may differ
```

nhưng tool resolution vẫn gọi cùng:

```text
mcp_tools_list
mcp_tools_resolve
mcp_tools_execute / command_executor
```

## 11.4 Xiaozhi JSON-RPC ping

Xiaozhi broker đã được quan sát gửi:

```json
{"jsonrpc":"2.0","id":2,"method":"ping","params":{}}
```

`mcp_core` phải xử lý `ping` đồng bộ và trả:

```json
{"jsonrpc":"2.0","id":2,"result":{}}
```

Rules:

- preserve nguyên JSON-RPC ID type/value;
- cho phép `params` absent hoặc empty object nếu protocol fixture chứng minh cả hai;
- không route qua tool registry hoặc command executor;
- không trả `Method not found`;
- không nhầm với WebSocket control ping/pong do client library quản lý.

---

# 12. Response abstraction: per-request responder

## 12.1 Không dùng global response transport

Mỗi request phải sở hữu responder riêng.

Đề xuất:

```c
typedef struct mcp_responder mcp_responder_t;

typedef esp_err_t (*mcp_send_json_fn)(
    void *context,
    const char *json,
    size_t len);

typedef bool (*mcp_is_alive_fn)(void *context);

typedef void (*mcp_release_fn)(void *context);

struct mcp_responder {
    void *context;
    mcp_send_json_fn send_json;
    mcp_is_alive_fn is_alive;
    mcp_release_fn release;
};
```

Có thể thêm transport-specific metadata mapping bên adapter thay vì core.

## 12.2 Ownership và lifetime bắt buộc

Responder phải có contract ownership rõ ràng; generation check một mình không ngăn use-after-free.

Chọn một trong hai implementation hợp lệ:

```text
Option 1
responder context có retain/release hoặc reference count atomic

Option 2
bridge là singleton có lifetime bằng firmware process;
reload chỉ thay connection-owned state dưới mutex, không free bridge object
```

Rules bắt buộc:

- `mcp_core_handle_json()` phải ghi rõ nó borrow hay retain responder.
- Khi async command được submit, async context phải sở hữu một reference độc lập.
- Mọi success/error/drop/cancel path phải release đúng một lần.
- `stop`, `reload` và disable không được free object còn được BLE completion callback tham chiếu.
- Destroy connection phải invalidate generation trước khi giải phóng connection-owned resources.
- Nếu implementation destroy bridge object, `stop()` phải drain/cancel pending async contexts trước destroy.
- `is_alive()` và generation read phải synchronization-safe; không đọc connection handle đã free.

Test bắt buộc:

```text
submit BLE command
  -> disconnect
  -> reload/disable bridge
  -> BLE callback completes
expected: no send, no UAF, no double release, no leak
```

## 12.3 Reply semantic

Không dùng abstraction `send_empty()` chung.

Core phải biểu diễn:

```c
typedef enum {
    MCP_REPLY_NONE = 0,
    MCP_REPLY_JSON,
} mcp_reply_kind_t;
```

Mapping:

```text
notification over HTTP
    MCP_REPLY_NONE
    -> HTTP 202 empty body

notification over WS
    MCP_REPLY_NONE
    -> send nothing
```

Không gửi zero-length WebSocket frame để đại diện notification response.

---

# 13. MCP core API đề xuất

Có hai lựa chọn implementation.

## 13.1 Option A — Core tự send qua responder

```c
esp_err_t mcp_core_handle_json(
    const char *json,
    size_t json_len,
    const mcp_wire_context_t *wire,
    const mcp_responder_t *responder);
```

Ưu điểm:

- patch nhỏ hơn;
- phù hợp async command completion.

## 13.2 Option B — Core trả structured outcome

```c
typedef struct {
    mcp_reply_kind_t kind;
    char *json;
    size_t json_len;
} mcp_core_result_t;
```

Async path vẫn cần responder/context riêng.

## 13.3 Khuyến nghị

Dùng **Option A** cho v1.1 implementation vì current `tools/call` đã có async callback lifecycle.

---

# 14. Async command context

Current HTTP path giữ `httpd_req_t *` trong async command context.

Target:

```c
typedef struct {
    cJSON *id;
    mcp_request_context_t protocol;
    mcp_responder_t responder;
    bool notification;

    uint32_t connection_generation;
} mcp_async_context_t;
```

HTTP responder:

```text
context -> async httpd_req_t
```

WS responder:

```text
context -> retained bridge/connection responder + generation
```

Chỉ copy struct là chưa đủ; context reference phải được retain theo Section 12.2.

---

# 15. Connection generation — bắt buộc

Case cần bảo vệ:

```text
WS generation 10
       |
       | tools/call
       v
BLE pending
       |
WS disconnect
       |
WS reconnect -> generation 11
       |
BLE generation 10 completes
```

Expected:

```text
DROP old response
```

Không gửi response request cũ vào socket mới.

Implementation:

```text
on disconnect or explicit stop/reload:
    invalidate current generation before connection resources are released

on successful WS connect:
    generation++

on request:
    async_ctx.generation = current_generation

on completion:
    if !responder.is_alive() or
       async_ctx.generation != current_generation:
        drop safely
```

Generation dùng `uint32_t` là đủ nếu wrap được so sánh equality.

---

# 16. Component mới: `mcp_ws_bridge`

Tên đề xuất:

```text
components/mcp_ws_bridge/
```

Thay vì `xiaozhi_mcp`, vì component thực tế là generic MCP-over-WebSocket client transport.

UI vẫn có thể gọi section là:

```text
Xiaozhi Integration
```

Cấu trúc:

```text
components/mcp_ws_bridge/
├── include/
│   └── mcp_ws_bridge.h
├── mcp_ws_bridge.c
├── mcp_ws_client.c
├── mcp_ws_config.c
├── mcp_ws_rx.c              optional split
├── mcp_ws_tx.c              optional split
├── CMakeLists.txt
├── Kconfig.projbuild
└── test/
    ├── test_mcp_ws_config.c
    ├── test_mcp_ws_state.c
    ├── test_mcp_ws_generation.c
    ├── test_mcp_ws_frame.c
    └── test_mcp_ws_reconnect.c
```

Không cần split quá nhỏ ngay từ đầu; giữ mỗi `.c` có responsibility rõ ràng.

---

# 17. Public API `mcp_ws_bridge`

```c
typedef enum {
    MCP_WS_DISABLED = 0,
    MCP_WS_WAIT_NETWORK,
    MCP_WS_CONNECTING,
    MCP_WS_HANDSHAKING,
    MCP_WS_READY,
    MCP_WS_BACKOFF,
    MCP_WS_ERROR,
} mcp_ws_state_t;

typedef struct {
    bool enabled;
    bool endpoint_configured;
    mcp_ws_state_t state;
    uint32_t generation;
    uint32_t retry_count;
    int last_error;
    int last_http_status;
    int last_ws_close_code;
    char negotiated_protocol_version[16];
} mcp_ws_status_t;

esp_err_t mcp_ws_bridge_init(void);
esp_err_t mcp_ws_bridge_start(void);
esp_err_t mcp_ws_bridge_stop(void);
esp_err_t mcp_ws_bridge_reload(void);

esp_err_t mcp_ws_bridge_get_status(mcp_ws_status_t *out);
```

Config setter/getter có thể public cho Web Settings service:

```c
esp_err_t mcp_ws_bridge_config_set(const mcp_ws_config_t *config);
esp_err_t mcp_ws_bridge_config_get_public(mcp_ws_public_config_t *out);
```

Không public raw secret getter nếu không cần.

Không lưu raw TLS/library error string nếu string đó có thể chứa full URI. Adapter phải map sang sanitized code/message trước khi expose hoặc log.

---

# 18. WebSocket client dependency

Repo hiện chưa có WebSocket client dependency.

Đề xuất thêm managed component:

```yaml
espressif/esp_websocket_client: "~1.8.0"
```

Target IDF hiện là:

```yaml
idf: ">=6.1.0-rc1,<6.2.0"
```

`esp_websocket_client 1.8.0` hỗ trợ IDF `>=5.0`, phù hợp baseline.

Sau khi thêm dependency phải cập nhật lock file bằng ESP-IDF Component Manager.

Managed component này chỉ compile source/header khi WebSocket transport được bật. Thêm vào defaults:

```text
CONFIG_WS_TRANSPORT=y
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL=y
```

Không nhầm `CONFIG_WS_TRANSPORT` của outbound client với `CONFIG_HTTPD_WS_SUPPORT` của inbound HTTP server.

Không copy source library vào repo nếu không có lý do đặc biệt.

---

# 19. `CONFIG_HTTPD_WS_SUPPORT` giữ nguyên OFF

Current production config có:

```text
CONFIG_HTTPD_WS_SUPPORT=n
```

Giữ nguyên.

Feature này cần WebSocket **client**:

```text
Gateway -> Xiaozhi
```

không cần HTTPD WebSocket server.

Không bật server feature vô ích chỉ vì dùng WebSocket client.

---

# 20. WebSocket connection state machine

```text
DISABLED
   |
   | enabled + endpoint configured
   v
WAIT_NETWORK
   |
   | STA got IP
   v
CONNECTING
   |
   +------ WS success ----> MCP_HANDSHAKING
   |                              |
   |                              | initialize response sent
   |                              | + notifications/initialized received
   |                              v
   |                            READY
   |                              |
   |                              | disconnect/error/handshake timeout
   |                              v
   +------ failure -----------> BACKOFF
                                   |
                                   | timer
                                   v
                               CONNECTING
```

`READY` mới được hiển thị là `Connected` trong Web Settings. `MCP_HANDSHAKING` phải hiển thị là `Handshaking`.

Mỗi generation phải có protocol session state riêng:

```text
initialize_received
initialize_response_sent
initialized_notification_received
negotiated_protocol_version
handshake_deadline
```

Rules:

- Gateway không chủ động gửi `initialize`.
- Handshake timeout phải đưa connection vào close/backoff, không treo vĩnh viễn.
- Reset toàn bộ MCP session state khi disconnect/reconnect.
- Không nhận `tools/call` như một ready session trước khi lifecycle hoàn tất.
- Duplicate `initialize` trong cùng generation phải được xử lý deterministically theo protocol policy, không tạo session thứ hai.
- `ping` phải được trả lời khi connection còn alive; policy trước/sau READY phải được fixture test xác nhận.

Nếu Wi-Fi mất:

```text
READY / MCP_HANDSHAKING / CONNECTING / BACKOFF
       |
       v
close WS
cancel reconnect timer
WAIT_NETWORK
```

Không busy-loop reconnect khi Wi-Fi chưa có IP.

---

# 21. Wi-Fi event integration

`wifi_provisioning` hiện đã quản lý Wi-Fi reconnect.

`mcp_ws_bridge` không được gọi `esp_wifi_connect()` hoặc tự quản Wi-Fi policy.

Bridge chỉ subscribe:

```text
IP_EVENT_STA_GOT_IP
WIFI_EVENT_STA_DISCONNECTED
```

Behavior:

```text
GOT_IP:
  if enabled -> connect/reconnect WS

STA_DISCONNECTED:
  close WS
  state = WAIT_NETWORK
```

Không poll `wifi_prov_is_connected()` liên tục bằng task loop nếu event-driven path đã khả dụng.

---

# 22. Reconnect policy

Application-owned reconnect policy:

```text
1 s
2 s
4 s
8 s
16 s
30 s
60 s
```

Cap:

```text
60 s
```

Jitter:

```text
±10–20%
```

Backoff reset sau connection ổn định đủ lâu, ví dụ 30 s.

Nếu tự implement backoff trong application thì **disable internal auto reconnect** của `esp_websocket_client` để tránh double reconnect loops.

---

# 23. WebSocket RX framing

Phase đầu hỗ trợ:

```text
TEXT frames only
```

Binary frame:

```text
ignore/reject + warning
```

Phải support fragmented message nếu client library deliver chunked events.

RX assembler phải track:

```text
message total length
current offset
opcode
FIN/message completion
```

Không parse JSON trước khi complete message.

Với `esp_websocket_client`, implementation phải dùng đúng event metadata của version đã lock:

```text
data_len
payload_len
payload_offset
op_code
fin
```

Một WebSocket message có thể tạo nhiều `WEBSOCKET_EVENT_DATA` khi lớn hơn library buffer. Control frame có thể xuất hiện xen kẽ và không được làm hỏng TEXT assembler. Continuation frame phải được gắn đúng message đang active.

Reject protocol-invalid cases deterministically:

- offset không liên tục;
- declared payload vượt max trước khi allocate;
- TEXT message mới khi assembler cũ chưa complete;
- invalid UTF-8 nếu JSON parser/library không đảm bảo;
- mixed binary/continuation không hợp lệ.

Sau oversize/protocol error, reset assembler về clean state; không để bytes còn lại bị parse thành JSON message mới.

---

# 24. Message size policy

Không hard-code `8 KiB` không benchmark.

Tách:

```text
CONFIG_MCP_WS_MAX_RX_MESSAGE
CONFIG_MCP_WS_MAX_TX_MESSAGE
```

Initial values chỉ được freeze sau measurement.

Recommended starting test range:

```text
RX: 4–8 KiB
TX: 8–32 KiB
```

Nhưng acceptance phải dựa trên worst-case `tools/list` với target device count.

`tools/list` là payload dễ vượt limit nhất vì chứa:

```text
static tools
+
dynamic device tools
+
descriptions
+
input schemas
```

Benchmark với ít nhất:

```text
9 BLE links
maximum enabled dynamic tools configured for production
```

---

# 25. TX ownership

Không để arbitrary command worker gọi trực tiếp:

```c
esp_websocket_client_send_text(...)
```

Đề xuất:

```text
MCP core / async completion
       |
       v
mcp_ws_tx_queue
       |
       v
single bridge TX owner
       |
       v
esp_websocket_client_send_text
```

Queue item nên chứa:

```c
typedef struct {
    uint32_t generation;
    char *payload;
    size_t payload_len;
} mcp_ws_tx_item_t;
```

Trước send:

```text
item.generation == current_generation ? send : drop
```

---

# 26. WebSocket callback rule

Event callback chỉ:

```text
capture event
copy minimal metadata/data
queue work
return
```

Không:

- parse large JSON sâu trong callback;
- gọi `command_executor` nếu callback context không phù hợp;
- block chờ BLE;
- sleep/backoff;
- write NVS;
- gọi Web Settings logic.

---

# 27. Settings integration với Web architecture hiện tại

Repo hiện đã có modular Web UI:

```text
components/web_server/www_src/dashboard/
├── shell.html
├── js/
├── partials/
└── views/
    └── settings.html
```

Vì vậy không tạo page mới ngoài architecture này.

## 27.1 UI placement

Trong Settings hiện có, thêm section:

```text
Xiaozhi Integration

[ ] Enable direct MCP connection

MCP WebSocket Endpoint
[wss://.................................]

Status: Connected
Last error: -
```

Không tạo sidebar/tab MCP riêng.

## 27.2 Baseline fields

Phase đầu chỉ cần:

```text
enabled
endpoint
```

Không thêm `token` hoặc `client_id` trừ khi Phase 0 chứng minh external endpoint cần custom header riêng.

Nếu credential nằm trong endpoint URL thì endpoint itself là secret.

---

# 28. Settings API integration

Repo hiện đã có:

```http
GET /api/settings
```

Không cần tạo status endpoint riêng.

Mở rộng response:

```json
{
  "success": true,
  "system": {},
  "network": {},
  "auth": {},
  "mcp": {},
  "xiaozhi": {
    "enabled": true,
    "endpoint_configured": true,
    "endpoint_display": "wss://example/path?...****",
    "state": "connected",
    "retry_count": 0,
    "last_error": 0,
    "last_http_status": 0,
    "last_ws_close_code": 0,
    "protocol_version": "2024-11-05"
  }
}
```

Mutation API đề xuất một route duy nhất:

```http
PUT /api/settings/xiaozhi
```

Body:

```json
{
  "enabled": true,
  "endpoint": "wss://..."
}
```

Không tạo nhiều route connect/disconnect/status nếu không cần.

---

# 29. Route budget

Gateway HTTP server hiện có route budget hữu hạn.

Current accounting theo source comment:

```text
gateway Web/API routes     29
MCP GET/POST/DELETE         3
current total              32
configured maximum         34
Xiaozhi PUT addition       +1
target total               33
remaining headroom          1
```

Implementation phải kiểm tra lại count từ registration code tại thời điểm merge vì source comment có thể drift.

Do đó feature Xiaozhi nên tiêu thụ tối đa:

```text
+1 route
```

Preferred:

```text
GET /api/settings           existing
PUT /api/settings/xiaozhi   new
```

Status xem qua GET settings.

Connect/disconnect runtime được derive từ `enabled + endpoint` và reload action, không cần explicit REST endpoint.

---

# 30. NVS ownership

Không để `web_settings_api.c` trực tiếp mở namespace Xiaozhi.

Đúng:

```text
web_settings_api
      |
      v
mcp_ws_bridge_config_set()
      |
      v
mcp_ws_config owns NVS
```

Sai:

```text
web_settings_api
      |
      v
nvs_open("xiaozhi")
```

Namespace đề xuất:

```text
mcp_ws
```

Keys:

```text
enabled
endpoint
```

Nếu Phase 0 cần auth header:

```text
auth_mode
auth_secret
```

Không lưu runtime state vào NVS.

---

# 31. Secret handling

External endpoint có thể mang signed token trong query:

```text
wss://host/path?token=SECRET
```

Vì vậy endpoint cần được coi là credential nếu có query/embedded secret.

## 31.1 GET API

Không trả raw endpoint nếu nó chứa secret.

Trả:

```json
{
  "endpoint_configured": true,
  "endpoint_display": "wss://host/path?...****"
}
```

## 31.2 Logs

Không log:

```text
full endpoint query
Authorization header
raw token
```

Log sanitized:

```text
Connecting Xiaozhi MCP endpoint host=example.com path=/mcp
```

`esp_websocket_client` upstream có các failure/redirect log chứa full URI. Vì
signed query là secret, implementation baseline tắt log tag
`websocket_client` và giữ error/status đã sanitize trong bridge/UI. Không được
bật lại tag này ở production nếu chưa patch/redact upstream log paths.

## 31.3 Update semantics

Nếu PUT body không chứa endpoint:

```text
keep current endpoint
```

Nếu explicit clear action được hỗ trợ, dùng field riêng hoặc endpoint `null`, không dùng ambiguous empty string nếu UI dễ gửi nhầm.

Config validation trước khi commit NVS:

- enforce configured maximum URL length;
- chỉ chấp nhận `wss://`, hoặc `ws://` khi dev/LAN policy cho phép;
- reject missing host, URI có control character và malformed percent encoding;
- không tự thêm token vào URL;
- parse/sanitize log bằng code riêng, không log raw URI khi parser lỗi;
- validate/copy new config hoàn tất trước khi thay config đang hoạt động;
- nếu reconnect với config mới fail, giữ config đã save nhưng expose sanitized error rõ ràng;
- redirect policy phải được explicit; không forward credential query sang host khác một cách im lặng.

## 31.4 Bảo vệ secret at rest và threat model

Mask GET API và logs không đồng nghĩa credential đã được bảo vệ at rest.

Baseline repository hiện không bật:

```text
CONFIG_NVS_ENCRYPTION
CONFIG_SECURE_FLASH_ENC_ENABLED
CONFIG_SECURE_BOOT
```

Vì vậy phải chọn và document rõ một trong hai product policy:

### Policy A — production confidentiality at rest

- enable flash encryption/NVS encryption theo ESP-IDF production provisioning flow;
- document key provisioning, recovery và OTA implications;
- acceptance test đọc raw flash/NVS không thấy plaintext endpoint/token.

### Policy B — physical extraction ngoài threat model

- ghi rõ signed endpoint được lưu plaintext trong NVS;
- không tuyên bố credential được bảo vệ at rest;
- UI cảnh báo người vận hành rằng credential có thể bị lấy khi có physical flash access.

Dù dùng policy nào:

- không hard-code endpoint/token vào source, defaults, fixtures hoặc committed assets;
- không đưa full URI vào assertion, crash dump hay error string của library;
- sanitize trước khi log, bao gồm failure path của URI parser;
- zeroize/free temporary request buffers chứa endpoint sau config update khi khả thi;
- không trả raw endpoint về browser sau reload;
- production token phải có rotation/revocation procedure.

Token dùng cho capture/review phải được rotate sau khi hoàn tất vì đã được truyền qua kênh làm việc bên ngoài thiết bị.

---

# 32. TLS/WSS policy

Production ưu tiên:

```text
wss://
```

`ws://` chỉ cho LAN/dev nếu user chủ động cấu hình.

Production không default:

```text
skip_cert_verify
```

Dùng ESP-IDF certificate bundle/trust configuration phù hợp.

Baseline implementation cho public Xiaozhi endpoint:

```c
esp_websocket_client_config_t config = {
    .uri = sanitized_internal_uri,
    .crt_bundle_attach = esp_crt_bundle_attach,
    .disable_auto_reconnect = true,
};
```

Tên field chính xác phải được xác nhận với header của `esp_websocket_client` version đã lock.

Defaults phải giữ certificate bundle được bật để clean build tái lập được; không chỉ dựa vào generated `sdkconfig`.

Nếu endpoint dùng private CA, đó là feature riêng; không silently disable verification.

---

# 33. HTTP MCP auth vs Xiaozhi endpoint auth

Hai credential domain độc lập.

HTTP `/mcp` token hiện bảo vệ inbound MCP HTTP access.

Xiaozhi endpoint credential nếu có là outbound credential.

Không reuse mặc định:

```text
MCP HTTP bearer token
```

làm:

```text
Xiaozhi WS auth token
```

trừ khi user explicitly cấu hình cùng giá trị.

---

# 34. Tool exposure consistency

Một source of truth duy nhất:

```text
mcp_tool_exposure
+
mcp_tool_catalog
```

Acceptance:

```text
Enable tool X in Device Detail
   |
   +-> HTTP tools/list contains X
   |
   +-> Xiaozhi WS tools/list contains X
```

Sau disable:

```text
HTTP -> X absent
WS   -> X absent
```

Không có cache riêng ở `mcp_ws_bridge` trong phase đầu.

---

# 35. tools/call execution flow

```text
Xiaozhi WS RX
      |
      v
mcp_ws_adapter
      |
      v
mcp_core
      |
      v
mcp_tools_resolve
      |
      +-- gateway command -> sync execution path
      |
      +-- device command
              |
              v
      command_executor_submit
              |
              v
      command_dispatcher
              |
              v
             BLE
              |
              v
      completion callback
              |
              v
      format MCP result/error
              |
              v
       mcp_ws_tx_queue
              |
              v
      generation check
              |
              v
          WebSocket TX
```

Không bypass `command_executor` cho BLE device command.

---

# 36. Failure isolation

Bắt buộc:

```text
Xiaozhi unavailable
      |
      +--> Web UI PASS
      +--> HTTP /mcp PASS
      +--> BLE PASS
      +--> Device Detail PASS
      +--> local command execution PASS
      +--> bridge state BACKOFF/WAIT_NETWORK
```

`mcp_ws_bridge` không được trở thành boot-critical dependency.

Startup failure:

```text
log warning
continue gateway startup
```

---

# 37. Startup integration

Current boot sequence đã chờ Wi-Fi connected trước khi gateway services start.

Đề xuất:

```text
NVS
 |
Wi-Fi provisioning init
 |
Wi-Fi connected
 |
device_store
 |
device_capabilities
 |
mcp_tool_exposure
 |
command_dispatcher
 |
command_executor
 |
BLE Central
 |
Web server + HTTP MCP
 |
mcp_ws_bridge_init
 |
mcp_ws_bridge_start
```

Nếu `mcp_ws_bridge_init/start` lỗi:

```text
ESP_LOGW
```

không `return` khỏi `app_main()`.

---

# 38. Memory policy

Gateway chạy đồng thời:

```text
BLE Central
Web UI HTTP server
HTTP MCP
Xiaozhi WebSocket
command workers
MCP exposure worker
Wi-Fi
```

Do đó bridge phải có memory budget rõ.

## 38.1 Không duplicate tool list lâu dài

Không giữ:

```text
persistent WS copy of tools/list
```

Generate on demand từ existing catalog.

## 38.2 Buffer policy

Ưu tiên:

- control structs: internal RAM;
- queue control: internal RAM;
- large transient JSON buffer: có thể dùng heap policy hiện tại/PSRAM nếu safe;
- TLS/library internal buffers: theo component behavior.

Không ép toàn bộ bridge buffer vào PSRAM nếu library yêu cầu DMA/internal-capable memory.

## 38.3 Benchmark bắt buộc

Đo:

```text
free internal heap before WS
free internal heap after WS connected
largest free block
PSRAM free
stack high-water
```

với:

```text
9 BLE links
HTTP active
WS active
maximum tool exposure target
```

---

# 39. Concurrency rules

## 39.1 MCP core thread safety

Audit tất cả static buffers/mutex trong:

```text
mcp_tools
mcp_registry
mcp_rpc
mcp_tool_exposure
```

vì sau feature mới MCP có thể được gọi từ:

```text
HTTPD task
+
WS worker task
```

Current dispatch serialization must be preserved.

## 39.2 No global mutable transport

Global test transport hook có thể giữ cho unit test nếu chỉ compiled/test-only, nhưng production runtime path không được dùng nó để multiplex HTTP và WS.

## 39.3 Core parse concurrency

Nếu cJSON usage là per-request, có thể concurrent.

Nếu shared static result/buffer tồn tại, phải giữ mutex hoặc refactor.

---

# 40. Error behavior

## 40.1 Invalid JSON

Trả JSON-RPC parse error nếu request có thể được xử lý theo contract.

Không disconnect socket chỉ vì một malformed MCP request, trừ khi endpoint contract yêu cầu.

## 40.2 Unknown method

Trả method not found theo era.

## 40.3 Queue full

Map existing `command_executor_submit()` queue-full thành MCP tool/server error.

Không close WS.

## 40.4 Device timeout

Trả tool error/result theo existing MCP semantic.

## 40.5 Oversize frame

Không allocate vượt configured limit.

Khi `payload_len` cho biết message vượt limit:

- mark current message as discard;
- drain toàn bộ chunks/continuations thuộc message đó;
- reset assembler chỉ sau message boundary;
- log length và generation, không log payload;
- tiếp tục connection hoặc close theo policy đã test, nhưng không parse tail bytes như request mới.

## 40.6 Socket disconnect

```text
cleanup connection
invalidate generation
state BACKOFF/WAIT_NETWORK
```

Không ảnh hưởng HTTP request lifecycle.

---

# 41. Kconfig

Đề xuất:

```text
CONFIG_MCP_WS_BRIDGE=y
CONFIG_MCP_WS_MAX_RX_MESSAGE=<benchmark result>
CONFIG_MCP_WS_MAX_TX_MESSAGE=<benchmark result>
CONFIG_MCP_WS_RX_QUEUE_DEPTH=4
CONFIG_MCP_WS_TX_QUEUE_DEPTH=4
CONFIG_MCP_WS_MAX_BACKOFF_MS=60000
CONFIG_MCP_WS_HANDSHAKE_TIMEOUT_MS=10000
```

Không expose reconnect tuning lên Web UI phase đầu.

Nếu bridge disabled compile-time:

- không link component;
- Settings UI có thể hide section;
- HTTP MCP unaffected.

---

# 42. Build integration

Managed dependency có thể đặt tại root/main manifest hoặc component manifest theo convention repo.

Vì cả firmware root và `test/` đều dùng `MINIMAL_BUILD ON`:

- component phải reachable từ dependency graph production qua `REQUIRES`;
- `test/CMakeLists.txt` phải thêm `mcp_ws_bridge` vào `TEST_COMPONENTS` để test directory của component được collect;
- mỗi project phải regenerate dependency lock/config riêng theo convention ESP-IDF hiện tại;
- clean build phải kiểm tra component thực sự compile/link, không chỉ được download vào `managed_components`.

`mcp_ws_bridge/CMakeLists.txt` cần dependency tối thiểu:

```text
esp_websocket_client
esp_event
nvs_flash
freertos
espressif__cjson
mcp_endpoint or mcp_core target
```

Thêm dependency public/private đúng với header usage; TLS/certificate-bundle symbols phải được resolve qua dependency chain của component version đã lock.

Nếu `mcp_core` nằm trong `mcp_endpoint`, tránh circular dependency:

```text
mcp_ws_bridge -> mcp_endpoint/core
web_server    -> mcp_ws_bridge public config/status API
```

`mcp_endpoint` không được depend ngược vào `mcp_ws_bridge`.

---

# 43. Dependency direction

Bắt buộc:

```text
mcp_http_adapter ---->
                      mcp_core -> mcp_tools -> command_executor
mcp_ws_bridge ------->
```

Không:

```text
mcp_core -> mcp_ws_bridge
mcp_core -> web_server
mcp_endpoint -> web_server
mcp_ws_bridge -> web_server
```

Web Settings gọi public API của bridge, nhưng bridge không biết Web UI.

---

# 44. Phase triển khai cập nhật

## Phase 0 — Wire contract capture

**Trạng thái:** một phần — initialize, initialized, tools/list request và ping
đã freeze; `tools/call`, tools/list response và reconnect/close vẫn pending.

Tasks:

1. freeze các raw frames đã quan sát: initialize, initialized, tools/list, ping;
2. capture `tools/call` request/response thật;
3. capture reconnect/close behavior và message phụ nếu có;
4. tạo fixtures đã redacted;
5. xác định endpoint secret/threat model;
6. viết compatibility matrix.

Exit:

```text
XIAOZHI_WIRE_CONTRACT.md approved
```

---

## Phase 1 — MCP core transport decoupling

**Trạng thái:** đã implement; cần chạy bộ Unity trên phần cứng để chốt PASS runtime.

Tasks:

1. tạo `mcp_core`;
2. tạo `mcp_wire_context_t`;
3. tạo per-request responder với ownership/retain/release contract;
4. refactor `mcp_protocol_detect()` không nhận `httpd_req_t *`;
5. tạo `mcp_http_adapter`;
6. giữ toàn bộ HTTP behavior;
7. loại global runtime transport switching.

Exit:

```text
all existing MCP HTTP tests PASS
```

---

## Phase 2 — Protocol compatibility từ Phase 0

**Trạng thái:** đã implement/test ở mức compile cho lifecycle đã capture;
`tools/call` fixture test còn bị gate bởi capture thật.

Tasks:

1. thêm đúng protocol version Xiaozhi cần;
2. implement negotiation rule;
3. add captured initialize tests;
4. add `tools/list` fixture tests;
5. add JSON-RPC `ping` fixture/handler tests;
6. add `tools/call` fixture tests;
7. add MCP handshake/session-state tests;
8. giữ MCP 2026 HTTP mode không regression.

Exit:

```text
all captured Xiaozhi messages pass through mcp_core using mock responder
```

---

## Phase 3 — `mcp_ws_bridge`

**Trạng thái:** đã implement baseline; live endpoint, BLE async và soak test trên
ESP32-S3 thật vẫn pending.

Tasks:

1. add `esp_websocket_client` dependency;
2. config/NVS;
3. WS/WSS connect;
4. Wi-Fi event integration;
5. WS + MCP handshake/readiness state machine;
6. reconnect/backoff;
7. RX assembly;
8. TX queue;
9. generation protection;
10. responder lifetime protection across stop/reload;
11. connect WS adapter to `mcp_core`.

Exit:

```text
initialize -> tools/list -> tools/call works over real WS test endpoint
```

---

## Phase 4 — Settings integration

**Trạng thái:** đã implement API/UI; endpoint raw không xuất hiện trong GET API.

Tasks:

1. extend GET `/api/settings`;
2. add one mutation route;
3. Settings UI section;
4. secret masking;
5. implement/document selected credential-at-rest policy;
6. reload/reconnect after config save;
7. no reboot required.

Exit:

```text
user can configure endpoint and see connection state from dashboard
```

---

## Phase 5 — Integration/stress

Test simultaneously:

```text
9 BLE links
HTTP /mcp
Xiaozhi WS MCP
Web UI
Wi-Fi reconnect
BLE reconnect
```

Exit:

```text
no leak
no cross-transport response
no old-generation response
no boot dependency on Xiaozhi
```

---

# 45. Test plan chi tiết

## 45.1 Existing HTTP regression suite

Bắt buộc giữ pass:

```text
MCP 2026 conformance
MCP compatibility initialize
notifications/initialized
tools/list
tools/call
HTTP auth gate
rate limit
header validation
async device command
```

## 45.2 Core responder tests

Test hai responder độc lập:

```text
HTTP responder A
WS responder B
```

Submit concurrent async jobs và verify response không cross.

## 45.3 Xiaozhi fixture tests

Replay nguyên raw fixture từ Phase 0.

Không sửa fixture để “phù hợp gateway”.

Gateway phải adapt vào observed contract.

Fixture suite tối thiểu:

```text
initialize 2024-11-05 id=0
notifications/initialized không params
tools/list id=1 params={}
ping id=2 params={}
tools/call request/response từ capture thật
```

`ping` expected response là `result:{}`, không phải method error hoặc WebSocket PONG.

## 45.4 Notification test

WS notification:

```text
input notification
expected: zero WebSocket response frames
```

HTTP notification:

```text
expected: HTTP 202 empty
```

## 45.5 Generation test

```text
receive call generation 7
submit BLE
WS disconnect
reconnect generation 8
BLE generation 7 completes
```

Expected:

```text
no WS TX
no crash
context released
```

## 45.6 Responder lifetime test

```text
receive tools/call generation 7
submit BLE
disconnect
disable/reload bridge
release connection resources
BLE generation 7 completes
```

Expected:

```text
no send
no use-after-free
no double release
no leak
bridge can enable/connect again
```

Chạy variant completion đồng thời với disconnect để exercise race window.

## 45.7 Handshake/readiness test

Verify:

- WS open -> state `MCP_HANDSHAKING`, chưa phải `READY`;
- đúng initialize response `2024-11-05`;
- initialized notification -> `READY`;
- handshake timeout -> close/backoff;
- reconnect reset toàn bộ protocol state;
- duplicate initialize xử lý deterministically;
- Web Settings chỉ báo Connected khi `READY`.

## 45.8 Reconnect test

Verify:

- exponential delay;
- jitter within bounds;
- max cap;
- reset after stable connection;
- no reconnect while Wi-Fi disconnected.

## 45.9 Settings secret test

Store signed URL.

Verify:

```text
GET settings -> masked
logs -> no query token
NVS -> stored correctly
UI -> does not expose secret after reload
```

Verify thêm invalid/malicious URI, overlength URL, parser error logging, explicit clear và redirect-to-different-host policy.

Nếu chọn Policy A, verify raw flash/NVS không chứa plaintext secret. Nếu chọn Policy B, verify UI/documentation công bố đúng limitation.

## 45.10 Tool consistency

Enable/disable tool and compare HTTP vs WS `tools/list`.

## 45.11 Failure isolation

Kill Xiaozhi endpoint.

Expected:

```text
bridge -> BACKOFF
Web UI -> PASS
HTTP MCP -> PASS
BLE -> PASS
```

## 45.12 Memory soak

Run reconnect loop + command traffic for extended test.

Measure:

```text
internal heap delta
largest free block
PSRAM delta
queue depth
stack watermark
```

Không có monotonic memory loss.

---

# 46. Acceptance criteria

Feature được coi là production-ready khi toàn bộ tiêu chí dưới đây được xác
minh. Build thành công chỉ chứng minh compile/link, không thay thế test phần cứng:

1. Xiaozhi wire contract đã được captured và lưu fixture.
2. HTTP MCP existing tests không regression.
3. MCP core không phụ thuộc `httpd_req_t *`.
4. Production runtime không dùng global mutable transport để multiplex HTTP/WS.
5. Gateway kết nối external Xiaozhi WS/WSS trực tiếp.
6. Gateway reconnect khi socket mất.
7. Gateway dừng reconnect khi Wi-Fi mất IP.
8. WS open chỉ vào `MCP_HANDSHAKING`; UI chỉ báo Connected khi MCP `READY`.
9. Xiaozhi `initialize` hoàn tất với exact observed version `2024-11-05`.
10. `notifications/initialized` không tạo WS response frame.
11. Xiaozhi JSON-RPC `ping` trả `result:{}` với đúng request ID.
12. Xiaozhi `tools/list` trả đúng shared tool catalog.
13. Dynamic exposed device tools xuất hiện nhất quán qua HTTP và WS.
14. Xiaozhi `tools/call` gateway tool hoạt động.
15. Xiaozhi `tools/call` BLE device tool hoạt động.
16. Async BLE response giữ đúng JSON-RPC ID.
17. Old generation response bị drop sau reconnect.
18. Disconnect/reload trong lúc async completion không UAF/double-free/leak.
19. Xiaozhi offline không ảnh hưởng Web UI/HTTP MCP/BLE.
20. Endpoint secret không xuất hiện plaintext trong GET settings/log/fixtures/crash diagnostics.
21. Credential-at-rest policy được implement hoặc limitation được document rõ.
22. WSS certificate verification bằng CA bundle hoạt động production.
23. Clean firmware/test builds compile và link `mcp_ws_bridge` dưới `MINIMAL_BUILD`.
24. Web route budget không bị vượt.
25. Memory soak không leak.

---

# 47. Các việc tuyệt đối không làm

```text
- port nguyên mcp_pipe.py vào C;
- chạy subprocess/stdio proxy trên ESP32;
- gọi localhost HTTP /mcp từ WS bridge;
- fake httpd_req_t cho WebSocket;
- dùng global mutable mcp_transport để switch HTTP/WS;
- duplicate MCP parser;
- duplicate tool registry;
- duplicate device exposure state;
- duplicate dispatcher;
- execute BLE directly in WS callback;
- send old async result vào connection mới;
- send empty WS frame cho notification;
- trả Method not found cho Xiaozhi JSON-RPC ping;
- coi WS socket open là MCP READY trước khi handshake hoàn tất;
- free bridge/responder context khi async BLE callback còn giữ reference;
- log signed endpoint/token;
- commit signed endpoint/token vào fixture, defaults hoặc Web assets;
- auto reconnect ở cả library và application cùng lúc;
- mở provisioning portal vì Xiaozhi disconnect;
- làm gateway boot phụ thuộc endpoint Xiaozhi.
```

---

# 48. File impact dự kiến

## Existing files likely modified

```text
components/mcp_endpoint/mcp_endpoint.c
components/mcp_endpoint/mcp_codec.c
components/mcp_endpoint/mcp_rpc.c
components/mcp_endpoint/mcp_endpoint_internal.h
components/mcp_endpoint/CMakeLists.txt
components/mcp_endpoint/test/*

components/web_server/web_settings_api.c
components/web_server/web_server.c           only if route registrar added
components/web_server/CMakeLists.txt
components/web_server/www_src/dashboard/views/settings.html
components/web_server/www_src/dashboard/js/*

main/main.c
main/CMakeLists.txt
main/idf_component.yml or relevant manifest
test/CMakeLists.txt
sdkconfig.defaults
sdkconfig.defaults.esp32s3                    if target-specific setting is needed
```

## New files/components

```text
components/mcp_endpoint/mcp_core.c
components/mcp_endpoint/mcp_http_adapter.c
components/mcp_endpoint/include/mcp_core.h

components/mcp_ws_bridge/*

test/fixtures/xiaozhi_mcp/*
docs/XIAOZHI_MCP_WIRE_CONTRACT.md
```

Exact names có thể điều chỉnh theo repo convention, nhưng boundary phải giữ.

---

# 49. Migration strategy

Không refactor tất cả một lần.

Recommended safe migration:

```text
Step A
extract response builder/core while HTTP still only transport

Step B
move protocol header extraction into HTTP adapter

Step C
run all existing tests

Step D
add mock WS responder without real socket

Step E
replay Xiaozhi fixtures

Step F
add real mcp_ws_bridge

Step G
add Settings UI
```

Mỗi step phải build/test trước khi sang step tiếp theo.

---

# 50. Rollback strategy

Compile-time feature flag:

```text
CONFIG_MCP_WS_BRIDGE=n
```

phải đưa firmware về behavior cũ:

```text
HTTP MCP
Web UI
BLE
```

mà không cần rollback MCP core refactor.

Điều này yêu cầu Phase 1 refactor phải là behavior-preserving với HTTP.

---

# 51. Observability tối thiểu

Bridge status nên có:

```text
state
retry_count
generation
last_error
last_connected_timestamp/uptime optional
last_disconnect_reason optional
```

Không cần persistent logs.

Không ghi runtime status vào NVS.

Web Settings chỉ hiển thị status cần thiết, không expose internal socket details quá mức.

---

# 52. Final architecture

```text
                                +----------------------+
                                |      Xiaozhi         |
                                | External MCP WS/WSS  |
                                +----------+-----------+
                                           |
                                      raw JSON-RPC
                                           |
                                           v
                                +----------------------+
                                |    mcp_ws_bridge     |
                                |----------------------|
                                | config/NVS           |
                                | TLS                  |
                                | reconnect            |
                                | RX/TX queues         |
                                | generation           |
                                +----------+-----------+
                                           |
                                +----------v-----------+
                                |    mcp_ws_adapter    |
                                +----------+-----------+
                                           |
                                           v
+-------------------+          +----------------------+
| POST /mcp         |          |      mcp_core        |
| HTTP client       |          |----------------------|
+---------+---------+          | JSON-RPC validation  |
          |                    | protocol negotiation |
          v                    | initialize           |
+-------------------+          | tools/list           |
| mcp_http_adapter  |--------->| tools/call           |
|-------------------|          +----------+-----------+
| HTTP auth/gate    |                     |
| protocol headers |                     v
| HTTP status       |                mcp_tools
+-------------------+                     |
                                  mcp_tool_exposure
                                           |
                                   command_executor
                                           |
                                  command_dispatcher
                                           |
                                      BLE Central
                                           |
                                        Devices
```

---

# 53. Kết luận

Repo hiện tại đã có phần lớn foundation cho direct Xiaozhi MCP integration.

Không cần thay đổi BLE architecture hoặc tạo một MCP stack thứ hai.

Các phần quan trọng nhất còn thiếu là:

```text
1. Freeze fixture cho wire contract đã quan sát và capture tools/call/reconnect còn thiếu
2. transport-neutral MCP core
3. per-request responder
4. responder ownership/lifetime an toàn khi async + reload
5. protocol compatibility 2024-11-05 + JSON-RPC ping
6. MCP handshake/readiness state trên mỗi connection generation
7. WebSocket client bridge
8. Settings/NVS integration với credential threat model rõ ràng
9. reconnect/generation/failure isolation tests
```

Kiến trúc đúng là:

```text
Xiaozhi WS ------+
                 |
                 v
              mcp_core
                 ^
                 |
HTTP /mcp -------+
                 |
              mcp_tools
                 |
          command_executor
                 |
         command_dispatcher
                 |
                BLE
```

Điểm quyết định:

> Không port `mcp_pipe.py`; chỉ tái hiện transport behavior của nó bằng một WebSocket bridge native và nối bridge đó trực tiếp vào shared MCP core của gateway.

Baseline Phase 1–4 đã được implement từ các facts quan sát. Production sign-off
vẫn bị gate bởi capture `tools/call`/reconnect còn thiếu, Unity runtime trên
ESP32-S3, live Xiaozhi handshake/tool calls, và responder-lifetime/soak tests.

---

# 54. Tài liệu/code tham chiếu

## Gateway

- `components/mcp_endpoint/mcp_endpoint.c`
- `components/mcp_endpoint/mcp_codec.c`
- `components/mcp_endpoint/mcp_tools.c`
- `components/mcp_endpoint/test/test_mcp_conformance.c`
- `components/mcp_tool_exposure/`
- `components/command_executor/`
- `components/web_server/web_settings_api.c`
- `components/web_server/web_server.c`
- `components/web_server/www_src/`
- `components/wifi_provisioning/`
- `main/main.c`
- `main/idf_component.yml`
- `sdkconfig.defaults`
- `docs/MCP_ENDPOINT_DUAL_ERA_UPDATE_PLAN_v1.1.md`
- `docs/MCP_DYNAMIC_DEVICE_TOOLS_DASHBOARD_EXPOSURE_SPEC_v1.1.md`
- `docs/WEB_DASHBOARD_SETTINGS_DEVELOPMENT_SPEC_v1.1.md`

## External references

- `https://github.com/78/mcp-calculator`
- `mcp_pipe.py`
- `requirements.txt`
- `https://github.com/modelcontextprotocol/python-sdk/tree/v1.20.0`
- `https://github.com/espressif/esp-protocols/tree/master/components/esp_websocket_client`

---

**End of specification v1.1**
