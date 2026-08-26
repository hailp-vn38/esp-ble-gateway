# Web Server Refactor & Development Plan v2

**Component chính:** `components/web_server`  
**Components liên quan:** `command_dispatcher`, `mcp_endpoint`, `ble_central`, `device_store`, `wifi_provisioning`, `log_buffer`  
**Repository:** `hailp-vn38/esp-ble-gateway`  
**Target:** ESP32-S3 / ESP-IDF / NimBLE / `esp_http_server`  
**Trạng thái:** Implementation Specification  
**Phiên bản:** 2.0

---

# 1. Mục tiêu

Giai đoạn này không rewrite `web_server`.

Mục tiêu là ổn định execution model và boundary giữa:

```text
Transport
Application
Dispatcher
BLE
```

trước khi tiếp tục mở rộng Web API và MCP.

Các mục tiêu chính:

1. Không block HTTP server task khi chờ BLE ACK.
2. Không tạo FreeRTOS task mới theo từng HTTP command.
3. REST và MCP dùng chung một command execution path.
4. Không tồn tại hai hoặc ba command scheduler độc lập.
5. Giới hạn concurrency dựa trên:
   - RAM;
   - socket budget;
   - BLE latency;
   - số device.
6. Loại bỏ BLE scan timeout task lifetime race.
7. Làm request-body handling an toàn với HTTP keep-alive.
8. Tạo application service dùng chung cho Web và MCP.
9. Giữ memory footprint deterministic.
10. Chuẩn bị architecture cho khoảng 10 BLE devices/gateway.

---

# 2. Kiến trúc hiện tại

Luồng Web hiện tại:

```text
Browser
   │
   ▼
esp_http_server
   │
   ├── web_gateway_api
   │       │
   │       └── command_dispatcher
   │
   ├── web_system_api
   │
   ├── web_ble_api
   │
   └── web_wifi_api
```

MCP hiện chạy trên cùng HTTP server:

```text
AI Agent
   │
   ▼
POST /mcp
   │
   ▼
mcp_endpoint
   │
   ├── synchronous tools
   │
   └── mcp_async
           │
           ▼
      command_dispatcher
```

Web `/api/command` cũng có asynchronous execution riêng.

Như vậy hiện tồn tại hai execution mechanisms:

```text
Web async worker
MCP async worker
```

Đây là vấn đề kiến trúc chính cần giải quyết.

---

# 3. Target Architecture

Kiến trúc cuối cùng:

```text
                       ESP HTTP Server
                             │
               ┌─────────────┴──────────────┐
               │                            │
               ▼                            ▼
         REST Adapter                  MCP Adapter
               │                            │
               │ JSON/HTTP                  │ JSON-RPC
               │                            │
               └─────────────┬──────────────┘
                             │
                             ▼
                    Application Layer
                             │
             ┌───────────────┼────────────────┐
             │               │                │
             ▼               ▼                ▼
     Command Executor   Gateway Status   BLE Scan Service
             │
             ▼
     Command Dispatcher
             │
       ┌─────┴──────┐
       │            │
       ▼            ▼
 Gateway Command Device Command
                    │
                    ▼
                BLE Central
```

Nguyên tắc:

```text
HTTP/MCP = transport
```

không phải:

```text
HTTP/MCP = transport + scheduler + application logic
```

---

# 4. Architecture Invariants

Các invariant sau phải được duy trì sau refactor.

## 4.1 HTTP invariant

```text
HTTP server task không chờ BLE ACK.
```

## 4.2 Dispatcher invariant

```text
command_dispatcher_handle()
là synchronous API.
```

Dispatcher có thể block tối đa theo:

```c
DISPATCHER_ACK_TIMEOUT_MS
```

đối với `device_command`.

## 4.3 Executor invariant

```text
Chỉ command executor worker được phép
block trong device command ACK wait.
```

## 4.4 Device concurrency invariant

Hiện tại:

```text
một device
→ tối đa một pending command
```

Các device khác nhau có thể có command đồng thời nếu underlying BLE layer cho phép.

## 4.5 Transport invariant

REST và MCP không tự tạo command scheduler riêng.

## 4.6 Core dependency invariant

Không được tồn tại:

```text
command_dispatcher → web_server
ble_central        → web_server
device_store       → web_server
```

Dependency phải đi:

```text
transport
   ↓
application
   ↓
core
```

---

# 5. P0 — Shared Command Executor

Đây là thay đổi quan trọng nhất.

---

# 6. Vấn đề hiện tại

Web `/api/command` hiện sử dụng:

```text
HTTP request
   │
   ▼
xTaskCreate()
   │
   ▼
command_dispatcher_handle()
   │
   ▼
vTaskDelete()
```

MCP lại có:

```text
HTTP request
   │
   ▼
mcp_async queue
   │
   ▼
mcp_async worker
   │
   ▼
command_dispatcher_handle()
```

Nếu thêm một `command_executor` nhưng giữ `mcp_async` nguyên trạng:

```text
MCP
 ↓
mcp_async
 ↓
command_executor
 ↓
dispatcher
```

sẽ tạo **double queue**.

Không được triển khai kiến trúc này.

---

# 7. Target Command Path

REST:

```text
POST /api/command
        │
        ▼
web_gateway_api
        │
        ▼
command_executor_submit()
```

MCP:

```text
tools/call
    │
    ▼
mcp_endpoint
    │
    ▼
command_executor_submit()
```

Cả hai:

```text
command_executor
       │
       ▼
command_dispatcher_handle()
```

---

# 8. Xử lý `mcp_async`

Sau khi shared executor hoàn thành:

```text
mcp_async
```

không còn sở hữu command scheduling.

Có hai phương án.

## Phương án A — Xóa `mcp_async`

Ưu tiên nếu implementation cho phép MCP adapter trực tiếp:

```text
async_handler_begin
       ↓
command_executor_submit
       ↓
completion
       ↓
JSON-RPC response
       ↓
async_handler_complete
```

Đây là phương án khuyến nghị.

## Phương án B — Giữ adapter mỏng

Nếu muốn giữ module:

```text
mcp_async
```

thì module này chỉ được chứa transport-specific completion context.

Không được có:

```text
FreeRTOS command worker
command queue
command dispatcher scheduling
```

---

# 9. Command Executor API

Public interface đề xuất:

```c
typedef struct command_executor command_executor_t;

typedef void (*command_completion_fn)(
    const dispatch_result_t *result,
    void *context
);

esp_err_t command_executor_init(void);

esp_err_t command_executor_submit(
    const gw_message_t *message,
    command_completion_fn completion,
    void *context
);
```

Optional:

```c
void command_executor_get_stats(
    command_executor_stats_t *stats
);
```

---

# 10. Executor Job

Queue item không chứa `dispatch_result_t`.

Ví dụ:

```c
typedef struct {
    gw_message_t message;

    command_completion_fn completion;

    void *context;

    int64_t submitted_at_us;
    int64_t deadline_us;
} command_job_t;
```

Lý do:

```c
dispatch_result_t
```

có payload lớn.

Không cần copy result buffer vào queue.

---

# 11. Worker Result Memory

Không nên:

```c
void worker()
{
    dispatch_result_t result;
}
```

nếu điều đó làm tăng worker stack thêm khoảng 4 KB.

Thay vào đó mỗi persistent worker sở hữu result buffer riêng:

```c
typedef struct {
    TaskHandle_t task;

    dispatch_result_t result;

    uint32_t jobs_processed;
} command_worker_t;
```

Ví dụ:

```c
static command_worker_t s_workers[COMMAND_WORKER_COUNT];
```

Memory trở nên deterministic.

---

# 12. Result Lifetime Contract

Callback nhận:

```c
const dispatch_result_t *result
```

Pointer chỉ valid:

```text
trong thời gian completion callback chạy.
```

Completion callback không được:

```text
store pointer
use pointer after callback return
free pointer
```

Nếu cần giữ result lâu hơn, caller phải copy.

---

# 13. Worker Count

Không hard-code architecture document rằng:

```text
workers = 3
```

là giá trị cuối cùng.

Initial candidate có thể là:

```text
2 hoặc 3
```

nhưng phải benchmark.

Các yếu tố:

```text
BLE throughput
BLE connection count
HTTP sockets
RAM
ACK latency
dispatcher behavior
```

---

# 14. Executor Queue Length

Không mặc định:

```text
queue = 6
```

Queue phải được tính từ:

```text
HTTP socket budget
+
maximum tolerable command latency
+
RAM budget
```

Ví dụ:

```text
workers = 2
queue = 2
```

có thể tốt hơn:

```text
workers = 3
queue = 6
```

nếu socket budget nhỏ.

---

# 15. Socket Budget

Async HTTP request vẫn giữ socket mở.

Do đó:

```text
running jobs
+
queued async jobs
```

không được làm Web UI mất socket.

Phải tính ít nhất:

```text
HTTP listener
Web dashboard
status polling
logs polling
MCP
REST command
```

Executor queue size phải được benchmark cùng:

```text
CONFIG_LWIP_MAX_SOCKETS
```

và HTTP server connection limit thực tế.

---

# 16. End-to-End Deadline

`DISPATCHER_ACK_TIMEOUT_MS` chỉ giới hạn thời gian:

```text
send BLE command
→ wait ACK
```

Nó không bao gồm thời gian command nằm trong queue.

Do đó executor nên có:

```text
job deadline
```

Ví dụ:

```text
HTTP command accepted
    ↓
queue
    ↓
worker
```

Nếu job đã nằm trong queue quá lâu thì worker có thể trả:

```text
executor timeout
```

mà không gửi BLE command.

---

# 17. Executor Saturation

Hai loại busy phải phân biệt.

## Device busy

Dispatcher có thể trả:

```text
DISPATCH_STATUS_BUSY
```

khi cùng device đã có pending command.

HTTP:

```text
409 Conflict
```

## Executor saturated

Nếu command queue full:

```text
503 Service Unavailable
```

Hai trường hợp không được map chung.

---

# 18. Executor Metrics

Nên có:

```c
typedef struct {
    uint32_t submitted;
    uint32_t completed;

    uint32_t queue_full;

    uint32_t queue_timeout;

    uint32_t dispatch_timeout;

    uint32_t max_queue_depth;

    uint32_t active_workers;
} command_executor_stats_t;
```

Metrics chỉ ở RAM.

Không ghi NVS.

---

# 19. P0 — BLE Scan Timeout Refactor

Hiện BLE scan timeout được triển khai bằng một FreeRTOS task delay.

Mục tiêu:

```text
FreeRTOS timeout task
→
esp_timer one-shot
```

---

# 20. Vấn đề Task Lifetime

Hiện có:

```text
worker tự delete
HTTP handler delete worker
```

dẫn tới lifetime ownership không rõ ràng.

Refactor phải loại bỏ:

```text
s_scan_stop_task
ble_scan_stop_worker
vTaskDelete(scan timeout task)
```

---

# 21. Không chỉ thay task bằng timer

Một implementation đơn giản:

```text
start timer
stop timer
```

chưa đủ.

Có thể xảy ra:

```text
scan A
 ↓
timer A scheduled
 ↓
DELETE scan A
 ↓
POST scan B
 ↓
timer A callback chạy muộn
 ↓
scan B bị stop
```

Do đó timer phải có session/generation protection.

---

# 22. BLE Scan Generation

State đề xuất:

```c
static uint32_t s_scan_generation;

static bool s_scan_active;

static esp_timer_handle_t s_scan_timer;
```

Khi start scan:

```text
generation++
active = true
```

Timer context phải tương ứng generation hiện tại.

Nếu callback thuộc generation cũ:

```text
ignore
```

---

# 23. Alternative — Deadline Guard

Có thể sử dụng:

```c
static int64_t s_scan_deadline_us;
```

Callback chỉ stop nếu:

```text
scan đang active
AND
current_time >= expected deadline
```

Session generation vẫn rõ ràng hơn nếu logic sau này phức tạp.

---

# 24. BLE Timer Callback

Callback phải rất ngắn:

```text
validate generation
mark timeout state
request BLE scan stop
return
```

Không:

```text
parse JSON
send HTTP response
perform long locking
allocate large memory
```

---

# 25. BLE Scan State Machine

Target:

```text
IDLE
 │
 │ POST
 ▼
SCANNING
 │  \
 │   \ DELETE
 │    \
 │     ▼
 │    IDLE
 │
 │ timeout
 ▼
IDLE
```

Repeated POST khi đang scan:

```text
idempotent
```

hoặc:

```text
409
```

Phải giữ behavior hiện tại nếu UI đang phụ thuộc.

---

# 26. BLE Timer Acceptance Criteria

- Không còn scan timeout FreeRTOS task.
- Không còn TaskHandle lifetime.
- Auto-stop vẫn khoảng 6 giây.
- DELETE hoạt động trước timeout.
- POST mới sau DELETE không bị stale callback stop.
- Repeated POST không tạo timer duplicate.
- NimBLE host reset không để scan state sai.

---

# 27. P0/P1 — HTTP Receive Timeout

Hiện timeout khi:

```c
httpd_req_recv()
```

có thể được retry.

Không được có loop:

```c
if (timeout)
    continue;
```

vô hạn.

---

# 28. HTTP Receive Policy

Dùng một trong hai.

## Recommended

Absolute receive deadline:

```text
start_time
+
WEB_BODY_RECEIVE_TIMEOUT_MS
```

Mỗi timeout kiểm tra deadline.

Nếu quá:

```text
WEB_BODY_TIMEOUT
```

## Alternative

Bounded retry count:

```text
WEB_BODY_MAX_TIMEOUT_RETRIES
```

Absolute deadline dễ reasoning hơn.

---

# 29. Body Status

API đề xuất:

```c
typedef enum {
    WEB_BODY_OK = 0,

    WEB_BODY_EMPTY,

    WEB_BODY_TOO_LARGE,

    WEB_BODY_TIMEOUT,

    WEB_BODY_IO_ERROR,

    WEB_BODY_INVALID_JSON

} web_body_status_t;
```

---

# 30. JSON Parser API

Ví dụ:

```c
web_body_status_t web_parse_request_json(
    httpd_req_t *request,
    char *buffer,
    size_t capacity,
    cJSON **out_json
);
```

Hoặc chia:

```text
web_read_body()
web_parse_json()
```

nếu muốn separation rõ hơn.

---

# 31. HTTP Mapping

```text
EMPTY
→ 400 Bad Request

INVALID_JSON
→ 400 Bad Request

TOO_LARGE
→ 413 Payload Too Large

TIMEOUT
→ 408 Request Timeout

IO_ERROR
→ connection/server failure
```

---

# 32. Oversized Body + Keep-Alive

Đây là invariant quan trọng.

Nếu server reject request do:

```text
Content-Length > limit
```

mà chưa consume body:

```text
không được tiếp tục keep-alive như bình thường.
```

Nếu giữ connection:

```text
remaining body bytes
```

có thể bị đọc như request kế tiếp.

---

# 33. Oversized Request Policy

Khi reject trước khi đọc toàn bộ body:

```text
Option A:
drain body

Option B:
close connection
```

Khuyến nghị:

```text
413
+
Connection: close
```

đối với ESP32.

Đơn giản và deterministic hơn.

---

# 34. Endpoint-Specific Body Limits

Không dùng một global limit cho tất cả API.

Ví dụ:

```c
#define WEB_DEVICE_BODY_MAX_LEN  512

#define WEB_COMMAND_BODY_MAX_LEN 1024

#define WEB_WIFI_BODY_MAX_LEN    256
```

MCP giữ limit riêng.

Ví dụ hiện tại:

```text
MCP ≈ 4096 bytes
```

---

# 35. Large Request Memory

Không tăng stack buffer toàn cục:

```text
1024
→
4096
→
8192
```

cho tất cả endpoint.

Đối với body lớn:

```text
bounded heap allocation
```

Ví dụ:

```c
char *web_read_body_alloc(
    ...
);
```

Allocation luôn phải có hard maximum.

---

# 36. P1 — URI Handler Capacity

HTTP server hiện dùng fixed:

```text
max_uri_handlers
```

Gateway gần chạm limit.

Provisioning đã chạm limit.

Do đó route budget phải trở thành explicit resource.

---

# 37. Gateway Route Budget

Hiện khoảng:

```text
Assets         5
Gateway API    5
System API     3
BLE API        3
MCP            1
----------------
Total         17
```

Server hiện có:

```text
18 slots
```

Chỉ còn:

```text
1 slot
```

---

# 38. Provisioning Route Budget

Hiện khoảng:

```text
Assets        6
System API    2
Wi-Fi API     4
---------------
Total        12
```

Server hiện:

```text
12 slots
```

Không còn headroom.

---

# 39. Route Capacity Policy

Không đặt:

```text
max_uri_handlers = current route count
```

Nên có explicit headroom.

Ví dụ:

```text
Gateway:
current routes + 4

Provisioning:
current routes + 2
```

Exact value benchmark theo RAM.

---

# 40. Compile-Time Route Accounting

Có thể khai báo:

```c
#define WEB_GATEWAY_ROUTE_CAPACITY ...
#define WEB_PROVISIONING_ROUTE_CAPACITY ...
```

Không cần tạo framework phức tạp.

Quan trọng là document:

```text
current route count
configured capacity
headroom
```

---

# 41. P1 — Device Mutation Execution

GET devices có thể tiếp tục synchronous nếu benchmark xác nhận nhanh.

Các mutation:

```text
POST /api/devices
PUT /api/devices
DELETE /api/devices
```

có thể đi qua executor.

---

# 42. Vì sao mutation có thể block

Gateway command hiện có thể thực hiện:

```text
NVS write
BLE connect
bond removal
connection termination
```

Do đó latency có thể tăng trong tương lai.

HTTP task không nên phụ thuộc assumption:

```text
gateway command luôn nhanh
```

---

# 43. Recommended Policy

```text
GET
→ synchronous nếu read-only và bounded

POST
PUT
DELETE
→ executor
```

Không cần ép mọi operation thành asynchronous.

---

# 44. P1 — Gateway Status Service

`/api/status` hiện aggregate data trực tiếp.

Dispatcher cũng có:

```text
get_status
```

gateway command.

Không được tạo thêm một implementation thứ ba.

---

# 45. Single Source of Truth

Tạo:

```c
esp_err_t gateway_status_get(
    gateway_status_t *status
);
```

Sau đó:

```text
REST /api/status
        │
        ▼
gateway_status_get()
```

và:

```text
dispatcher get_status
        │
        ▼
gateway_status_get()
```

MCP nếu cần status:

```text
MCP
 │
 ▼
gateway_status_get()
```

---

# 46. Gateway Status Structure

Ví dụ:

```c
typedef struct {
    int device_count;

    int connected_count;

    int ble_link_count;

    char ip[16];

    bool wifi_connected;

    bool provisioning;

    char wifi_state[24];

    uint32_t free_heap;

    uint64_t uptime_ms;

    char firmware_version[32];

    char idf_version[32];

    char wifi_ssid[33];

    char wifi_mac[18];

    bool has_wifi_rssi;

    int wifi_rssi;

} gateway_status_t;
```

Exact size có thể chỉnh theo actual constants.

---

# 47. Không bắt buộc Status đi qua Dispatcher

REST status không cần:

```text
REST
 ↓
dispatcher
 ↓
status
```

Chỉ cần:

```text
REST
 ↓
gateway_status service
```

Dispatcher command `get_status` cũng gọi cùng service.

Điểm quan trọng là:

```text
single implementation
```

không phải:

```text
mọi API đều qua dispatcher
```

---

# 48. P1 — Error Semantics

Phải tách transport saturation và device state conflict.

---

# 49. Dispatcher Mapping

```text
DISPATCH_STATUS_OK
→ 200

INVALID_ARGUMENT
→ 400

NOT_FOUND
→ 404

BUSY
→ 409

TIMEOUT
→ 504

NOT_CONNECTED
→ 502 hoặc 409 theo API contract

TRANSPORT_ERROR
→ 502

DEVICE_ERROR
→ 502

INTERNAL_ERROR
→ 500
```

---

# 50. Executor Mapping

Executor queue full:

```text
503 Service Unavailable
```

Executor job deadline exceeded trước dispatch:

```text
503
```

hoặc:

```text
504
```

Phải chọn một semantic contract thống nhất.

Khuyến nghị:

```text
queue admission failure
→ 503

accepted nhưng deadline expired
→ 504
```

---

# 51. Machine-Readable Error

REST có thể tiến tới:

```json
{
  "success": false,
  "message": "Command executor is full",
  "error": {
    "code": "executor_busy"
  }
}
```

Giữ `message` để backward compatibility.

---

# 52. Error Codes

Đề xuất:

```text
invalid_json

invalid_request

payload_too_large

request_timeout

device_not_found

device_not_connected

device_busy

executor_busy

command_timeout

transport_error

device_error

ble_busy

wifi_busy

internal_error
```

---

# 53. P2 — Metrics

Metrics cần ưu tiên sau P0/P1.

Các metric hữu ích:

```text
http_requests
http_errors

executor_submitted
executor_completed
executor_queue_full
executor_max_queue_depth

command_timeout
command_device_busy

ble_scan_count
ble_scan_timeout

free_heap_low_water
```

---

# 54. Command Latency Metrics

Nên đo ít nhất:

```text
queue_wait_ms

dispatch_ms

total_request_ms
```

Điều này giúp xác định:

```text
worker count
queue size
```

bằng dữ liệu thực tế thay vì assumption.

---

# 55. Stack Metrics

Dùng:

```c
uxTaskGetStackHighWaterMark()
```

cho:

```text
HTTP task
executor workers
Wi-Fi workers
BLE related tasks
```

Không giữ stack 8 KB chỉ vì implementation cũ dùng 8 KB.

---

# 56. Heap Metrics

Theo dõi:

```c
esp_get_free_heap_size()
```

và:

```c
heap_caps_get_largest_free_block(...)
```

nếu phù hợp.

Test:

```text
100 commands
1000 commands
100 scans
repeated MCP calls
repeated REST commands
```

Heap không được giảm liên tục.

---

# 57. Log Snapshot

Hiện:

```text
LOG_BUFFER_CAPACITY = 64
LOG_ENTRY_MAX_LEN ≈ 192
```

Full snapshot đã lớn hơn 12 KB.

Không được chuyển thành:

```c
log_entry_t snapshot[LOG_BUFFER_CAPACITY];
```

trên HTTP stack.

---

# 58. Log Snapshot Policy

Ở phase hiện tại:

```text
giữ static snapshot
```

là acceptable.

Chỉ refactor nếu có concurrency requirement mới.

Alternative về sau:

```text
bounded heap snapshot
```

hoặc:

```text
log_buffer iterator/snapshot API
```

Đây không phải P0/P1.

---

# 59. BLE Scan Cache

BLE central chỉ forward advertisements phù hợp gateway service.

Do đó:

```text
BLE_SCAN_CACHE_SIZE = 20
```

đã tương đối phù hợp với mục tiêu:

```text
~10 devices
```

---

# 60. Cache Eviction

Eviction vẫn nên có để robustness:

```text
known address
→ update

free slot
→ append

full cache
→ replace oldest
```

Dùng:

```text
last_seen_ms
```

Nhưng đây là:

```text
P3
```

không phải blocker cho executor refactor.

---

# 61. last_seen_ms

Nếu giữ field:

```c
last_seen_ms
```

nên một trong hai:

```text
A. expose qua API
```

hoặc:

```text
B. dùng internally cho eviction/stale filtering
```

Không nên giữ field không dùng lâu dài.

---

# 62. Wi-Fi Provisioning

`web_wifi_api` hiện đã có worker riêng cho Wi-Fi workflow.

Không đưa Wi-Fi provisioning vào command executor.

Lý do:

```text
Wi-Fi provisioning
```

là workflow độc lập với:

```text
gateway/device command execution
```

Giữ separation này.

---

# 63. Wi-Fi Credential Handling

Phải tiếp tục giữ:

```text
password zeroization
```

trước khi free context.

Không log:

```text
SSID password
authorization token
raw credential
```

---

# 64. Static Assets

Không cần refactor lớn.

Giữ:

```text
embedded assets

gzip provisioning page

cache headers

CSP
```

Có thể thêm:

```http
X-Content-Type-Options: nosniff

Referrer-Policy: no-referrer
```

nếu không phá UI.

---

# 65. MCP Boundary

`web_server` không chứa:

```text
MCP protocol parsing

JSON-RPC methods

MCP authorization

MCP version handling
```

Những thứ này tiếp tục nằm trong:

```text
mcp_endpoint
```

---

# 66. MCP + Shared Executor

MCP adapter chịu trách nhiệm:

```text
parse JSON-RPC

auth

tool resolution

format MCP response
```

Command executor chịu trách nhiệm:

```text
queue

worker

deadline

dispatcher execution
```

---

# 67. MCP Completion Flow

Target:

```text
MCP HTTP request
      │
      ▼
async_handler_begin
      │
      ▼
command_executor_submit
      │
      ▼
worker
      │
      ▼
dispatcher
      │
      ▼
MCP completion callback
      │
      ├── format JSON-RPC
      ├── send response
      └── async_handler_complete
```

Không có intermediate MCP command worker.

---

# 68. REST Completion Flow

```text
REST request
      │
      ▼
async_handler_begin
      │
      ▼
command_executor_submit
      │
      ▼
worker
      │
      ▼
dispatcher
      │
      ▼
REST completion callback
      │
      ├── map status
      ├── JSON response
      └── async_handler_complete
```

REST và MCP khác nhau ở transport formatting.

Không khác nhau ở command execution.

---

# 69. Executor Shutdown

Mặc dù production firmware thường không shutdown executor, API cần predictable behavior cho:

```text
initialization failure
test
registration failure
```

Không delete queue khi worker vẫn có thể đang blocked trong dispatcher.

Shutdown sequence phải là:

```text
stop accepting jobs
      ↓
signal workers
      ↓
wait workers exit
      ↓
delete queue/resources
```

Nếu dispatcher đang chờ BLE ACK thì shutdown timeout phải tính trường hợp đó.

---

# 70. Initialization Order

Recommended startup:

```text
NVS
 ↓
Wi-Fi
 ↓
device_store
 ↓
command_dispatcher
 ↓
freeze registry
 ↓
BLE central
 ↓
command_executor
 ↓
web_server
 ↓
mcp_endpoint
```

Nếu executor init fail:

```text
không expose command endpoints như bình thường
```

---

# 71. Provisioning Mode

Provisioning không cần:

```text
command_dispatcher
BLE central
command executor
MCP
```

nếu architecture hiện tại vẫn defer gateway modules tới reboot.

Giữ provisioning lightweight.

---

# 72. P0 Implementation Sequence

## Step 1

Thiết kế `command_executor` contract.

Chưa migrate Web/MCP.

## Step 2

Implement:

```text
persistent queue
persistent workers
per-worker result buffer
statistics
```

## Step 3

Unit test executor với fake dispatcher hook nếu cần.

## Step 4

Migrate `/api/command`.

## Step 5

Stress-test REST.

## Step 6

Migrate MCP device commands.

## Step 7

Remove old:

```text
web command task creation

mcp command queue/worker
```

---

# 73. P0 BLE Sequence

Sau executor:

```text
replace scan timeout worker
→ race-safe esp_timer
```

Test riêng trước khi làm các API cleanup khác.

---

# 74. P1 HTTP Sequence

Sau BLE timer:

```text
bounded body receive
       ↓
typed body errors
       ↓
oversize connection close
       ↓
endpoint-specific limits
```

Không thay tất cả HTTP response schema cùng lúc.

---

# 75. P1 Application Service Sequence

Sau HTTP correctness:

```text
gateway_status service
       ↓
REST migration
       ↓
dispatcher get_status migration
       ↓
MCP reuse nếu cần
```

---

# 76. Tests — Command Executor

Test:

```text
submit one command

submit commands to different devices

two commands same device

queue full

worker unavailable

dispatcher timeout

completion callback

job deadline

callback ownership
```

---

# 77. Tests — Cross-Transport

Đây là test quan trọng mới.

Chạy đồng thời:

```text
REST command device A

MCP command device B
```

Cả hai phải chạy qua cùng executor.

Sau đó:

```text
REST command device A

MCP command device A
```

Một command phải nhận:

```text
device busy
```

theo invariant hiện tại.

---

# 78. Tests — Socket Pressure

Test:

```text
dashboard open

status polling active

logs polling active

MCP commands queued

REST commands queued
```

Xác nhận:

```text
dashboard vẫn responsive
```

và không xảy ra:

```text
socket starvation
```

---

# 79. Tests — BLE Scan

Test:

```text
POST start

GET during scan

automatic timeout

DELETE before timeout

POST immediately after DELETE

stale timer callback simulation

NimBLE host reset during scan
```

---

# 80. Tests — HTTP Body

Test:

```text
empty body

malformed JSON

body đúng maximum

body maximum + 1

slow request

repeated socket timeout

client disconnect

oversize body + keep-alive attempt
```

Đặc biệt xác nhận oversized body không làm request kế tiếp bị parse sai.

---

# 81. Tests — URI Capacity

Test startup phải xác nhận:

```text
all Gateway routes register

/mcp registers

all Provisioning routes register
```

Failure phải log cụ thể route gây lỗi.

---

# 82. Stress Scenario A

```text
5 BLE devices

2 command workers

Web dashboard polling

1 MCP client

1 REST client
```

Chạy command liên tục trong ít nhất vài trăm operation.

---

# 83. Stress Scenario B

```text
10 BLE devices

multiple notifications

REST + MCP concurrent commands

BLE reconnect supervisor active
```

Đo:

```text
ACK latency

queue wait

heap

stack

HTTP responsiveness
```

---

# 84. Stress Scenario C

Giả lập device không ACK.

```text
worker 1 waits 2s

worker 2 waits 2s
```

Trong lúc đó gọi:

```text
/api/status

/api/logs

MCP list_tools
```

Các endpoint không phụ thuộc command execution phải vẫn responsive.

---

# 85. Memory Acceptance Criteria

Sau:

```text
1000 command operations
```

không có monotonic heap loss.

Sau repeated REST/MCP interaction:

```text
largest free block
```

không giảm liên tục do task creation fragmentation.

---

# 86. Stack Acceptance Criteria

Mỗi persistent worker phải có đủ margin.

Target ban đầu:

```text
>= 25% stack remaining
```

sau worst-case test.

Exact threshold có thể điều chỉnh sau benchmark.

---

# 87. Command Latency Acceptance Criteria

Phải đo:

```text
queue wait
dispatcher execution
total latency
```

Không đặt hard SLA trước benchmark.

Nhưng phải phát hiện command:

```text
nằm queue quá lâu
```

và expire theo deadline.

---

# 88. Definition of Done — P0

P0 hoàn thành khi:

- `/api/command` không tạo task per request.
- MCP không còn command scheduler riêng.
- REST và MCP dùng cùng executor.
- Không double queue.
- Executor có bounded queue.
- Queue size có rationale theo socket/RAM budget.
- Device busy và executor busy được phân biệt.
- BLE scan không dùng timeout task.
- Stale BLE timer callback không thể stop scan mới.
- HTTP task không block trong BLE ACK wait.

---

# 89. Definition of Done — P1

P1 hoàn thành khi:

- HTTP receive timeout bounded.
- Oversized request trả 413.
- Oversized unread body không phá keep-alive stream.
- Request size theo endpoint.
- URI handlers có headroom.
- Device mutations có execution policy rõ ràng.
- Gateway status có một source of truth.
- Dispatcher `get_status` dùng cùng status service.
- Error mapping nhất quán.

---

# 90. Definition of Done — P2

P2 hoàn thành khi:

- Executor metrics tồn tại.
- Command latency được đo.
- Stack high-water được benchmark.
- Heap fragmentation test đạt.
- Error response có machine-readable code.
- Security headers được review.

---

# 91. Definition of Done — P3

P3 có thể gồm:

```text
BLE cache eviction

last_seen API

log snapshot service

API cleanup nhỏ

additional observability
```

Các việc này không blocker cho architecture stabilization.

---

# 92. Những việc không làm

Không rewrite toàn bộ:

```text
web_server
```

Không biến dispatcher thành HTTP-aware component.

Không đưa:

```text
JSON-RPC
```

vào `web_server`.

Không để:

```text
REST scheduler
MCP scheduler
shared scheduler
```

cùng tồn tại.

Không dùng unbounded queues.

Không dùng unbounded HTTP timeout retries.

Không tăng stack chỉ để giải quyết body/result buffers.

Không đặt full log snapshot lên HTTP stack.

Không đổi toàn bộ REST contract trong cùng refactor.

Không thêm abstraction generic nếu chưa có ít nhất hai use case thực tế.

---

# 93. Target Component Structure

Một cấu trúc khả thi:

```text
components/
│
├── web_server/
│   ├── web_server.c
│   ├── web_http.c
│   ├── web_assets.c
│   ├── web_gateway_api.c
│   ├── web_system_api.c
│   ├── web_ble_api.c
│   └── web_wifi_api.c
│
├── command_dispatcher/
│
├── command_executor/
│
├── gateway_status/
│
├── mcp_endpoint/
│
├── ble_central/
│
├── device_store/
│
├── wifi_provisioning/
│
└── log_buffer/
```

Không bắt buộc mỗi abstraction phải là một ESP-IDF component.

Có thể đặt:

```text
command_executor
```

bên trong `command_dispatcher` component nếu muốn giảm số component.

Điều quan trọng là module boundary.

---

# 94. Recommended Dependency Graph

```text
web_server ───────────────┐
                          │
mcp_endpoint ─────────────┼──► command_executor
                          │          │
                          │          ▼
                          │    command_dispatcher
                          │          │
                          │          ▼
                          │      BLE central
                          │
                          └──► gateway_status
                                    │
                         ┌──────────┼─────────┐
                         ▼          ▼         ▼
                   device_store  Wi-Fi   BLE central
```

---

# 95. Migration Safety

Mỗi phase phải giữ firmware buildable.

Không làm một refactor lớn yêu cầu:

```text
Web
MCP
Dispatcher
BLE
```

đều thay đồng thời trong một bước.

Recommended migration:

```text
new executor exists
       ↓
Web migrates
       ↓
test
       ↓
MCP migrates
       ↓
test
       ↓
old schedulers removed
```

---

# 96. Rollback Strategy

Trong development branch, nếu executor gây regression:

```text
Web có thể tạm quay lại old async worker
```

trước khi merge.

Nhưng final main branch không được giữ cả hai execution paths dưới feature flags lâu dài.

Mục tiêu cuối cùng vẫn phải là:

```text
one executor
```

---

# 97. Implementation Priority Summary

| Priority | Công việc |
|---|---|
| P0 | Shared command executor |
| P0 | Migrate Web command execution |
| P0 | Migrate/remove MCP command worker |
| P0 | Socket-aware executor limits |
| P0 | Per-worker persistent result storage |
| P0 | End-to-end command deadline |
| P0 | Race-safe BLE `esp_timer` |
| P0/P1 | Bounded HTTP recv timeout |
| P1 | 413 + unread-body connection handling |
| P1 | Endpoint request limits |
| P1 | URI handler headroom |
| P1 | Device mutation execution policy |
| P1 | Shared gateway status service |
| P1 | Correct busy/error semantics |
| P2 | Executor/runtime metrics |
| P2 | Stack and heap tuning |
| P2 | Structured REST error codes |
| P3 | BLE scan cache eviction |
| P3 | Optional log snapshot redesign |

---

# 98. Recommended Development Order

```text
1. command_executor API design
          ↓
2. command_executor implementation
          ↓
3. executor unit tests
          ↓
4. migrate REST /api/command
          ↓
5. REST stress tests
          ↓
6. migrate MCP commands
          ↓
7. remove mcp_async scheduling
          ↓
8. cross-transport stress tests
          ↓
9. BLE scan timer refactor
          ↓
10. HTTP receive/body correctness
          ↓
11. URI capacity cleanup
          ↓
12. device mutation execution
          ↓
13. gateway_status service
          ↓
14. metrics
          ↓
15. RAM/stack tuning
          ↓
16. P3 improvements
```

---

# 99. Kiến trúc cần đạt trước khi tiếp tục MCP

Trước khi thêm nhiều MCP tools hơn, phải đạt:

```text
REST ─────┐
          │
          ▼
     Command Executor
          │
          ▼
     Dispatcher
          ▲
          │
MCP ──────┘
```

Không còn:

```text
REST worker system
+
MCP worker system
```

---

# 100. Kết luận

`components/web_server` hiện không cần rewrite.

Vấn đề chính của giai đoạn tiếp theo là:

```text
execution ownership
resource budgeting
transport/application separation
```

Ba thay đổi quan trọng nhất:

```text
Web + MCP schedulers
        ↓
one shared command executor
```

```text
BLE timeout task
        ↓
race-safe esp_timer
```

```text
simple HTTP body parser
        ↓
bounded, keep-alive-safe request handling
```

Sau refactor, hệ thống phải có đặc tính:

```text
bounded memory

bounded concurrency

bounded request waiting

single execution path

transport-independent dispatcher

REST/MCP reusable application services
```

Đây là nền tảng cần hoàn thành trước khi tiếp tục mở rộng:

```text
MCP tool set

device command capabilities

multi-device concurrency

future multi-gateway architecture
```