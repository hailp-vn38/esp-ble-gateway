# Kế hoạch cải tiến mcp_endpoint theo danh_gia.md

## Bối cảnh

`components/mcp_endpoint` hiện tại có nhiều vấn đề P0/P1: infinite retry trên timeout, sai mã lỗi JSON-RPC, không có authentication, hidden command surface, BLE ACK block HTTP task 2s, và thiếu hoàn toàn wire codec MCP 2026-07-28. Kế hoạch này thực thi 8 bước theo thứ tự từ danh_gia.md.

**Ràng buộc ESP32:** RAM ~200KB, HTTP stack 12KB, LWIP sockets ~10, `dispatch_result_t` 4KB. Mọi hàm mới phải xử lý OOM (cJSON/malloc trả NULL).

---

## Bước 1 — Characterization/integration tests

**Tạo mới:**
- `components/mcp_endpoint/test/test_mcp_endpoint.c`
- `components/mcp_endpoint/test/CMakeLists.txt`

**Thiết kế:** Dùng weak-symbol mock cho `httpd_req_recv` / `httpd_resp_send` — mock nạp byte từ buffer tĩnh, ghi nhận payload JSON-RPC để assert. Không kéo lwip vào mock. Theo pattern `components/command_dispatcher/test/test_command_dispatcher.c`.

**Test cases tối thiểu:**
1. `tools/list` happy path → trả về mảng tools đúng cấu trúc
2. `tools/call` với tool đã đăng ký → dispatch đúng, trả result
3. JSON root là array → hiện tại trả -32700 (bug, ghi nhận baseline)
4. Body vượt 4096 → -32600
5. Body rỗng → -32600
6. JSON malformed → -32700
7. Method không tồn tại → -32601
8. Notification (không có id) → 204

**Cập nhật:** `test/CMakeLists.txt` — thêm `mcp_endpoint` vào `TEST_COMPONENTS`.

**Acceptance:** `idf.py test` pass; baseline ghi nhận hành vi sai hiện tại.

---

## Bước 2 — Sửa receive timeout, phân loại lỗi, body limit, OOM paths

**Sửa:** `mcp_endpoint.c`, `mcp_tools.c`, `mcp_rpc.c`

**Thay đổi cụ thể:**

1. **Infinite retry** (mcp_endpoint.c:27):
```c
// Trước: if (chunk == HTTPD_SOCK_ERR_TIMEOUT) continue;
// Sau: retry tối đa 3 lần, sau đó fail
#define MCP_MAX_RECV_RETRIES 3
```

2. **Sai mã lỗi** (mcp_endpoint.c:50-53):
```c
// Root không phải object → -32600 (Invalid Request), không phải -32700
```

3. **Body limit:** Trả JSON-RPC error `-32600` kèm HTTP 413 khi body > `MCP_MAX_REQUEST_LEN`, không đọc body.

4. **OOM paths trong mcp_tools.c:**
   - `create_input_schema()`: `cJSON_AddItemToObject` sau mỗi `cJSON_CreateObject` phải check NULL → free partial → trả NULL
   - `mcp_tools_call()`: `dispatch_result_t dispatch_result;` trên stack → thay bằng `dispatch_result_t *dispatch_result = malloc(sizeof(*dispatch_result))` (4KB heap thay vì stack)

5. **`receive_body()` trả status rõ ràng:**
```c
typedef enum {
    MCP_RECV_OK,
    MCP_RECV_ERR_SIZE,
    MCP_RECV_ERR_MEM,
    MCP_RECV_ERR_READ,
    MCP_RECV_ERR_TIMEOUT,
} mcp_recv_status_t;
mcp_recv_status_t mcp_receive_body(httpd_req_t *req, char **out_body);
```

**Acceptance:** Test từ Bước 1 pass với mã lỗi mới; heap stable sau error paths.

---

## Bước 3 — Authentication, Content-Type, Origin/Host validation

**Tạo mới:** `components/mcp_endpoint/mcp_auth.c`, thêm khai báo vào `mcp_endpoint_internal.h`

**Sửa:** `mcp_endpoint.c` (gọi validation trước khi đọc body)

**Thiết kế:**

```c
// Token lưu trong Kconfig, đọc qua CONFIG_MCP_AUTH_TOKEN
// Nếu token rỗng → dev mode, cho phép kèm ESP_LOGW
bool mcp_auth_check(httpd_req_t *req);        // Authorization: Bearer <token>
bool mcp_validate_content_type(httpd_req_t *req); // application/json
bool mcp_validate_host(httpd_req_t *req);     // Host trong allowlist
```

- So sánh token: vòng lặp constant-time (không dùng `strcmp`)
- Không log token, chỉ log "auth failed"
- Host allowlist từ `CONFIG_MCP_HOST_ALLOWLIST` (comma-separated), xử lý `host:port` và IPv6 `[...]`
- Origin: nếu có, phải trùng Host allowlist
- Rate limit: token bucket đơn giản 10 req/s (biến static + tick count)

**Kconfig mới** trong `components/mcp_endpoint/Kconfig.projbuild`:
```
CONFIG_MCP_AUTH_TOKEN (string, default "")
CONFIG_MCP_HOST_ALLOWLIST (string, default "gateway.local,192.168.4.1")
```

**Threat model** ghi trong comment đầu file `mcp_auth.c` (không tạo file riêng):
- Bearer trên HTTP plaintext có thể bị sniff → chỉ dùng trong LAN tin cậy
- Host header do attacker kiểm soát → không dùng thay thế auth

**Acceptance:** Không có token → -32021 (hoặc dev mode cho phép); token sai → -32021; Host không khớp → -32021.

---

## Bước 4 — Strict tool registry, schema đầy đủ, command allowlist

**Tạo mới:** `components/mcp_endpoint/mcp_registry.c`

**Sửa:** `mcp_tools.c`

**Thiết kế:**

```c
typedef struct {
    const char *name;
    const char *description;
    cJSON *(*input_schema)(void);  // schema riêng từng tool
    bool read_only;
    bool destructive;
} mcp_tool_desc_t;

static const mcp_tool_desc_t MCP_TOOL_TABLE[] = {
    {"add_device",    "Add a BLE device", schema_add_device,    false, false},
    {"edit_device",   "Edit a device",    schema_edit_device,   false, false},
    {"delete_device", "Delete a device",  schema_delete_device, false, true},
    {"list_devices",  "List devices",     schema_list_devices,  true,  false},
    {"get_status",    "Get gateway status", schema_get_status,  true,  false},
    {"device_command","Send command to device", schema_device_command, false, false},
};
```

**Thay đổi chính trong mcp_tools.c:**
- Xóa fallback `unknown tool + device_id → device_command` (mcp_tools.c:151-153)
- `mcp_tools_list()`: dùng `MCP_TOOL_TABLE`, schema riêng cho từng tool với `maxLength`, `pattern`, `enum` phù hợp
- `mcp_tools_call()`: tra `mcp_registry_find(name)`, không thấy → `-32602`
- Command allowlist: `CONFIG_MCP_DEVICE_COMMAND_ALLOWLIST` — command không trong list → tool error (`isError: true`), không phải protocol error

**Acceptance:** `tools/list` chỉ trả về tools trong registry; `tools/call` với command ngoài allowlist bị từ chối; không còn hidden command surface.

---

## Bước 5 — Async worker cho device_command

**Tạo mới:** `components/mcp_endpoint/mcp_async.c`

**Sửa:** `mcp_tools.c`, `mcp_endpoint.c`, `CMakeLists.txt`

**Thiết kế:**

```c
#define MCP_ASYNC_WORKERS 1
#define MCP_ASYNC_QUEUE_LEN 2
#define MCP_ASYNC_STACK 8192  // phải chứa dispatch_result_t 4KB heap + overhead

esp_err_t mcp_async_init(void);
esp_err_t mcp_async_deinit(void);
esp_err_t mcp_async_submit(httpd_req_t *req, cJSON *params);
```

**Luồng:**
1. `mcp_tools_call()` phát hiện `device_command` → gọi `httpd_req_async_handler_begin(req)` → enqueue vào FreeRTOS queue → trả về ngay (202 JSON với taskId)
2. Worker task chờ queue, gọi `command_dispatcher_handle()`, gọi `httpd_resp_send()` rồi `httpd_req_async_handler_complete()`
3. Queue đầy → `httpd_req_async_handler_begin()` trước, nếu enqueue fail → `httpd_req_async_handler_complete()` + trả 503

**Ràng buộc socket:**
- LWIP max 10 sockets, HTTP server dùng 1, Web UI cần ít nhất 2 → async giữ tối đa 2 socket (queue length 2)
- `dispatch_result_t` luôn heap-allocated (từ Bước 2)

**Cleanup:** `mcp_async_deinit()` gọi khi `mcp_endpoint_register()` fail; worker kiểm tra flag shutdown khi dequeue.

**Acceptance:** 3 request đồng thời → 2 queued, 1 nhận 503; stack high-water > 512B; không leak socket sau 50 cycles.

---

## Bước 6 — MCP 2026-07-28 wire codec

**Tạo mới:** `components/mcp_endpoint/mcp_codec.c`

**Sửa:** `mcp_rpc.c`, `mcp_endpoint.c`, `mcp_tools.c`

**Thiết kế:**

```c
// Parse header MCP-Protocol-Version
// Nếu không phải "2026-07-28" và legacy mode off → -32022
// Nếu "2026-07-28" → bật wire codec mới

typedef struct {
    bool mcp_2026;          // protocol version từ header
    const char *mcp_method; // Mcp-Method header
    const char *mcp_name;   // Mcp-Name header
} mcp_request_meta_t;
```

**Thay đổi wire format khi `mcp_2026=true`:**

| Hiện tại | MCP 2026-07-28 |
|---|---|
| `{success, message, data}` | `{resultType:"complete", content:[{type:"text",text:...}], isError:false}` |
| Không có `_meta` | `_meta: {"io.modelcontextprotocol/protocolVersion": "2026-07-28"}` |
| `tools/list` không có cache | Thêm `ttlMs`, `cacheScope` |
| Không có error codes mới | `-32020` transport, `-32021` auth, `-32022` protocol version |
| Không có discovery | `server/discover` trả `{name, version, protocolVersion, capabilities}` |

**Acceptance:** Request với header `MCP-Protocol-Version: 2026-07-28` nhận response đúng wire format; không có header → legacy format (nếu legacy mode on).

---

## Bước 7 — Legacy mode feature flag

**Sửa:** `Kconfig.projbuild`, `mcp_endpoint_internal.h`, `mcp_codec.c`

```
CONFIG_MCP_LEGACY_MODE (bool, default y)
```

- `CONFIG_MCP_LEGACY_MODE=y`: chấp nhận request không có header MCP, trả legacy format `{success,message,data}`
- `CONFIG_MCP_LEGACY_MODE=n`: bắt buộc header `MCP-Protocol-Version: 2026-07-28`, từ chối bằng `-32022` nếu thiếu/sai
- Runtime override qua NVS key `"mcp_legacy"` (cho phép OTA flip không cần reflash)

**Acceptance:** `legacy=y` → test cũ vẫn pass; `legacy=n` → request thiếu header nhận `-32022`.

---

## Bước 8 — Conformance, concurrency, memory stress tests

**Tạo mới:**
- `components/mcp_endpoint/test/test_mcp_conformance.c` — vector từ MCP 2026 spec
- `components/mcp_endpoint/test/test_mcp_stress.c` — concurrency + memory

**Test cases:**
- Conformance: mỗi error code (-32020/-32021/-32022), mỗi method với header đúng/sai, `resultType` bắt buộc
- Concurrency: 5 HTTP client gửi đồng thời, kiểm tra không socket leak
- Memory: loop 100x `tools/call` → `heap_caps_check_integrity_all()` → leak < 2KB
- Timeout: mock `httpd_req_recv` trả `HTTPD_SOCK_ERR_TIMEOUT` → không infinite loop

**Acceptance:** Tất cả test pass; heap stable sau stress test.

---

## Thứ tự phụ thuộc

```
Bước 1 → Bước 2 → Bước 3 → Bước 4 → Bước 5 → Bước 6 → Bước 7 → Bước 8
```

Bước 3 và 4 có thể song song sau Bước 2. Bước 5 cần Bước 2 (heap dispatch_result_t). Bước 6 cần Bước 4 (registry). Bước 7 cần Bước 6 (codec). Bước 8 cần tất cả.

## Xác minh end-to-end

```bash
cd test && idf.py set-target esp32s3 && idf.py build && idf.py -p <PORT> flash monitor
idf.py set-target esp32s3 && idf.py build   # verify main firmware compiles
# Test thủ công với curl:
curl -X POST http://<IP>/mcp \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer <token>" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}'
```

Lưu ý: không có target `idf.py test` — Unity tests tự chạy lúc boot rồi rơi vào
menu tương tác (theo AGENTS.md).

---

## Quyết định thiết kế đã khóa (bản hoàn thiện)

Các điều chỉnh so với bản gốc sau khi review code thực tế:

1. **Bước 1 — transport hooks thay vì weak-symbol mock.** Weak-symbol
   `httpd_req_recv`/`httpd_resp_send` gây xung đột symbol khi `esp_http_server`
   bị link vào test app. Thay bằng struct hook bất biến kiểu
   `mcp_transport_t` (recv/send/send_err/set_type/set_status/set_hdr/get_header/
   async_begin/async_complete), mặc định trỏ vào hàm httpd thật, test inject
   qua `mcp_transport_set()` — cùng pattern với `device_command_set_hooks()`.

2. **Bước 2 — early-reject phải đóng connection.** Mọi path từ chối mà không
   đọc body (oversize, auth fail, host fail, rate limit, sai Content-Type) đặt
   header `Connection: close`, nếu không keep-alive sẽ parse rác từ body còn
   tồn đọng. `dispatch_result_t` không heap-alloc mỗi call nữa: một buffer
   static duy nhất + mutex (đồng thời là lock serialization dispatcher giữa
   httpd task và async worker).

3. **Bước 3 — semantics lỗi HTTP rõ ràng.** Token sai/thiếu → `401` +
   `-32021` (+ `WWW-Authenticate: Bearer`); Host/Origin không khớp → `403` +
   `-32021`; rate limit vượt → `429` + `-32021`; Content-Type sai → `415`.
   Rate limit chạy trước auth để chặn brute-force token.

4. **Bước 4 — allowlist denial là tool error, không phải protocol error.**
   Unknown tool → `-32602`. Command ngoài allowlist → result với
   `isError: true` (legacy: `success: false`). Fallback
   `unknown + device_id → device_command` bị xóa hoàn toàn kể cả legacy mode
   (breaking change có chủ ý, ghi trong README).

5. **Bước 5 — validate đồng bộ trước enqueue.** Job chỉ mang `gw_message_t`
   (256B) + id copy + meta; worker chỉ dispatch + format + send + complete.
   `httpd_req_async_handler_begin()` fail hoặc queue đầy → 503 ngay trên path
   đã biết. Notification device_command cũng đi qua worker nhưng worker bỏ qua
   send response. Stack high-water được log sau mỗi job.

6. **Bước 6 — wire contract 2026-07-28.** `tools/call` trả CallToolResult:
   `{resultType:"complete", content:[{type:"text",text}], isError,
   structuredContent?, _meta}`; `tools/list` thêm `ttlMs`/`cacheScope`;
   method mới `server/discover`; `_meta["io.modelcontextprotocol/
   protocolVersion"]="2026-07-28"` trên mọi result. Header version khác giá
   trị hỗ trợ → `-32022` kể cả khi legacy mode bật.

7. **Bước 7 — NVS override đọc mỗi request** (namespace `mcp`, key `legacy`,
   u8) để OTA-flip có hiệu lực không cần reboot; đọc thất bại → fallback
   Kconfig.

8. **Bước 8 — giới hạn thực tế của Unity on-target.** Concurrency nhiều client
   thật cần hardware ngoài; unit test phủ: ma trận error code, wire format
   matrix, timeout-no-infinite-loop, heap-stability loop (leak < 2KB), queue
   full → 503 với mock ACK. Đo concurrency thật bằng curl song song thủ công.
