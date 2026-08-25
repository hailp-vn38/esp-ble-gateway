# MCP Endpoint

## 1. Tổng quan

`mcp_endpoint` cung cấp một endpoint HTTP để AI Agent hoặc ứng dụng trong mạng
LAN gọi các command của ESP32 BLE Gateway bằng JSON-RPC 2.0.

Endpoint hiện tại là một **MCP subset tối giản**, tập trung vào hai thao tác:

- lấy danh sách tool từ Command Dispatcher;
- gọi một tool và trả kết quả dưới dạng JSON-RPC.

Component không triển khai đầy đủ MCP protocol. Hiện chưa có session,
`initialize`, resources, prompts, streaming/SSE hoặc authentication.

```text
AI Agent / Client
        │
        │ POST /mcp (JSON-RPC 2.0)
        ▼
  mcp_endpoint.c
        │
        ├── mcp_tools.c ──► command_dispatcher ──► Gateway/BLE device
        │
        └── mcp_rpc.c   ──► JSON-RPC result/error response
```

## 2. Cấu trúc component

```text
components/mcp_endpoint/
├── include/
│   └── mcp_endpoint.h
├── CMakeLists.txt
├── mcp_endpoint.c
├── mcp_endpoint_internal.h
├── mcp_rpc.c
├── mcp_tools.c
└── README.md
```

| File | Chức năng |
| --- | --- |
| `mcp_endpoint.c` | Đăng ký route, nhận HTTP body, kiểm tra JSON-RPC và điều phối method |
| `mcp_rpc.c` | Dựng và gửi JSON-RPC result/error response |
| `mcp_tools.c` | Liệt kê tool, chuẩn hóa arguments thành `gw_message_t` và gọi dispatcher |
| `mcp_endpoint_internal.h` | Kiểu dữ liệu và API private giữa các source file |
| `include/mcp_endpoint.h` | Public API của component |
| `CMakeLists.txt` | Khai báo source và dependency cho ESP-IDF |

Chỉ `include/mcp_endpoint.h` là public interface. Không nên gọi trực tiếp các
hàm khai báo trong `mcp_endpoint_internal.h` từ component khác.

## 3. Dependency

Component phụ thuộc vào:

- `esp_http_server`: đăng ký và xử lý `POST /mcp`;
- `espressif__cjson`: parse và tạo JSON;
- `cbor_codec`: chuyển arguments JSON sang `gw_message_t`;
- `command_dispatcher`: lấy danh sách command và thực thi command.

Các dependency được khai báo trong `CMakeLists.txt`:

```cmake
idf_component_register(
    SRCS "mcp_endpoint.c"
         "mcp_rpc.c"
         "mcp_tools.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server cbor_codec command_dispatcher espressif__cjson
)
```

## 4. Public API

Include public header:

```c
#include "mcp_endpoint.h"
```

Component cung cấp một hàm:

```c
int mcp_endpoint_register(httpd_handle_t server);
```

Hàm đăng ký route `POST /mcp` vào một HTTP server đã được khởi động:

- trả `0` khi đăng ký thành công;
- trả `-1` nếu handle bằng `NULL` hoặc không đăng ký được route.

Ví dụ tích hợp:

```c
httpd_handle_t server = web_server_start();
if (server != NULL && mcp_endpoint_register(server) != 0) {
    ESP_LOGE(TAG, "MCP endpoint registration failed");
}
```

`command_dispatcher_init()` cần hoàn tất trước khi client gọi endpoint để
registry đã chứa các gateway command mặc định.

## 5. HTTP contract

```text
Method       POST
Path         /mcp
Content-Type application/json
Body limit   4096 bytes
```

Request phải là một JSON object có dạng:

```json
{
  "jsonrpc": "2.0",
  "method": "tools/list",
  "params": {},
  "id": 1
}
```

Quy tắc chính:

- `jsonrpc` bắt buộc và phải bằng `"2.0"`;
- `method` bắt buộc, phải là chuỗi không rỗng;
- `id` có thể là string, number hoặc `null`;
- nếu không có `id`, request được xử lý như notification và server trả
  `204 No Content`;
- response JSON thông thường có `Content-Type: application/json`.

## 6. Các method được hỗ trợ

### 6.1. Liệt kê tool

Tên method ưu tiên:

```text
tools/list
```

Alias tương thích cũ:

```text
list_tools
```

Ví dụ:

```sh
curl -X POST http://<GATEWAY_IP>/mcp \
  -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","method":"tools/list","id":1}'
```

Danh sách được lấy động từ registry của `command_dispatcher`, không hardcode
trong HTTP handler. Response có cả mô tả tool theo MCP và `tool_names` để giữ
tương thích với client cũ:

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "tools": [
      {
        "name": "get_status",
        "description": "Get gateway and BLE status",
        "inputSchema": {
          "type": "object",
          "properties": {
            "device_id": { "type": "string" },
            "bool_value": { "type": "boolean" }
          }
        }
      }
    ],
    "tool_names": ["get_status"]
  }
}
```

Ví dụ trên được rút gọn. Response thực tế chứa toàn bộ command đã đăng ký và
các field trong input schema.

### 6.2. Gọi tool

Tên method ưu tiên:

```text
tools/call
```

Alias tương thích cũ:

```text
call_tool
```

Dạng `name` và `arguments`:

```sh
curl -X POST http://<GATEWAY_IP>/mcp \
  -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","method":"tools/call","params":{"name":"get_status","arguments":{}},"id":"status-1"}'
```

Dạng tương thích cũ với `command` nằm trực tiếp trong `params`:

```sh
curl -X POST http://<GATEWAY_IP>/mcp \
  -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","method":"call_tool","params":{"device_id":"lamp-1","command":"toggle","bool_value":true},"id":2}'
```

Các argument được chuyển sang `gw_message_t`. Những field hiện được chuyển
tiếp gồm:

```text
protocol_version
device_id
int_value
bool_value
name
device_type
ble_addr
ble_addr_type
```

Nếu `type` không được truyền, component suy luận theo thứ tự:

1. command có trong registry: `gateway_command`;
2. command không có trong registry nhưng có `device_id`: `device_command`;
3. các trường hợp còn lại: `gateway_command`.

Client cũng có thể truyền rõ `type` bằng `gateway_command` hoặc
`device_command`. Giá trị khác sẽ bị từ chối.

Response thành công có dạng:

```json
{
  "jsonrpc": "2.0",
  "id": "status-1",
  "result": {
    "success": true,
    "message": "command result"
  }
}
```

Nếu `dispatch_result.message` là một JSON hợp lệ, component thêm bản parse vào
field `data`:

```json
{
  "jsonrpc": "2.0",
  "id": 3,
  "result": {
    "success": true,
    "message": "{\"devices\":[]}",
    "data": {
      "devices": []
    }
  }
}
```

Lưu ý: kết quả thực thi command thất bại vẫn nằm trong JSON-RPC `result`, với
`success: false`. JSON-RPC `error` chỉ dùng cho lỗi protocol, validation hoặc
lỗi nội bộ trước khi có kết quả từ dispatcher.

## 7. JSON-RPC error

Response lỗi có dạng:

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "error": {
    "code": -32601,
    "message": "Method not found"
  }
}
```

| Code | Ý nghĩa trong component |
| ---: | --- |
| `-32700` | JSON không parse được hoặc không nhận được request body hợp lệ |
| `-32600` | Request không đúng JSON-RPC 2.0, `id` không hợp lệ hoặc body vượt giới hạn |
| `-32601` | Method không được hỗ trợ |
| `-32602` | `params`, tool name, command type hoặc arguments không hợp lệ |
| `-32603` | Lỗi nội bộ, thường là không đủ bộ nhớ hoặc không tạo được tool list |

Khi không thể tạo JSON response do hết bộ nhớ, HTTP server trả
`500 Internal Server Error`.

## 8. Luồng xử lý

```text
POST /mcp
    │
    ▼
Kiểm tra content length và đọc body
    │
    ▼
Parse + validate JSON-RPC 2.0
    │
    ├── tools/list ──► lấy command registry ──► tool list
    │
    ├── tools/call ──► chuẩn hóa gw_message_t ──► dispatcher
    │
    └── method khác ──► -32601 Method not found
    │
    ▼
JSON-RPC result/error hoặc HTTP 204 cho notification
```

## 9. Build và kiểm thử thủ công

Build firmware từ thư mục gốc:

```sh
idf.py set-target esp32s3
idf.py build
```

Sau khi flash firmware và ESP32 kết nối Wi-Fi, kiểm tra endpoint:

```sh
curl -i -X POST http://<GATEWAY_IP>/mcp \
  -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","method":"tools/list","id":1}'
```

Kiểm tra method không tồn tại:

```sh
curl -i -X POST http://<GATEWAY_IP>/mcp \
  -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","method":"unknown","id":2}'
```

Kiểm tra notification, kết quả mong đợi là `204 No Content`:

```sh
curl -i -X POST http://<GATEWAY_IP>/mcp \
  -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","method":"tools/list"}'
```

## 10. Giới hạn và bảo mật

- Endpoint hiện không yêu cầu API key hoặc cơ chế xác thực khác.
- HTTP chưa được mã hóa bằng TLS.
- Không nên expose `POST /mcp` trực tiếp ra Internet.
- Endpoint không phải MCP server đầy đủ; client yêu cầu MCP handshake/session
  chuẩn sẽ không tương thích trực tiếp.
- Device command có thể chờ ACK trong dispatcher, vì vậy HTTP request có thể
  kéo dài tới khi command hoàn tất hoặc timeout.

Trong giai đoạn hiện tại, endpoint chỉ nên được sử dụng trong mạng LAN tin cậy.
