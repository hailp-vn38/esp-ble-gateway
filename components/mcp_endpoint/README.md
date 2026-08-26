# MCP Endpoint

## 1. Tổng quan

`mcp_endpoint` cung cấp một endpoint HTTP để AI Agent hoặc ứng dụng trong mạng
LAN gọi các command của ESP32 BLE Gateway bằng JSON-RPC 2.0.

Endpoint hỗ trợ hai chế độ wire format:

- **Legacy** (mặc định): JSON-RPC 2.0 thuần, response dạng `{success,message,data}`;
- **MCP `2026-07-28`**: client gửi header `MCP-Protocol-Version: 2026-07-28`,
  response chuyển sang CallToolResult (`resultType`, `content`, `isError`,
  `_meta`) kèm cache hints cho `tools/list` và method `server/discover`.

Component KHÔNG triển khai đầy đủ MCP protocol: không có session,
`initialize`, resources, prompts hay streaming/SSE.

```text
AI Agent / Client
        │
        │ POST /mcp (JSON-RPC 2.0)
        ▼
  mcp_endpoint.c ──► mcp_auth.c      (token / Host / Origin / rate limit)
        │        ──► mcp_codec.c     (header 2026-07-28, legacy NVS flag)
        │
        ├── mcp_tools.c ──► command_dispatcher ──► Gateway/BLE device
        │         ▲
        │         └── mcp_registry.c (tool table + schema + annotations)
        │
        ├── mcp_async.c   (device_command chạy trên worker riêng)
        │
        └── mcp_rpc.c     (JSON-RPC result/error, legacy + 2026 envelope)
```

## 2. Cấu trúc component

```text
components/mcp_endpoint/
├── include/
│   └── mcp_endpoint.h          Public API
├── Kconfig.projbuild           Token, allowlist, legacy mode, rate limit
├── CMakeLists.txt
├── mcp_endpoint.c              Route, receive body, JSON-RPC dispatch
├── mcp_endpoint_internal.h     Kiểu + API private giữa các source file
├── mcp_auth.c                  Bearer token (constant-time), Host/Origin,
│                               Content-Type, rate limit (token bucket 10 rps)
├── mcp_codec.c                 Header MCP-Protocol-Version, NVS legacy flag,
│                               server/discover payload
├── mcp_registry.c              Bảng tool duy nhất: schema từng tool +
│                               annotations (readOnlyHint, destructiveHint…)
├── mcp_tools.c                 Chuẩn hóa arguments → gw_message_t, allowlist
│                               device_command, format kết quả theo wire mode
├── mcp_async.c                 Worker task (1 worker / queue 2) cho
│                               device_command — không block HTTPD task 2s
├── mcp_rpc.c                   Envelope JSON-RPC legacy + 2026 (_meta)
├── test/                       Unity tests (characterization, conformance,
│                               stress) qua transport-hook mock
└── README.md
```

Chỉ `include/mcp_endpoint.h` là public interface. Không nên gọi trực tiếp các
hàm khai báo trong `mcp_endpoint_internal.h` từ component khác.

## 3. Dependency

- `esp_http_server`: đăng ký và xử lý `POST /mcp`;
- `espressif__cjson`: parse và tạo JSON;
- `cbor_codec`: chuyển arguments JSON sang `gw_message_t`;
- `command_dispatcher`: thực thi command;
- `nvs_flash`: override runtime cho token và legacy mode;
- `esp_timer`, `freertos`: rate limit và async worker.

## 4. Public API

```c
#include "mcp_endpoint.h"

int mcp_endpoint_register(httpd_handle_t server);
```

Hàm khởi tạo async worker, đăng ký route `POST /mcp`; trả `0` thành công,
`-1` thất bại (worker/route). Gọi sau khi `command_dispatcher_init()` +
`freeze_registry()`.

## 5. HTTP contract

```text
Method       POST
Path         /mcp
Content-Type application/json (bắt buộc, sai → 415)
Body limit   4096 bytes (vượt → 413 + Connection: close)
Rate limit   10 req/s burst 10 (vượt → 429)
Auth         Authorization: Bearer <token> (sai/thiếu → 401)
Host         phải nằm trong allowlist (sai → 403, chống DNS rebinding)
```

Mọi path từ chối trước khi đọc body đều kèm `Connection: close`.

Quy tắc JSON-RPC giữ nguyên như trước: `jsonrpc:"2.0"`, `method` bắt buộc,
`id` string/number/null; không có `id` → notification → `204 No Content`.

## 6. Các method được hỗ trợ

| Method | Alias | Ghi chú |
| --- | --- | --- |
| `tools/list` | `list_tools` | Danh sách tool cố định trong registry |
| `tools/call` | `call_tool` | Gọi tool đã registry |
| `server/discover` | — | Identity + capabilities (2026 mode) |

### 6.1. Tool registry

`tools/list` chỉ trả về 7 tool cố định — hết hidden command surface:

```text
add_device, edit_device, delete_device, list_devices, get_status,
list_device_capabilities, device_command
```

Mỗi tool có inputSchema riêng (`required`, `maxLength`, `pattern` BLE address,
range cho `ble_addr_type`) và annotations (`readOnlyHint`, `destructiveHint`,
`idempotentHint`). Unknown tool → `-32602`.

Fallback cũ `unknown tool + device_id → device_command` đã bị xóa hoàn toàn,
kể cả ở legacy mode (breaking change).

### 6.2. device_command và allowlist

Tool `device_command` chỉ cho phép các command liệt kê trong
`CONFIG_MCP_DEVICE_COMMAND_ALLOWLIST` (comma-separated; mặc định rỗng = cấm
tất cả). Command ngoài allowlist → **tool error** (`isError:true` /
legacy `success:false`), không phải protocol error.

Nếu gateway đã có capability snapshot, `device_command` còn phải tồn tại trong
snapshot và argument phải đúng type/range. Capability do peripheral quảng bá
không bao giờ tự mở rộng allowlist MCP.

`device_command` được thực thi trên async worker (1 worker, queue 2):
request nhận `503 {code:-32000}` khi queue đầy; HTTPD task không bao giờ bị
block chờ BLE ACK nữa.

### 6.3. Ví dụ

```sh
curl -X POST http://<GATEWAY_IP>/mcp \
  -H 'Content-Type: application/json' \
  -H "Authorization: Bearer $TOKEN" \
  -d '{"jsonrpc":"2.0","method":"tools/call","params":{"name":"get_status"},"id":1}'
```

Legacy form với `command` trong params vẫn hoạt động ở legacy mode cho các
command đã registry.

## 7. Wire format MCP 2026-07-28

Client bật bằng header:

```text
MCP-Protocol-Version: 2026-07-28
```

Khi đó:

- `tools/call` trả CallToolResult:
  `{resultType:"complete", content:[{type:"text",...}], isError,
  structuredContent?}` (thay `{success,message,data}`);
- `tools/list` thêm `ttlMs` / `cacheScope`;
- mọi response có `_meta["io.modelcontextprotocol/protocolVersion"]` và
  `_meta["io.modelcontextprotocol/server"]` (identity);
- `server/discover` trả `{name, version, protocolVersion, capabilities}`;
- lỗi mở rộng: `-32020` header/transport, `-32021` auth/host/rate,
  `-32022` protocol version không hỗ trợ (kèm HTTP 400).

Header version khác giá trị hỗ trợ → `-32022` bất kể legacy mode.
Header vắng mặt: legacy mode bật → xử lý như client cũ; tắt → `-32022`.

## 8. Legacy mode feature flag

```text
CONFIG_MCP_LEGACY_MODE (default y)
```

Override runtime qua NVS namespace `mcp`: key `token` (string) và key `legacy`
(u8: 1 = on, 0 = off) — flip được sau OTA mà không cần reflash. Xóa key → quay
về giá trị Kconfig.

## 9. JSON-RPC error

| Code | HTTP | Ý nghĩa |
| ---: | --- | --- |
| `-32700` | 200 | JSON không parse được hoặc đọc body thất bại |
| `-32600` | 200/413/415 | Request không đúng JSON-RPC, root sai kiểu, body vượt hạn chế, Content-Type sai |
| `-32601` | 200 | Method không hỗ trợ |
| `-32602` | 200 | params/tool name/arguments không hợp lệ, unknown tool |
| `-32603` | 200/500 | Lỗi nội bộ/OOM |
| `-32000` | 503 | Queue async đầy |
| `-32021` | 401/403/429 | Token sai, Host/Origin không hợp lệ, vượt rate limit |
| `-32022` | 400 | Protocol version thiếu (legacy off) hoặc không hỗ trợ |

Lỗi thực thi (BLE timeout, device error…) luôn nằm trong `result`
(`isError:true` / `success:false`), không dùng JSON-RPC `error`.

## 10. Build và kiểm thử

```sh
idf.py set-target esp32s3 && idf.py build       # firmware
cd test && idf.py set-target esp32s3 && idf.py build && idf.py -p <PORT> flash monitor
```

Unity tests (77 case: characterization, auth gate, wire-mode conformance,
heap-stability stress, queue-full async) tự chạy lúc boot. Tag:
`[mcp_endpoint]`, `[mcp_conformance]`, `[mcp_stress]`.

Kiểm tra thủ công:

```sh
curl -i -X POST http://<IP>/mcp -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","method":"tools/list","id":1}'
curl -i -X POST http://<IP>/mcp -H 'Content-Type: application/json' \
  -H 'MCP-Protocol-Version: 2026-07-28' \
  -d '{"jsonrpc":"2.0","method":"server/discover","id":2}'
```

## 11. Giới hạn và bảo mật

- Bearer token đi trên plaintext HTTP: có thể bị sniff/replay trong LAN.
  Chỉ dùng trong mạng tin cậy, tuyệt đối không expose ra Internet.
- Static token không phải MCP OAuth authorization; chưa có rotation UI
  (rotation thủ công qua NVS key `mcp.token`).
- Host/Origin check chống DNS rebinding, KHÔNG thay thế authentication.
- Rate limit là token bucket toàn cục 10 rps (burst 10), đủ cho một agent.
- Async worker giữ tối đa 3 socket (1 đang chạy + 2 queue) cho `/mcp`.
- Endpoint vẫn không có session/handshake MCP đầy đủ; client cần session
  chuẩn sẽ không tương thích trực tiếp.
