# ESP32 BLE Gateway — Kế hoạch loại bỏ Logging / Log Feature

**Repository:** `hailp-vn38/esp-ble-gateway`  
**Ngày rà soát:** 27/08/2026  
**Mục tiêu:** loại bỏ chức năng lưu/hiển thị log khỏi firmware và Web UI, giải phóng RAM/Flash/code-path không cần thiết, đồng thời tránh làm hỏng BLE, Web API, MCP, provisioning và command pipeline.

---

## 1. Kết luận ngắn

Trong code hiện tại có **hai khái niệm khác nhau** cần phân biệt:

1. **Log feature của dự án**
   - `components/log_buffer`
   - API `/api/logs`
   - System Logs / Device Logs trên dashboard
   - RAM buffer, snapshot buffer, mutex phục vụ log
   - phần tài liệu/test liên quan

2. **ESP-IDF runtime logging**
   - `ESP_LOGE`
   - `ESP_LOGW`
   - `ESP_LOGI`
   - `ESP_LOGD`
   - `ESP_LOGV`
   - `#include "esp_log.h"`

Khuyến nghị triển khai:

- **Xóa hoàn toàn nhóm (1).**
- **Không cần xóa thủ công toàn bộ nhóm (2) trong lần refactor đầu tiên.**
- Với production firmware, có thể cấu hình ESP-IDF để giảm hoặc tắt compile-time log sau khi hệ thống ổn định.
- Các lỗi điều khiển luồng phải được giữ bằng return code/state/status, không được phụ thuộc vào log.

---

# 2. Các component phải xóa hoàn toàn

## 2.1 `components/log_buffer/`

Xóa toàn bộ thư mục:

```text
components/log_buffer/
├── CMakeLists.txt
├── Kconfig
├── include/
│   └── log_buffer.h
├── log_buffer.c
├── refactor-components-log_buffer_v2.md
└── test/
    ├── CMakeLists.txt
    └── test_log_buffer.c
```

### Lý do

Component này tồn tại chỉ để giữ log trong RAM và cung cấp snapshot cho Web API.

`CMakeLists.txt` hiện đăng ký:

```cmake
idf_component_register(
    SRCS "log_buffer.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer freertos log
)
```

Khi loại bỏ log feature thì component này không còn giá trị sử dụng.

### Việc phải làm kèm theo

Tìm và xóa:

```text
#include "log_buffer.h"
log_buffer_init(...)
log_buffer_get_recent(...)
log_buffer_*
LOG_BUFFER_*
log_entry_t
```

---

## 2.2 `components/message_trace/`

Xóa toàn bộ thư mục:

```text
components/message_trace/
├── CMakeLists.txt
├── include/
│   └── message_trace.h
├── message_trace.c
└── test/
    ├── CMakeLists.txt
    └── test_message_trace.c
```

### Nhận xét

Trong cây source hiện tại, `message_trace` là một component riêng biệt.

Cần coi toàn bộ component này là logging/trace infrastructure và loại bỏ cùng đợt.

Sau khi xóa, chạy tìm kiếm toàn repo:

```bash
rg -n "message_trace|MESSAGE_TRACE|trace_" .
```

Không được còn dependency compile-time hoặc API runtime nào trỏ về component này.

---

# 3. `main/main.c` — phải sửa

File:

```text
main/main.c
```

Hiện có:

```c
#include "esp_log.h"
...
#include "log_buffer.h"
```

và trong `app_main()`:

```c
esp_err_t log_error = log_buffer_init();
if (log_error != ESP_OK) {
    ESP_LOGW(TAG, "RAM log buffer unavailable: %s",
             esp_err_to_name(log_error));
}
```

## Bắt buộc xóa

Xóa:

```c
#include "log_buffer.h"
```

Xóa toàn bộ:

```c
esp_err_t log_error = log_buffer_init();
if (log_error != ESP_OK) {
    ESP_LOGW(TAG, "RAM log buffer unavailable: %s",
             esp_err_to_name(log_error));
}
```

Sau refactor, `app_main()` phải bắt đầu trực tiếp với initialization thực sự cần thiết, ví dụ:

```c
void app_main(void)
{
    esp_err_t io_rc = board_io_init();
    ...
}
```

## Không được thay thế log buffer bằng buffer khác

Không tạo:

```text
debug_buffer
event_buffer
runtime_log
trace_queue
history_buffer
```

nếu mục tiêu là loại bỏ hoàn toàn chức năng log.

---

# 4. `main/CMakeLists.txt` — phải sửa

File:

```text
main/CMakeLists.txt
```

Hiện có dependency:

```cmake
REQUIRES device_store device_capabilities wifi_provisioning ble_central cbor_codec
         command_dispatcher command_executor web_server mcp_endpoint
         log_buffer nvs_flash esp_wifi esp_event bt esp_http_server
         board_io
```

## Bắt buộc xóa

Xóa:

```cmake
log_buffer
```

Kết quả:

```cmake
idf_component_register(
    SRCS "main.c"
         "board_status_sync.c"
    INCLUDE_DIRS "."
    REQUIRES device_store device_capabilities wifi_provisioning ble_central cbor_codec
             command_dispatcher command_executor web_server mcp_endpoint
             nvs_flash esp_wifi esp_event bt esp_http_server
             board_io
)
```

---

# 5. `components/web_server/CMakeLists.txt` — phải sửa

File:

```text
components/web_server/CMakeLists.txt
```

Hiện có:

```cmake
REQUIRES esp_http_server cbor_codec command_dispatcher command_executor
         device_store gateway_status log_buffer wifi_provisioning
         ble_central device_capabilities espressif__cjson esp_system esp_timer freertos
         esp_wifi esp_app_format
```

## Bắt buộc xóa

Xóa:

```cmake
log_buffer
```

Sau refactor:

```cmake
REQUIRES esp_http_server cbor_codec command_dispatcher command_executor
         device_store gateway_status wifi_provisioning
         ble_central device_capabilities espressif__cjson esp_system esp_timer freertos
         esp_wifi esp_app_format
```

Sau khi sửa `web_system_api.c`, cần kiểm tra lại xem:

```text
freertos
esp_timer
```

còn thực sự cần bởi component hay không.

Không xóa dependency chỉ dựa trên tên; hãy kiểm tra toàn bộ source của `web_server`.

---

# 6. `components/web_server/web_system_api.c` — điểm refactor quan trọng nhất

File:

```text
components/web_server/web_system_api.c
```

Hiện file này chứa toàn bộ backend phục vụ System Logs.

## 6.1 Xóa include

Xóa:

```c
#include "log_buffer.h"
```

Nếu sau refactor không còn dùng semaphore ở file này, xóa thêm:

```c
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
```

Nhưng chỉ xóa nếu không còn code khác sử dụng.

---

## 6.2 Xóa các constant/global dành cho logs

Xóa:

```c
#define LOG_API_MAX_ENTRIES LOG_BUFFER_CAPACITY

static log_entry_t s_log_snapshot[LOG_API_MAX_ENTRIES];
static SemaphoreHandle_t s_log_mutex;
```

### RAM được giải phóng

Sau thay đổi này firmware không còn:

- static snapshot array cho log
- semaphore/mutex riêng cho API log
- RAM log ring buffer do `log_buffer` quản lý

---

## 6.3 Xóa `ensure_resources()` nếu chỉ phục vụ log

Hiện tại:

```c
static esp_err_t ensure_resources(void)
{
    if (s_log_mutex == NULL) s_log_mutex = xSemaphoreCreateMutex();
    return s_log_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}
```

Nếu không có tài nguyên khác dùng hàm này thì xóa toàn bộ.

---

## 6.4 Xóa `logs_get_handler()`

Xóa toàn bộ handler:

```c
static esp_err_t logs_get_handler(httpd_req_t *request)
{
    ...
}
```

Handler này hiện:

1. lock `s_log_mutex`
2. đọc `log_buffer_get_recent()`
3. tạo JSON array
4. serialize `text`
5. serialize `timestamp_ms`
6. trả response `/api/logs`

Tất cả đều không còn cần.

---

## 6.5 Xóa route `/api/logs`

### Gateway mode

Hiện tại:

```c
static const httpd_uri_t routes[] = {
    {.uri = "/api/logs", .method = HTTP_GET,
     .handler = logs_get_handler},
    {.uri = "/api/status", .method = HTTP_GET,
     .handler = status_get_handler},
    {.uri = "/api/restart", .method = HTTP_POST,
     .handler = restart_post_handler},
};
```

Sửa thành:

```c
static const httpd_uri_t routes[] = {
    {.uri = "/api/status", .method = HTTP_GET,
     .handler = status_get_handler},
    {.uri = "/api/restart", .method = HTTP_POST,
     .handler = restart_post_handler},
};
```

### Provisioning mode

Hiện tại:

```c
static const httpd_uri_t routes[] = {
    {.uri = "/api/status", .method = HTTP_GET,
     .handler = provisioning_status_get_handler},
    {.uri = "/api/logs", .method = HTTP_GET,
     .handler = logs_get_handler},
};
```

Sửa thành:

```c
static const httpd_uri_t routes[] = {
    {.uri = "/api/status", .method = HTTP_GET,
     .handler = provisioning_status_get_handler},
};
```

---

## 6.6 Bỏ `ensure_resources()` khỏi register functions

Hiện tại cả:

```c
web_system_api_register_gateway(...)
```

và:

```c
web_system_api_register_provisioning(...)
```

đều gọi:

```c
esp_err_t init_error = ensure_resources();
if (init_error != ESP_OK) return init_error;
```

Nếu `ensure_resources()` chỉ tạo log mutex thì xóa hai đoạn này.

---

# 7. `components/web_server/www/dashboard.html` — phải loại toàn bộ Log UI

Đây là file cần cleanup lớn.

```text
components/web_server/www/dashboard.html
```

---

## 7.1 Xóa CSS riêng cho log area

Đầu file đang có:

```css
/* Custom scrollbar for log area */
.log-container::-webkit-scrollbar {
    ...
}
...
```

Xóa toàn bộ CSS chỉ phục vụ:

```text
.log-container
```

Sau đó tìm lại:

```bash
rg -n "log-container" components/web_server/www
```

Nếu không còn reference thì cleanup hoàn tất.

---

## 7.2 Xóa navigation `System Logs`

Hiện dashboard có:

```html
<button onclick="nav.switchTab('logs')"
        id="nav-logs"
        class="hidden ...">
    ...
    System Logs
</button>
```

Xóa hoàn toàn node này.

Không chỉ giữ `hidden`, vì giữ lại vẫn là dead UI/dead JavaScript.

---

## 7.3 Xóa System Logs view

Xóa toàn bộ section dạng:

```html
<section id="view-logs">
    ...
    <h2>System Logs</h2>
    ...
</section>
```

Bao gồm:

```text
Clear
Export
log-content
System Logs
Live event stream
```

---

## 7.4 Xóa Device Logs panel trong Device Detail

Dashboard hiện có block:

```html
<!-- Device Logs -->
<div ...>
    ...
    <span>Device Logs</span>
    ...
    <button onclick="logger.clearDeviceLogs()">
    ...
    <div id="device-log-content" ...>
        Waiting for device data...
    </div>
</div>
```

Xóa toàn bộ panel.

### Sau khi xóa cần chỉnh layout

Hiện Device Detail dùng grid:

```text
md:grid-cols-3
```

trong đó:

- 1 cột cho device info
- 2 cột cho Device Logs

Sau khi bỏ Device Logs, nên refactor layout để phần:

```text
Status
Details
Device Commands
Capabilities
```

chiếm không gian hợp lý.

Không để:

```text
md:col-span-1
```

với hai cột còn lại trống.

---

# 8. Xóa JavaScript Logger System trong dashboard

Trong `dashboard.html` hiện có:

```javascript
// --- Logger System ---
const logger = {
    ...
};
```

## 8.1 Xóa logger object

Xóa toàn bộ:

```javascript
const logger = {
    ...
};
```

Bao gồm các hàm liên quan như:

```text
init
refresh
renderSystemLogs
renderDeviceLogs
clear
clearDeviceLogs
download
debug
info
warn
error
```

Tên chính xác cần đối chiếu source hiện tại, nhưng nguyên tắc là không để logger facade tồn tại.

---

## 8.2 Xóa `SHOW_SYSTEM_LOGS`

Tìm:

```bash
rg -n "SHOW_SYSTEM_LOGS" components/web_server/www/dashboard.html
```

Xóa:

- declaration
- condition
- UI visibility logic
- refresh logic

Ví dụ hiện logger có:

```javascript
if (SHOW_SYSTEM_LOGS) this.refresh();
```

Phải xóa cùng logger.

---

## 8.3 Xóa API client `getLogs()`

Tìm object `api` trong dashboard và xóa method kiểu:

```javascript
getLogs()
```

hoặc:

```javascript
fetch('/api/logs')
```

Tìm bằng:

```bash
rg -n "/api/logs|getLogs" components/web_server/www/dashboard.html
```

Không để browser tiếp tục request một endpoint đã bị xóa.

---

# 9. Các chỗ `logger.*` không được xóa mù quáng

Dashboard hiện dùng `logger` không chỉ để hiển thị system log mà còn như console/debug helper.

Ví dụ đã thấy:

```javascript
logger.error(`Scan refresh failed: ${error.message}`);
logger.error("Failed to start scan: " + e.message);
logger.error(`Failed to stop scan: ${error.message}`);
logger.debug(`Found new device: ${device.mac} [${device.rssi}dBm]`);
logger.error("Failed to load devices: " + e.message);
```

và:

```javascript
logger.renderDeviceLogs(dev.id);
await logger.refresh();
```

## Cách xử lý

### Trường hợp chỉ để debug

Ví dụ:

```javascript
logger.debug(...)
```

xóa statement.

### Trường hợp lỗi đã có UI feedback

Ví dụ:

```javascript
logger.error(...);
ui.showToast(...);
```

giữ:

```javascript
ui.showToast(...);
```

và xóa `logger.error()`.

### Trường hợp logger call đang ảnh hưởng flow

Ví dụ:

```javascript
await logger.refresh();
```

xóa lời gọi nhưng phải đảm bảo sequence UI/API vẫn đúng.

Không thay bằng delay giả.

---

# 10. State của frontend cần cleanup

Tìm:

```bash
rg -n "systemLogs|deviceLogs|logs" components/web_server/www/dashboard.html
```

Xóa các state field chỉ phục vụ logging, ví dụ:

```javascript
state.systemLogs
```

và các cache/filter log theo device nếu có.

Không xóa các field business như:

```text
connectedDevices
scannedDevices
selectedDeviceDetail
capabilities
```

---

# 11. Navigation/router frontend cần cleanup

Tìm logic:

```javascript
nav.switchTab('logs')
```

và các switch/case kiểu:

```javascript
case 'logs':
```

Xóa nhánh logs.

Sau refactor các tab hợp lệ chỉ nên còn các view thực sự dùng, ví dụ:

```text
devices
scanner
device-detail
settings
```

Tùy source cuối cùng.

---

# 12. `components/web_server/README.md` — cập nhật tài liệu

File:

```text
components/web_server/README.md
```

Tìm và xóa/cập nhật mọi mô tả về:

```text
/api/logs
System Logs
Device Logs
log buffer
real-time logs
recent logs
```

API contract mới không được quảng bá `/api/logs`.

---

# 13. Root `README.md` — cập nhật

File:

```text
README.md
```

Tìm các đoạn mô tả:

```text
logs
log buffer
System Logs
Device Logs
/api/logs
message trace
```

Xóa hoặc sửa để phản ánh kiến trúc mới.

---

# 14. Tài liệu `docs/` cần cập nhật

Cây repo hiện có nhiều tài liệu thiết kế cũ.

Ưu tiên rà:

```text
docs/ESP32_BLE_Gateway_Project_Framework.md
docs/ESP_BLE_Gateway_Development_Spec_v1.3.md
docs/Giai_Doan_1_Plan_ESP32_BLE_Gateway.md
docs/Ke_Hoach_Chi_Tiet_7_Module_ESP32_Gateway.md
docs/Tai_lieu_Test_ESP32_BLE_Gateway.md
docs/refactor-execution-plan.md
```

Ngoài ra:

```text
components/device_capabilities/
Device_Capabilities_Cache_Refresh_Message_Trace_Development_Spec_v1.0.md
```

có chữ `Message_Trace` ngay trong tên tài liệu và cần đánh giá lại.

## Framework hiện tại cũng cần sửa

Khung dự án cũ mô tả:

```text
Log message gần đây (hiển thị lên Web UI) | RAM (circular buffer)
```

và Web UI có:

```text
Xem log message realtime/gần thời gian thực
```

Sau quyết định mới, hai yêu cầu này phải được loại khỏi architecture spec.

---

# 15. Test phải xóa

## Xóa hoàn toàn

```text
components/log_buffer/test/
components/message_trace/test/
```

## Rà integration tests

Tìm:

```bash
rg -n "/api/logs|log_buffer|message_trace|System Logs|Device Logs" test components/*/test
```

Xóa hoặc sửa mọi assertion mong đợi:

```text
/api/logs = 200
log entries
log timestamps
log order
log rollover
trace entries
```

---

# 16. `test/run_tests.sh` và test build configuration

Sau khi xóa component cần kiểm tra:

```text
test/CMakeLists.txt
test/run_tests.sh
test/sdkconfig.defaults
```

Mục tiêu:

- không còn explicit build `log_buffer`
- không còn explicit build `message_trace`
- không còn test filter tên log/trace
- không còn Kconfig chỉ phục vụ logging

---

# 17. Kconfig của `log_buffer`

Khi xóa:

```text
components/log_buffer/Kconfig
```

các symbol do file này cung cấp sẽ biến mất.

Sau đó chạy:

```bash
rg -n "CONFIG_.*LOG_BUFFER|LOG_BUFFER_" .
```

Nếu còn symbol trong:

```text
sdkconfig
sdkconfig.defaults
test/sdkconfig.defaults
README/docs
```

thì xóa.

---

# 18. ESP-IDF `ESP_LOGx`: nên xử lý thế nào?

Đây là phần cần tách khỏi `log_buffer`.

Hiện nhiều module dùng:

```c
#include "esp_log.h"
```

và:

```c
ESP_LOGE(...)
ESP_LOGW(...)
ESP_LOGI(...)
```

Ví dụ `main/main.c` dùng log cho:

- capability discovery warning
- restart/factory reset
- board I/O initialization
- Wi-Fi initialization
- provisioning
- device store
- dispatcher
- executor
- BLE
- web/MCP startup

`device_capabilities.c` cũng dùng ESP logging cho cache/NVS/errors.

## Khuyến nghị

### Phase A — refactor architecture

Xóa:

```text
log_buffer
message_trace
/api/logs
System Logs
Device Logs
frontend logger
```

Nhưng **giữ ESP_LOGE/W trong source**.

### Phase B — production optimization

Sau khi firmware ổn định, tắt/reduce log bằng sdkconfig.

Ví dụ hướng cấu hình:

```text
CONFIG_LOG_DEFAULT_LEVEL_NONE=y
```

hoặc cấu hình tương đương với version ESP-IDF đang dùng.

Cần xác nhận symbol đúng bằng:

```bash
idf.py menuconfig
```

vì ESP-IDF version có thể thay đổi naming/behavior.

---

# 19. Không nên xóa các mechanism sau

Loại bỏ logging không đồng nghĩa loại bỏ observability/state.

Phải giữ:

```text
gateway_status
command_executor_stats
BLE connection state
Wi-Fi state
device status
capability state
return codes
HTTP error response
JSON-RPC error response
board status LED/display
```

Ví dụ `/api/status` hiện trả:

```text
device_count
connected_count
ble_link_count
wifi_connected
provisioning
wifi_state
free_heap
uptime_ms
firmware_version
idf_version
wifi_ssid
wifi_mac
wifi_rssi
executor metrics
```

Đây là **runtime status/metrics**, không phải log history.

Nên giữ.

---

# 20. Không thay log history bằng event history trá hình

Sau refactor tránh tạo những API như:

```text
/api/events
/api/history
/api/debug
/api/trace
/api/recent-messages
```

nếu chúng chỉ là tên mới cho log.

Nếu sau này cần diagnostics, nên thiết kế riêng và bật compile-time theo development build.

---

# 21. Thứ tự triển khai đề xuất

## Step 1 — loại backend log API

Sửa:

```text
components/web_server/web_system_api.c
components/web_server/CMakeLists.txt
```

Xóa:

```text
/api/logs
log_buffer dependency
log snapshot
log mutex
logs_get_handler
```

---

## Step 2 — loại frontend log

Sửa:

```text
components/web_server/www/dashboard.html
```

Xóa:

```text
System Logs navigation
System Logs view
Device Logs panel
logger object
SHOW_SYSTEM_LOGS
getLogs()
/api/logs client calls
systemLogs state
log CSS
```

---

## Step 3 — loại app initialization

Sửa:

```text
main/main.c
main/CMakeLists.txt
```

Xóa:

```text
#include "log_buffer.h"
log_buffer_init()
log_buffer dependency
```

---

## Step 4 — xóa component

Xóa:

```text
components/log_buffer/
components/message_trace/
```

---

## Step 5 — cleanup test/docs

Xóa/sửa references trong:

```text
README.md
components/web_server/README.md
docs/
test/
```

---

## Step 6 — compile

Chạy:

```bash
idf.py fullclean
idf.py reconfigure
idf.py build
```

---

## Step 7 — test

Chạy test project theo workflow hiện tại.

Tối thiểu xác nhận:

```text
Wi-Fi provisioning
Web dashboard load
/api/status
/api/restart
BLE scan
device add/remove
device connect/reconnect
capability discovery
device command
MCP endpoint
factory reset
board_io state
```

---

# 22. Search gate trước khi merge

Sau refactor chạy:

```bash
rg -n "log_buffer" .
rg -n "LOG_BUFFER" .
rg -n "log_entry_t" .
rg -n "/api/logs" .
rg -n "getLogs" .
rg -n "System Logs" .
rg -n "Device Logs" .
rg -n "SHOW_SYSTEM_LOGS" .
rg -n "message_trace" .
rg -n "MESSAGE_TRACE" .
```

Kết quả mong muốn:

```text
0 references trong production code
```

Có thể còn chữ `log` trong lịch sử/tài liệu migration nếu cố ý giữ, nhưng không được còn dependency runtime.

---

# 23. Gate bổ sung nếu muốn firmware không in ESP-IDF log

Nếu quyết định **không chỉ bỏ log feature mà muốn firmware production hoàn toàn không log**, chạy thêm:

```bash
rg -n 'ESP_LOG[A-Z]*\(' --glob '*.{c,h}' .
rg -n '#include "esp_log.h"' --glob '*.{c,h}' .
```

Có hai lựa chọn:

## Option A — khuyến nghị

Giữ source `ESP_LOGx`, nhưng compile production ở log level NONE.

Ưu điểm:

- development/debug build vẫn quan sát được lỗi
- production không tốn runtime logging như trước
- ít xâm lấn source
- dễ bật diagnostics lại

## Option B — hard remove

Xóa toàn bộ:

```text
ESP_LOGE
ESP_LOGW
ESP_LOGI
ESP_LOGD
ESP_LOGV
TAG
esp_log.h
```

Chỉ nên làm nếu bạn xác nhận muốn source hoàn toàn không có logging.

Nếu dùng Option B phải đặc biệt chú ý những đoạn:

```c
if (something_failed) {
    ESP_LOGE(...);
    return;
}
```

Chỉ được xóa `ESP_LOGE(...)`, không được xóa:

```c
if
return
cleanup
state transition
retry
esp_restart
error response
```

---

# 24. Files chắc chắn bị ảnh hưởng

## Xóa

```text
components/log_buffer/CMakeLists.txt
components/log_buffer/Kconfig
components/log_buffer/include/log_buffer.h
components/log_buffer/log_buffer.c
components/log_buffer/refactor-components-log_buffer_v2.md
components/log_buffer/test/CMakeLists.txt
components/log_buffer/test/test_log_buffer.c

components/message_trace/CMakeLists.txt
components/message_trace/include/message_trace.h
components/message_trace/message_trace.c
components/message_trace/test/CMakeLists.txt
components/message_trace/test/test_message_trace.c
```

## Sửa bắt buộc

```text
main/main.c
main/CMakeLists.txt
components/web_server/CMakeLists.txt
components/web_server/web_system_api.c
components/web_server/www/dashboard.html
components/web_server/README.md
README.md
```

## Rà và sửa theo reference

```text
test/CMakeLists.txt
test/run_tests.sh
test/sdkconfig.defaults

docs/ESP32_BLE_Gateway_Project_Framework.md
docs/ESP_BLE_Gateway_Development_Spec_v1.3.md
docs/Giai_Doan_1_Plan_ESP32_BLE_Gateway.md
docs/Ke_Hoach_Chi_Tiet_7_Module_ESP32_Gateway.md
docs/Tai_lieu_Test_ESP32_BLE_Gateway.md
docs/refactor-execution-plan.md

components/device_capabilities/
Device_Capabilities_Cache_Refresh_Message_Trace_Development_Spec_v1.0.md
```

---

# 25. Acceptance Criteria

Refactor chỉ được coi là hoàn thành khi đạt toàn bộ:

- [ ] `components/log_buffer/` không còn tồn tại.
- [ ] `components/message_trace/` không còn tồn tại.
- [ ] Không component nào `REQUIRES log_buffer`.
- [ ] Không source nào include `log_buffer.h`.
- [ ] `app_main()` không gọi `log_buffer_init()`.
- [ ] `/api/logs` không còn được register.
- [ ] Browser không request `/api/logs`.
- [ ] System Logs view bị xóa.
- [ ] Device Logs panel bị xóa.
- [ ] `logger` frontend facade bị xóa.
- [ ] Không còn `SHOW_SYSTEM_LOGS`.
- [ ] Không còn RAM snapshot phục vụ logs.
- [ ] Không còn mutex phục vụ logs.
- [ ] Unit tests của log/trace bị xóa.
- [ ] Build production thành công.
- [ ] Test BLE thành công.
- [ ] Test provisioning thành công.
- [ ] Test Web API chính thành công.
- [ ] Test MCP thành công.
- [ ] `/api/status` và runtime metrics vẫn hoạt động.
- [ ] Device command/response không phụ thuộc log.
- [ ] Không còn reference runtime đến log/trace khi chạy `rg`.
- [ ] Nếu production yêu cầu silent firmware, ESP-IDF log level được cấu hình riêng.

---

# 26. Kiến trúc sau khi loại logging

```text
ESP32-S3 Gateway
│
├── board_io
├── wifi_provisioning
├── device_store
├── ble_central
├── device_capabilities
├── cbor_codec
├── command_dispatcher
├── command_executor
├── gateway_status
├── web_server
│   ├── status API
│   ├── device API
│   ├── BLE API
│   ├── Wi-Fi API
│   └── restart/config APIs
└── mcp_endpoint
```

Không còn:

```text
log_buffer
message_trace
/api/logs
System Logs
Device Logs
RAM log history
frontend log history
```

---

# 27. Quyết định kiến trúc khuyến nghị

Đối với project này, nên định nghĩa rõ:

> Gateway không lưu lịch sử log và không cung cấp log history qua Web API. Runtime health được thể hiện qua status/state/metrics. Development diagnostics dùng ESP-IDF logging và có thể compile out ở production.

Cách này phù hợp hơn với mục tiêu tối ưu RAM/Flash và giữ firmware gateway tập trung vào:

```text
BLE routing
device state
command execution
Web/MCP transport
configuration
hardware status
```

thay vì duy trì một subsystem logging riêng.

---

## Appendix A — lệnh kiểm tra cuối

```bash
# Feature logging
rg -n "log_buffer|LOG_BUFFER|log_entry_t" .
rg -n "/api/logs|getLogs|SHOW_SYSTEM_LOGS" .
rg -n "System Logs|Device Logs|log-container" .
rg -n "message_trace|MESSAGE_TRACE" .

# Optional: ESP-IDF runtime logs
rg -n 'ESP_LOG[A-Z]*\(' --glob '*.{c,h}' .
rg -n '#include "esp_log.h"' --glob '*.{c,h}' .

# Build
idf.py fullclean
idf.py reconfigure
idf.py build
```

## Appendix B — ưu tiên thực thi

```text
P0
- web_system_api.c
- dashboard.html
- main.c
- main/CMakeLists.txt
- web_server/CMakeLists.txt
- delete log_buffer
- delete message_trace

P1
- tests
- README
- component docs
- architecture docs

P2
- production ESP-IDF log level
- binary/RAM comparison trước-sau
```
