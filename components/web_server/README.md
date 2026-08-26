# Web Server

> **⚠️ Cập nhật Plan v2 (đã merge vào main):** tài liệu bên dưới mô tả kiến trúc
> trước refactor. Những thay đổi lớn đã áp dụng — xem `docs/refactor-execution-plan.md`:
>
> * `/api/command`, POST/PUT/DELETE `/api/devices` và MCP device_command chạy qua
>   component **`command_executor`** chung — không còn task-per-request, HTTPD
>   không chờ BLE ACK. Các mục mô tả `command_http_worker`,
>   `COMMAND_WORKER_COUNT`, `s_command_slots` đã lỗi thời.
> * BLE scan timeout dùng **esp_timer one-shot + deadline guard** — không còn
>   `ble_scan_stop_worker` / `vTaskDelete`.
> * Body handling có deadline tuyệt đối + typed errors (400/408/413 +
>   `Connection: close`) + limit theo endpoint (§34).
> * Trạng thái gateway đọc từ component **`gateway_status`** (single source),
>   `/api/status` có thêm block `"executor"` metrics.
> * REST error có thêm `error.code` machine-readable; CSP/nosniff/
>   Referrer-Policy trên mọi response.

## 1. Tổng quan

`components/web_server` là lớp HTTP/Web UI của ESP32 BLE Gateway.

Component này chịu trách nhiệm cho 4 nhóm chức năng chính:

```text
Web UI / Static Assets
        +
Gateway REST API
        +
BLE REST API
        +
Wi-Fi Provisioning API
```

Kiến trúc tổng quát:

```text
Browser / HTTP Client
        │
        ▼
ESP-IDF HTTP Server
        │
        ├── /
        │    └── Dashboard / Setup UI
        │
        ├── /api/status
        ├── /api/logs
        ├── /api/devices
        ├── /api/command
        ├── /api/ble/scan
        └── /api/wifi/*
                 │
                 ▼
        Internal Components
        ├── command_dispatcher
        ├── device_store
        ├── ble_central
        ├── wifi_provisioning
        └── log_buffer
```

Web server được xây dựng trên `esp_http_server` của ESP-IDF. Component có hai chế độ hoạt động độc lập:

```text
Gateway mode
Provisioning mode
```

---

# 2. Cấu trúc component

Hiện tại `components/web_server` gồm các phần chính:

```text
components/web_server/
│
├── web_server.c
├── web_http.c
├── web_http.h
│
├── web_assets.c
├── web_gateway_api.c
├── web_system_api.c
├── web_ble_api.c
├── web_wifi_api.c
│
├── web_modules.h
│
├── gzip_asset.py
├── CMakeLists.txt
│
├── tailwind.config.js
├── tailwind.input.css
│
├── include/
│   └── web_server.h
│
└── www/
    ├── dashboard.html
    ├── dashboard.css
    ├── setup.html
    ├── icons.css
    └── assets/
```

Phân chia trách nhiệm:

| File                | Chức năng                              |
| ------------------- | -------------------------------------- |
| `web_server.c`      | Khởi tạo HTTP server và các module     |
| `web_http.c`        | Helper chung cho JSON, error, route    |
| `web_assets.c`      | Phục vụ HTML/CSS/font                  |
| `web_gateway_api.c` | Device management + gửi device command |
| `web_system_api.c`  | Status, logs, restart                  |
| `web_ble_api.c`     | BLE scan                               |
| `web_wifi_api.c`    | Wi-Fi scan/configuration               |
| `web_modules.h`     | Internal module interface              |

---

# 3. Public API

Public API rất nhỏ:

```c
httpd_handle_t web_server_start(void);

httpd_handle_t web_server_start_provisioning(void);
```

Header:

```c
#include "web_server.h"
```

`web_server_start()` khởi động dashboard thông thường.

`web_server_start_provisioning()` khởi động giao diện cấu hình Wi-Fi.

Cả hai trả về:

```c
httpd_handle_t
```

để component khác có thể tiếp tục đăng ký route vào **cùng HTTP server**. Đây chính là cách `mcp_endpoint` đăng ký `/mcp`.

---

# 4. Hai chế độ hoạt động

## 4.1 Gateway Mode

Khi ESP32 đã có Wi-Fi và kết nối thành công:

```c
web_server_start();
```

được gọi.

Gateway mode khởi tạo:

```text
web_gateway_api
web_ble_api
```

và đăng ký:

```text
Web assets
Gateway API
System API
BLE API
```

Luồng:

```text
web_server_start()
       │
       ├── web_gateway_api_init()
       ├── web_ble_api_init()
       │
       ▼
start_server()
       │
       ├── web_assets_register_gateway()
       ├── web_gateway_api_register()
       ├── web_system_api_register_gateway()
       └── web_ble_api_register()
```

---

## 4.2 Provisioning Mode

Nếu thiết bị chưa có Wi-Fi hợp lệ:

```c
web_server_start_provisioning();
```

được dùng.

Các module lúc này:

```text
Setup UI
System API tối giản
Wi-Fi API
```

Luồng:

```text
web_server_start_provisioning()
       │
       ├── web_wifi_api_init()
       │
       ▼
start_server()
       │
       ├── web_assets_register_provisioning()
       ├── web_system_api_register_provisioning()
       └── web_wifi_api_register()
```

---

# 5. HTTP server configuration

Server sử dụng:

```c
HTTPD_DEFAULT_CONFIG()
```

sau đó điều chỉnh:

```text
task priority        = tskIDLE_PRIORITY + 6
LRU purge            = enabled
HTTP keep-alive      = enabled

keep_alive_idle      = 5 s
keep_alive_interval  = 5 s
keep_alive_count     = 3

recv timeout         = 5 s
send timeout         = 5 s
```

Gateway mode:

```text
max URI handlers = 18
stack size       = 12288 bytes
```

Provisioning mode:

```text
max URI handlers = 12
stack size       = 8192 bytes
```

---

# 6. Route tổng thể

## Gateway Mode

Các route chính:

```text
GET    /
GET    /dashboard.css
GET    /icons.css
GET    /assets/Phosphor.woff2

GET    /api/status
GET    /api/logs
POST   /api/restart

GET    /api/devices
POST   /api/devices
PUT    /api/devices
DELETE /api/devices

POST   /api/command

GET    /api/ble/scan
POST   /api/ble/scan
DELETE /api/ble/scan
```

Ngoài ra `main.c` đăng ký tiếp:

```text
POST /mcp
```

vào cùng server.

---

# 7. Dashboard UI

Gateway mode phục vụ:

```text
GET /
```

bằng file:

```text
www/dashboard.html
```

HTML không được đọc từ filesystem runtime.

Nó được **embed trực tiếp vào firmware** khi build:

```cmake
EMBED_FILES
    "www/dashboard.html"
    "www/dashboard.css"
    "www/icons.css"
    "www/assets/Phosphor.woff2"
```

Khi trình duyệt gọi:

```http
GET /
```

handler trả trực tiếp vùng binary embedded:

```c
dashboard_html_start
dashboard_html_end
```

---

# 8. Static Assets

Các tài nguyên hiện tại:

```text
/                     → dashboard.html
/dashboard.css        → dashboard.css
/icons.css            → icons.css
/assets/Phosphor.woff2
/favicon.ico
```

Cache policy:

```text
dashboard.html
    no-cache

CSS
    public, max-age=86400

Font
    public, max-age=604800
```

Riêng `/favicon.ico` hiện không có file icon thật mà trả:

```http
204 No Content
```

---

# 9. Provisioning UI

Provisioning mode sử dụng:

```text
www/setup.html
```

Nhưng khác dashboard, file này được **gzip lúc build**.

CMake thực hiện:

```text
setup.html
    │
    ▼
gzip_asset.py
    │
    ▼
setup.html.gz
    │
    ▼
embed firmware
```

Khi browser gọi `/`, server trả:

```http
Content-Encoding: gzip
```

vì vậy ESP32 không phải giải nén nội dung trước khi gửi.

---

# 10. Captive Portal

Provisioning server hỗ trợ một số URL mà hệ điều hành thường dùng để kiểm tra Internet:

```text
/generate_204
/hotspot-detect.html
/connecttest.txt
/ncsi.txt
```

Các URL này đều redirect:

```http
302 Found
Location: /
```

Mục đích là đưa người dùng trở lại:

```text
setup.html
```

khi ESP32 hoạt động như captive portal.

---

# 11. Helper HTTP chung

`web_http.c` chứa các helper để tránh mỗi API tự triển khai parser JSON.

Các hàm chính gồm:

```c
web_send_json()

web_send_api_error()

web_parse_request_json()

web_get_json_string()

web_register_routes()
```

---

# 12. JSON response

Ví dụ:

```c
cJSON *json = cJSON_CreateObject();

cJSON_AddBoolToObject(json, "success", true);

return web_send_json(request, json);
```

Response được đặt:

```http
Content-Type: application/json
Cache-Control: no-store
```

`web_send_json()` sử dụng:

```c
cJSON_PrintUnformatted()
```

nên JSON trả về dạng compact.

---

# 13. API error format

Lỗi API có format thống nhất:

```json
{
    "success": false,
    "message": "..."
}
```

Ví dụ:

```c
return web_send_api_error(
    request,
    "400 Bad Request",
    "Missing device_id"
);
```

sẽ trả HTTP status `400` cùng JSON error.

---

# 14. `/api/status`

Endpoint:

```http
GET /api/status
```

Gateway mode trả thông tin hệ thống khá đầy đủ:

```json
{
    "device_count": 3,
    "connected_count": 2,
    "ble_link_count": 2,

    "ip": "192.168.1.100",

    "wifi_connected": true,
    "provisioning": false,
    "wifi_state": "connected",

    "free_heap": 124000,
    "uptime_ms": 532180,

    "firmware_version": "1.0.0",
    "idf_version": "...",

    "wifi_ssid": "MyWifi",
    "wifi_mac": "AA:BB:CC:DD:EE:FF",
    "wifi_rssi": -58
}
```

Dữ liệu được tổng hợp từ:

```text
device_store
ble_central
wifi_provisioning
ESP-IDF system APIs
```

---

# 15. `/api/logs`

Endpoint:

```http
GET /api/logs
```

đọc log từ:

```c
log_buffer_get_recent()
```

Response:

```json
[
    {
        "text": "BLE device connected",
        "timestamp_ms": 12423
    },
    {
        "text": "Gateway started",
        "timestamp_ms": 252
    }
]
```

Số log tối đa được đọc bằng:

```c
LOG_BUFFER_CAPACITY
```

---

# 16. Restart Gateway

Endpoint:

```http
POST /api/restart
```

gọi:

```c
wifi_prov_schedule_restart(1000);
```

Nghĩa là restart được schedule sau khoảng:

```text
1000 ms
```

Response:

```json
{
    "success": true,
    "message": "Gateway restart scheduled"
}
```

---

# 17. Device REST API

Device management được expose tại:

```text
/api/devices
```

Các method:

| Method   | Chức năng     |
| -------- | ------------- |
| `GET`    | List devices  |
| `POST`   | Add device    |
| `PUT`    | Edit device   |
| `DELETE` | Delete device |

Điểm quan trọng là Web API **không tự thao tác Device Store trực tiếp**.

Nó chuyển request thành:

```c
gw_message_t
```

rồi gọi:

```c
command_dispatcher_handle()
```

Do đó:

```text
REST API
   │
   ▼
gw_message_t
   │
   ▼
Command Dispatcher
   │
   ▼
Gateway Command
   │
   ▼
Device Store
```

---

# 18. GET `/api/devices`

Request:

```http
GET /api/devices
```

Web server tạo message:

```c
type    = "gateway_command"
command = "list_devices"
```

sau đó gọi dispatcher.

Response dạng:

```json
[
    {
        "device_id": "light_01",
        "name": "Living Room",
        "type": "light",
        "connected": true,
        "has_ble_addr": true,
        "ble_addr": "AA:BB:CC:DD:EE:FF",
        "ble_addr_type": 0
    }
]
```

---

# 19. POST `/api/devices`

Thêm device.

Ví dụ:

```http
POST /api/devices
Content-Type: application/json
```

```json
{
    "device_id": "light_01",
    "name": "Living Room",
    "type": "light",
    "ble_addr": "AA:BB:CC:DD:EE:FF",
    "ble_addr_type": 0
}
```

Web API chuyển thành:

```text
type           = gateway_command
command        = add_device

device_id      = light_01
name           = Living Room
device_type    = light
ble_addr       = ...
```

Nếu `name` không có:

```text
name = device_id
```

Nếu `type` không có:

```text
type = generic
```

---

# 20. PUT `/api/devices`

Sửa device.

Ví dụ:

```http
PUT /api/devices
```

```json
{
    "device_id": "light_01",
    "name": "Bedroom Light"
}
```

Request được chuyển thành:

```text
gateway_command
    ↓
edit_device
```

---

# 21. DELETE `/api/devices`

Khác POST/PUT, `device_id` được truyền qua query string:

```http
DELETE /api/devices?device_id=light_01
```

Web server tạo:

```text
type      = gateway_command
command   = delete_device
device_id = light_01
```

rồi gửi tới dispatcher.

---

# 22. Gửi command tới BLE Device

Endpoint quan trọng:

```http
POST /api/command
```

Ví dụ bật thiết bị:

```json
{
    "device_id": "light_01",
    "command": "set_power",
    "bool_value": true
}
```

Hoặc:

```json
{
    "device_id": "light_01",
    "command": "set_brightness",
    "int_value": 75
}
```

Web server tạo:

```c
message.type = "device_command";
```

rồi gửi vào:

```c
command_dispatcher_handle()
```

---

# 23. Tại sao `/api/command` chạy asynchronous

Đây là một phần thiết kế rất quan trọng.

Như tài liệu `command_dispatcher` trước đó, `device_command_handle()` có thể chờ BLE ACK đến:

```text
2000 ms
```

Nếu gọi trực tiếp trong HTTP handler:

```text
HTTP Server Task
      │
      ▼
command_dispatcher_handle()
      │
      └── block ~2s
```

thì toàn bộ HTTP server có thể bị giữ.

Vì vậy `web_gateway_api.c` sử dụng:

```c
httpd_req_async_handler_begin()
```

sau đó tạo FreeRTOS task riêng:

```text
http_command
```

Luồng thực tế:

```text
HTTP POST /api/command
         │
         ▼
command_post_handler()
         │
         ├── validate JSON
         │
         ├── async_handler_begin()
         │
         ▼
Create FreeRTOS Worker
         │
         ▼
command_http_worker()
         │
         ▼
Command Dispatcher
         │
         ▼
BLE command
         │
         ▼
wait ACK
         │
         ▼
HTTP response
```

Đây là quyết định kiến trúc đúng và quan trọng.

---

# 24. Command Worker Pool

Số command worker tối đa:

```c
#define COMMAND_WORKER_COUNT 3
```

Mỗi worker có:

```c
#define COMMAND_WORKER_STACK 8192
```

Counting semaphore được tạo với:

```text
3 slots
```

Điều đó nghĩa là REST API có thể xử lý tối đa khoảng:

```text
3 /api/command đang chờ đồng thời
```

Nếu cả 3 worker đều đang bận:

```http
503 Service Unavailable
```

Response:

```json
{
    "success": false,
    "message": "All command workers are busy"
}
```

---

# 25. BLE Scan API

BLE scan được expose qua:

```text
/api/ble/scan
```

Có 3 operation:

```text
POST   start scan
GET    lấy trạng thái + kết quả
DELETE stop scan
```

---

# 26. POST `/api/ble/scan`

Request:

```http
POST /api/ble/scan
```

bắt đầu:

```c
ble_central_scan_start()
```

Response:

```json
{
    "success": true,
    "scanning": true
}
```

Scan tự động dừng sau:

```c
#define BLE_SCAN_DURATION_MS 6000
```

tức:

```text
6 giây
```

---

# 27. GET `/api/ble/scan`

Trong lúc scan hoặc sau khi scan:

```http
GET /api/ble/scan
```

Response:

```json
{
    "success": true,
    "scanning": false,
    "devices": [
        {
            "name": "BLE Sensor",
            "ble_addr": "AA:BB:CC:DD:EE:FF",
            "addr_type": 0,
            "rssi": -54
        }
    ]
}
```

Kết quả được giữ trong một cache RAM:

```c
#define BLE_SCAN_CACHE_SIZE 20
```

tức tối đa:

```text
20 BLE devices
```

---

# 28. BLE scan cache

Mỗi entry gồm:

```c
typedef struct {
    ble_scan_result_t result;
    int64_t last_seen_ms;
} scan_cache_entry_t;
```

Duplicate được xác định bằng:

```text
BLE address
+
address type
```

Nếu cùng device được phát hiện lại thì entry cũ được update thay vì thêm mới.

Cache được bảo vệ bởi:

```text
s_scan_mutex
```

---

# 29. DELETE `/api/ble/scan`

Có thể dừng scan trước 6 giây:

```http
DELETE /api/ble/scan
```

Response:

```json
{
    "success": true,
    "scanning": false
}
```

Server gọi:

```c
ble_central_scan_stop();
```

---

# 30. Wi-Fi Provisioning API

Provisioning mode cung cấp:

```text
GET  /api/wifi/scan
POST /api/wifi/scan

GET  /api/wifi
POST /api/wifi
```

---

# 31. Wi-Fi Scan

Để bắt đầu scan:

```http
POST /api/wifi/scan
```

Response:

```http
202 Accepted
```

```json
{
    "success": true,
    "scanning": true
}
```

Việc scan không chạy trong HTTP server task mà chạy trong FreeRTOS worker:

```text
wifi_scan
```

với stack:

```text
8192 bytes
```

---

# 32. Đọc kết quả Wi-Fi scan

```http
GET /api/wifi/scan
```

Ví dụ:

```json
{
    "success": true,
    "scanning": false,
    "networks": [
        {
            "ssid": "Home WiFi",
            "rssi": -55,
            "secure": true
        },
        {
            "ssid": "OpenWifi",
            "rssi": -78,
            "secure": false
        }
    ]
}
```

---

# 33. Cấu hình Wi-Fi

Request:

```http
POST /api/wifi
Content-Type: application/json
```

```json
{
    "ssid": "Home WiFi",
    "password": "example-password"
}
```

Web server không trả kết quả connection ngay lập tức.

Thay vào đó:

```text
POST /api/wifi
       │
       ▼
validate SSID/password
       │
       ▼
FreeRTOS worker
       │
       ▼
wifi_prov_test_and_save()
       │
       ├── test connection
       ├── save credentials
       └── schedule restart
```

Response ban đầu:

```http
202 Accepted
```

```json
{
    "success": true,
    "state": "connecting",
    "message": "Testing Wi-Fi credentials"
}
```

---

# 34. Kiểm tra trạng thái cấu hình Wi-Fi

Frontend poll:

```http
GET /api/wifi
```

Response có một trong các state:

```text
idle
connecting
succeeded
failed
```

Ví dụ:

```json
{
    "success": true,
    "state": "succeeded",
    "message": "Wi-Fi verified and saved; gateway is restarting"
}
```

---

# 35. Restart sau Wi-Fi provisioning

Nếu credentials hợp lệ:

```c
wifi_prov_test_and_save()
```

thành công, server schedule restart:

```c
#define WIFI_RESTART_DELAY_MS 4000
```

tức khoảng:

```text
4 giây
```

Luồng:

```text
Wi-Fi credentials valid
        │
        ▼
Save NVS
        │
        ▼
HTTP status → succeeded
        │
        ▼
wait ~4 s
        │
        ▼
ESP restart
        │
        ▼
normal Gateway Mode
```

---

# 36. Provisioning `/api/status`

Trong provisioning mode, `/api/status` đơn giản hơn gateway mode.

Response chỉ tập trung vào:

```text
IP
Wi-Fi connection
Provisioning state
Wi-Fi state
Free heap
Uptime
```

Ví dụ:

```json
{
    "ip": "192.168.4.1",
    "wifi_connected": false,
    "provisioning": true,
    "wifi_state": "provisioning",
    "free_heap": 150000,
    "uptime_ms": 12520
}
```

---

# 37. Khởi động từ `app_main()`

Trong ứng dụng chính, logic hiện tại gần như:

```text
wifi_prov_init()
       │
       ▼
Có đang provisioning?
       │
       ├── YES
       │     │
       │     ▼
       │ web_server_start_provisioning()
       │
       └── NO
             │
             ▼
        Wi-Fi connected?
             │
             ▼
        device_store_init()
             │
             ▼
        command_dispatcher_init()
             │
             ▼
        ble_central_init()
             │
             ▼
        web_server_start()
             │
             ▼
        mcp_endpoint_register(server)
```

Điều này giải thích vì sao provisioning server không có Device/BLE API đầy đủ: tại thời điểm đó các gateway module còn chưa được khởi tạo.

---

# 38. Quan hệ với MCP

Một điểm kiến trúc đáng chú ý:

```c
httpd_handle_t server = web_server_start();
```

sau đó:

```c
mcp_endpoint_register(server);
```

Do đó `/mcp` **không phải một web server khác**.

Nó nằm trên cùng HTTP server:

```text
ESP HTTP Server
      │
      ├── /
      ├── /api/status
      ├── /api/devices
      ├── /api/command
      └── /mcp
```

Public API cố ý trả `httpd_handle_t` vì lý do này.

---

# 39. Quan hệ với Command Dispatcher

Có hai hướng API khác nhau nhưng cùng backend:

```text
REST API
   │
   │ /api/devices
   │ /api/command
   ▼
Command Dispatcher
```

và:

```text
MCP JSON-RPC
      │
      ▼
Command Dispatcher
```

Do đó:

```text
REST
MCP
```

không tự implement logic device riêng.

Dispatcher trở thành business/control layer dùng chung.

Đây là một phân tách kiến trúc hợp lý.

---

# 40. Cách thêm REST endpoint mới

Giả sử muốn thêm:

```text
GET /api/info
```

Có thể tạo handler:

```c
static esp_err_t info_get_handler(httpd_req_t *request)
{
    cJSON *json = cJSON_CreateObject();

    if (json != NULL) {
        cJSON_AddStringToObject(
            json,
            "name",
            "ESP BLE Gateway"
        );
    }

    return web_send_json(request, json);
}
```

Sau đó thêm route:

```c
static const httpd_uri_t routes[] = {
    {
        .uri = "/api/info",
        .method = HTTP_GET,
        .handler = info_get_handler
    },
};
```

và:

```c
web_register_routes(
    server,
    routes,
    WEB_ARRAY_SIZE(routes)
);
```

Mô hình registration này được sử dụng xuyên suốt component.

---

# 41. Khi nào nên dùng Command Dispatcher

Nếu endpoint thực hiện business operation như:

```text
add device
delete device
send BLE command
```

nên tiếp tục route qua:

```text
Command Dispatcher
```

Ví dụ:

```text
POST /api/device/reset
          │
          ▼
gw_message_t
          │
          ▼
command_dispatcher_handle()
```

Không nên để Web Server trực tiếp xử lý protocol BLE.

---

# 42. Khi nào không cần Dispatcher

Các endpoint thuần system/read-only như:

```text
/api/status
/api/logs
/api/ble/scan
/api/wifi
```

có thể truy cập component tương ứng trực tiếp.

Code hiện tại cũng đang áp dụng mô hình này:

```text
Device operations
    → command_dispatcher

BLE scanning
    → ble_central

Wi-Fi configuration
    → wifi_provisioning

Logs
    → log_buffer
```

---

# 43. Threading model

Có ba kiểu xử lý chính.

### Request ngắn

Ví dụ:

```text
GET /api/status
GET /api/logs
GET /api/devices
```

được xử lý trực tiếp trong HTTP server task.

### Device command dài

```text
POST /api/command
```

được đưa sang worker riêng vì có thể chờ BLE ACK.

### Wi-Fi operation dài

```text
POST /api/wifi/scan
POST /api/wifi
```

cũng chạy qua FreeRTOS worker riêng.

Mô hình:

```text
HTTP Server Task
      │
      ├── quick operations
      │
      ├── command worker
      │
      ├── Wi-Fi scan worker
      │
      └── Wi-Fi config worker
```

---

# 44. Các mutex/semaphore quan trọng

Component sử dụng nhiều primitive của FreeRTOS.

Gateway commands:

```text
s_command_slots
```

là counting semaphore giới hạn số worker.

BLE scan:

```text
s_scan_mutex
```

bảo vệ scan cache/state.

Wi-Fi provisioning:

```text
s_wifi_mutex
```

bảo vệ:

```text
scan state
scan cache
configuration state
job message
```

---

# 45. Một số giới hạn hiện tại

## HTTP command concurrency

```text
3 simultaneous workers
```

do:

```c
COMMAND_WORKER_COUNT 3
```

---

## BLE scan result

```text
20 devices
```

do:

```c
BLE_SCAN_CACHE_SIZE 20
```

---

## BLE scan duration

```text
6 seconds
```

do:

```c
BLE_SCAN_DURATION_MS 6000
```

---

## Wi-Fi scan result

Số Wi-Fi network tối đa phụ thuộc:

```c
WIFI_PROV_MAX_SCAN_RESULTS
```

từ component `wifi_provisioning`.

---

# 46. Điểm kiến trúc cần chú ý

## 46.1 Dashboard HTML khá lớn

Hiện tại:

```text
dashboard.html ≈ 78 KB
dashboard.css  ≈ 24 KB
setup.html     ≈ 22 KB
```

Dashboard HTML hiện được embed thô, trong khi setup page được gzip.

Nếu flash size hoặc network latency trở thành vấn đề, một hướng tối ưu hợp lý là gzip cả:

```text
dashboard.html
dashboard.css
```

tương tự `setup.html`.

---

# 47. Static files không nằm trên SPIFFS/LittleFS

Một điểm cần hiểu rõ:

```text
www/dashboard.html
```

chỉ tồn tại như source khi build.

Sau build:

```text
HTML
CSS
Font
```

được nhúng trực tiếp vào firmware image.

Do đó runtime không làm:

```c
fopen("/spiffs/dashboard.html")
```

mà lấy binary symbol như:

```c
_binary_dashboard_html_start
_binary_dashboard_html_end
```

Điều này có ưu điểm:

```text
không cần filesystem
không cần mount SPIFFS
không có lỗi file missing runtime
deployment đơn giản
```

nhưng thay đổi giao diện yêu cầu build + flash firmware lại.

---

# 48. Security hiện tại

Provisioning page gzip đặt `Content-Security-Policy` khá chặt:

```text
default-src 'none'
connect-src 'self'
script-src 'unsafe-inline'
style-src 'unsafe-inline'
img-src data:
base-uri 'none'
form-action 'self'
frame-ancestors 'none'
```

Tuy nhiên REST API hiện tại không thể hiện authentication layer trong `web_server`.

Nghĩa là trong LAN, nếu client truy cập được ESP32 thì về kiến trúc hiện tại có thể có quyền gọi các API như:

```text
/api/restart
/api/devices
/api/command
```

Nếu gateway sau này hoạt động trên mạng không tin cậy, authentication/authorization là phần nên bổ sung.

---

# 49. Kiến trúc tổng thể

Có thể nhìn `web_server` như một adapter layer:

```text
┌─────────────────────────────┐
│        Browser / API        │
└──────────────┬──────────────┘
               │ HTTP
               ▼
┌─────────────────────────────┐
│          Web Server         │
│                             │
│ Assets                      │
│ REST API                    │
│ Provisioning API            │
└───────┬───────┬───────┬─────┘
        │       │       │
        ▼       ▼       ▼
 Command    BLE       Wi-Fi
Dispatcher Central  Provisioning
        │
        ▼
   Device Store
        │
        ▼
    BLE Devices
```

Web Server chủ yếu chịu trách nhiệm:

```text
HTTP parsing
JSON validation
HTTP response
API routing
UI asset serving
async worker management
```

Nó không nên chứa logic business sâu của BLE device.

---

# 50. Tóm tắt API

| Endpoint         | Method | Mode         | Chức năng            |
| ---------------- | ------ | ------------ | -------------------- |
| `/`              | GET    | Gateway      | Dashboard            |
| `/`              | GET    | Provisioning | Wi-Fi Setup          |
| `/api/status`    | GET    | Both         | System status        |
| `/api/logs`      | GET    | Both         | Logs                 |
| `/api/restart`   | POST   | Gateway      | Restart ESP32        |
| `/api/devices`   | GET    | Gateway      | List devices         |
| `/api/devices`   | POST   | Gateway      | Add device           |
| `/api/devices`   | PUT    | Gateway      | Edit device          |
| `/api/devices`   | DELETE | Gateway      | Delete device        |
| `/api/command`   | POST   | Gateway      | Send BLE command     |
| `/api/ble/scan`  | POST   | Gateway      | Start BLE scan       |
| `/api/ble/scan`  | GET    | Gateway      | BLE scan results     |
| `/api/ble/scan`  | DELETE | Gateway      | Stop BLE scan        |
| `/api/wifi/scan` | POST   | Provisioning | Start Wi-Fi scan     |
| `/api/wifi/scan` | GET    | Provisioning | Wi-Fi results        |
| `/api/wifi`      | POST   | Provisioning | Configure Wi-Fi      |
| `/api/wifi`      | GET    | Provisioning | Configuration status |

---

# 51. Luồng sử dụng điển hình

## Gateway đang hoạt động bình thường

```text
Browser
   │
   ▼
GET /
   │
   ▼
dashboard.html
   │
   ├── GET /api/status
   ├── GET /api/devices
   ├── GET /api/logs
   │
   ├── POST /api/ble/scan
   │
   └── POST /api/command
```

---

## Thiết bị chưa có Wi-Fi

```text
ESP32 Provisioning AP
       │
       ▼
Browser
       │
       ▼
GET /
       │
       ▼
setup.html
       │
       ├── POST /api/wifi/scan
       │
       ├── GET /api/wifi/scan
       │
       ├── POST /api/wifi
       │
       └── GET /api/wifi
                 │
                 ▼
           credentials OK
                 │
                 ▼
              restart
                 │
                 ▼
           Gateway Mode
```

---

# 52. Kết luận

`components/web_server` hiện không chỉ là một file HTML server đơn giản.

Nó thực chất là:

```text
Web UI server
+
REST API gateway
+
BLE management interface
+
Wi-Fi provisioning interface
```

và có sự phân chia khá rõ:

```text
web_server.c
    orchestration

web_assets.c
    frontend assets

web_http.c
    HTTP/JSON utilities

web_gateway_api.c
    device management
    device commands

web_ble_api.c
    BLE discovery

web_wifi_api.c
    provisioning

web_system_api.c
    system information
```

Điểm thiết kế đáng chú ý nhất là:

```text
REST /api/command
        │
        ▼
async FreeRTOS worker
        │
        ▼
Command Dispatcher
        │
        ▼
BLE Central
        │
        ▼
BLE Device
        │
        ▼
ACK
        │
        ▼
HTTP response
```

Nhờ vậy BLE ACK có thể mất tới vài giây nhưng không khóa HTTP server task.

Đồng thời `web_server_start()` trả lại `httpd_handle_t`, cho phép `/mcp` được đăng ký trên **cùng một HTTP server**, tạo ra kiến trúc thống nhất:

```text
                ESP HTTP Server
                      │
        ┌─────────────┼─────────────┐
        │             │             │
      Web UI        REST API       MCP
        │             │             │
        └─────────────┼─────────────┘
                      ▼
              Gateway Components
```

Đây là vai trò chính của `components/web_server` trong project hiện tại.
