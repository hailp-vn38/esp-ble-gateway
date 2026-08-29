# Xiaozhi MCP WebSocket Bridge — Runtime Control & Reconnect Implementation Plan v1.1

**Project:** `esp-ble-gateway`  
**Repository:** `https://github.com/hailp-vn38/esp-ble-gateway`  
**Target component:** `components/mcp_ws_bridge`  
**Related modules:** `components/web_server`, `main/main.c`, `sdkconfig.defaults`  
**Status:** Implementation-ready specification v1.1  
**Target framework:** ESP-IDF v6.1-rc1  
**Target hardware:** ESP32-S3 / 16 MB Flash / 8 MB PSRAM


## Revision v1.1

Bản v1.1 bổ sung các yêu cầu bắt buộc sau:

- tách **persisted desired state** khỏi **runtime applied state**;
- không để `mcp_ws_bridge_config_update()` tự động reload trong mọi trường hợp;
- định nghĩa rõ behavior khi chỉ thay đổi endpoint;
- thêm `runtime_enabled`;
- chuẩn hóa `restart_required = persisted_enabled != runtime_enabled`;
- chuẩn hóa error response theo `web_send_api_error_code()`;
- chỉ rõ các file Web UI source cần sửa, không sửa generated `www/dashboard.html`;
- yêu cầu staged cleanup khi `mcp_ws_bridge_init()` fail;
- bổ sung component tests riêng cho `mcp_ws_bridge`.


---

## 1. Mục tiêu

Tài liệu này mô tả kế hoạch triển khai hai thay đổi chính cho tích hợp Xiaozhi:

1. Thêm nút **Reconnect Xiaozhi** trong Web Settings để chủ động đóng kết nối WebSocket hiện tại và tạo lại phiên MCP mới.
2. Biến Xiaozhi thành một subsystem tùy chọn:
   - firmware vẫn build hỗ trợ Xiaozhi;
   - mặc định runtime **disabled**;
   - user có thể bật/tắt từ Settings;
   - thay đổi trạng thái enable/disable yêu cầu reboot;
   - nếu disabled thì không khởi tạo task, queue, RX buffer, timer và WebSocket client của `mcp_ws_bridge`.

Mục tiêu chính là giảm tài nguyên runtime khi Xiaozhi không được sử dụng, đồng thời cho phép người dùng refresh danh sách MCP tools trên Xiaozhi mà không phải reboot toàn bộ gateway.

---

# 2. Hiện trạng

## 2.1 Compile-time switch

Component hiện đã có Kconfig:

```text
CONFIG_MCP_WS_BRIDGE
```

Hiện tại default đang là:

```text
default y
```

Nếu `CONFIG_MCP_WS_BRIDGE=n`, các API của bridge được compile thành stub và trả về:

```c
ESP_ERR_NOT_SUPPORTED
```

Do đó compile-time switch này phải được hiểu là:

> Firmware có hỗ trợ Xiaozhi hay không.

Không được dùng trực tiếp switch này để biểu diễn trạng thái user bật/tắt Xiaozhi trong Web UI.

---

## 2.2 Runtime configuration

Bridge hiện lưu runtime config trong NVS:

```c
typedef struct {
    bool enabled;
    char endpoint[MCP_WS_ENDPOINT_MAX_LEN];
} mcp_ws_config_t;
```

Namespace hiện tại:

```text
mcp_ws
```

Các key chính:

```text
enabled
endpoint
```

Runtime config hiện đã cho phép bật/tắt connection và lưu Xiaozhi WebSocket endpoint.

---

## 2.3 Vấn đề lifecycle hiện tại

Trong `main/main.c`, gateway luôn gọi:

```c
mcp_ws_bridge_init();
mcp_ws_bridge_start();
```

Ngay cả khi runtime config:

```text
enabled=false
```

`mcp_ws_bridge_init()` vẫn tạo:

- mutex;
- event queue;
- RX buffer;
- reconnect timer;
- handshake timer;
- Wi-Fi/IP event handlers;
- FreeRTOS bridge task.

Default hiện tại bao gồm:

```text
CONFIG_MCP_WS_MAX_RX_MESSAGE=8192
CONFIG_MCP_WS_EVENT_QUEUE_DEPTH=8
CONFIG_MCP_WS_TASK_STACK=8192
```

Do đó Xiaozhi disabled hiện tại chỉ có nghĩa:

> Không mở WebSocket connection.

Nó chưa có nghĩa:

> Không sử dụng resource của Xiaozhi subsystem.

Đây là điểm chính cần refactor.

---

# 3. Kiến trúc đề xuất

Phải tách hai tầng cấu hình.

## 3.1 Compile-time support

```text
CONFIG_MCP_WS_BRIDGE=y
```

Ý nghĩa:

> Firmware có code hỗ trợ Xiaozhi MCP WebSocket Bridge.

Production firmware nên giữ:

```ini
CONFIG_MCP_WS_BRIDGE=y
```

để user vẫn có thể bật Xiaozhi từ Web Settings.

---

## 3.2 Runtime default

Thêm Kconfig mới:

```text
CONFIG_MCP_WS_DEFAULT_ENABLED
```

Default:

```text
n
```

Trong `sdkconfig.defaults`:

```ini
# ==== Xiaozhi MCP WebSocket Bridge ====
CONFIG_MCP_WS_BRIDGE=y
CONFIG_MCP_WS_DEFAULT_ENABLED=n
```

Ý nghĩa:

- firmware hỗ trợ Xiaozhi;
- thiết bị mới hoặc NVS chưa có config sẽ mặc định không chạy Xiaozhi.

---

# 4. Lifecycle mới

## 4.1 Boot khi Xiaozhi disabled

Luồng mong muốn:

```text
app_main
   |
   +-- init Wi-Fi
   +-- init device store
   +-- init MCP core
   +-- init BLE
   +-- start Web UI
   |
   +-- read Xiaozhi runtime config
          |
          +-- enabled=false
                 |
                 +-- KHÔNG gọi mcp_ws_bridge_init()
                 +-- KHÔNG gọi mcp_ws_bridge_start()
```

Khi disabled:

```text
bridge task          = không tồn tại
bridge event queue   = không tồn tại
bridge RX buffer     = không allocate
bridge timers        = không tạo
WebSocket client     = không tạo
Wi-Fi bridge handlers= không đăng ký
```

---

## 4.2 Boot khi Xiaozhi enabled

```text
app_main
   |
   +-- read Xiaozhi config
          |
          +-- enabled=true
                 |
                 +-- validate endpoint
                 +-- mcp_ws_bridge_init()
                 +-- mcp_ws_bridge_start()
```

Nếu endpoint chưa hợp lệ:

- không start connection;
- log warning;
- Settings vẫn phải hiển thị trạng thái cấu hình lỗi hoặc chưa hoàn chỉnh.

---

# 5. Runtime enable/disable

## 5.1 Nguyên tắc bắt buộc: Desired Config ≠ Runtime State

Phải tách rõ:

```text
Persisted desired state
        !=
Runtime applied state
```

Các giá trị cần quản lý:

```text
persisted_enabled
runtime_enabled
restart_required
```

Định nghĩa:

```text
restart_required = (persisted_enabled != runtime_enabled)
```

Ví dụ gateway boot với:

```text
persisted_enabled = false
runtime_enabled   = false
```

User bật Xiaozhi:

```text
persisted_enabled = true
runtime_enabled   = false
restart_required  = true
```

Sau reboot:

```text
persisted_enabled = true
runtime_enabled   = true
restart_required  = false
```

Không được dùng một field `enabled` duy nhất để đại diện đồng thời cho cả desired state và runtime state.

---

## 5.2 Thay đổi behavior của Settings

Endpoint:

```http
PUT /api/settings/xiaozhi
```

phải:

```text
validate
   ↓
read current persisted config
   ↓
calculate changed fields
   ↓
persist NVS
   ↓
apply runtime theo rule ở §5.3
   ↓
return current desired + runtime state
```

Không được gọi một API mà luôn:

```text
persist
+
invalidate runtime
+
reload bridge
```

cho mọi loại thay đổi.

---

## 5.3 Semantics theo loại thay đổi

### A. `enabled` thay đổi

Ví dụ:

```text
OFF -> ON
```

hoặc:

```text
ON -> OFF
```

Behavior:

```text
persist enabled
persist endpoint nếu có
KHÔNG init/deinit bridge
KHÔNG thay runtime_enabled
restart_required=true
```

Enable/disable chỉ được apply sau reboot.

---

### B. Chỉ thay đổi endpoint, `enabled` không đổi

Nếu runtime Xiaozhi đang active:

```text
runtime_enabled=true
```

thì:

```text
persist endpoint
update runtime endpoint
mcp_ws_bridge_reload()
restart_required=false
```

Mục tiêu:

> User không cần reboot chỉ để thay đổi Xiaozhi WebSocket URL.

Nếu runtime disabled:

```text
runtime_enabled=false
```

thì chỉ persist endpoint, không init bridge.

---

### C. Enable + endpoint cùng thay đổi

Ví dụ:

```text
OFF -> ON
+
new endpoint
```

Behavior:

```text
persist enabled=true
persist endpoint
KHÔNG init bridge
restart_required=true
```

Sau reboot bridge sử dụng endpoint mới.

---

### D. Disable + clear endpoint

Behavior:

```text
persist enabled=false
clear endpoint
runtime hiện tại không deinit
restart_required=true nếu runtime đang enabled
```

Sau reboot Xiaozhi subsystem không được init.

---

## 5.4 Tách Persist API khỏi Runtime Apply

Hiện `mcp_ws_bridge_config_update()` / `mcp_ws_bridge_config_set()` có thể cập nhật `s_bridge.config` và queue reload khi bridge initialized.

Bản v1.1 yêu cầu refactor theo một trong hai cách:

### Khuyến nghị

Tạo API persist-only:

```c
esp_err_t mcp_ws_bridge_config_store(const mcp_ws_config_t *config);
esp_err_t mcp_ws_bridge_config_load(mcp_ws_config_t *config);
```

và API runtime riêng:

```c
esp_err_t mcp_ws_bridge_apply_endpoint(const char *endpoint);
esp_err_t mcp_ws_bridge_reload(void);
```

### Hoặc

Giữ API hiện có nhưng bổ sung mode rõ ràng:

```c
typedef enum {
    MCP_WS_CONFIG_PERSIST_ONLY,
    MCP_WS_CONFIG_APPLY_RUNTIME,
} mcp_ws_config_apply_mode_t;
```

Không chấp nhận behavior implicit.

---

## 5.5 Vì sao cần reboot cho enable/disable

Bridge hiện chưa có lifecycle `deinit()` đầy đủ.

`stop()` không giải phóng toàn bộ:

- mutex;
- queue;
- task;
- timers;
- registered event handlers;
- RX buffer.

Do đó thay vì dynamic init/deinit phức tạp, enable/disable yêu cầu reboot để lifecycle deterministic.

Đây là lựa chọn phù hợp cho ESP32 production firmware.

---

# 6. Default runtime configuration

Khi NVS chưa tồn tại, không tham chiếu trực tiếp bool Kconfig nếu symbol có thể undefined khi `n`.

Dùng helper:

```c
static bool default_enabled(void)
{
#ifdef CONFIG_MCP_WS_DEFAULT_ENABLED
    return true;
#else
    return false;
#endif
}
```

Sau đó:

```c
config.enabled = default_enabled();
```

Nếu:

```text
CONFIG_MCP_WS_DEFAULT_ENABLED=n
```

thì factory default:

```text
xiaozhi.enabled = false
```

Endpoint có thể chưa tồn tại.

---

# 7. API đọc config khi bridge chưa init

Hiện:

```c
mcp_ws_bridge_config_get_public()
```

đã có khả năng đọc NVS khi bridge chưa initialized.

Behavior này phải được giữ.

Điều này cho phép Web Settings hoạt động ngay cả khi subsystem Xiaozhi không chạy.

---

## 7.1 Không trộn nguồn dữ liệu runtime và persisted

Không được để một API có behavior:

```text
bridge initialized
    -> đọc RAM

bridge not initialized
    -> đọc NVS
```

vì điều đó làm `restart_required` không xác định chính xác.

Phải tách rõ:

```c
mcp_ws_bridge_config_get_persisted(...)
mcp_ws_bridge_get_status(...)
```

hoặc tương đương.

`config_get_persisted()` luôn đọc desired config.

`get_status()` luôn trả runtime state.

---

## 7.2 Contract của `mcp_ws_bridge_get_status()`

Khuyến nghị:

```text
out == NULL
    -> ESP_ERR_INVALID_ARG

feature compile-out
    -> ESP_ERR_NOT_SUPPORTED

feature supported nhưng bridge chưa init
    -> ESP_ERR_INVALID_STATE

bridge initialized
    -> ESP_OK
```

Web API phải map `ESP_ERR_INVALID_STATE` + `runtime_enabled=false` thành:

```text
state="disabled"
```

chứ không thành internal error.

---

# 8. Status model mới

Hiện UI có thể trả:

```json
"state": "unavailable"
```

khi bridge chưa init.

Với lazy initialization, trạng thái này không còn đủ chính xác.

Schema bắt buộc:

```json
{
  "xiaozhi": {
    "supported": true,

    "enabled": true,
    "runtime_enabled": false,
    "restart_required": true,

    "endpoint_configured": true,
    "endpoint_display": "wss://...?...****",

    "state": "disabled"
  }
}
```

Ý nghĩa:

```text
enabled
    = desired state đã persist trong NVS

runtime_enabled
    = state đã apply tại boot

restart_required
    = enabled != runtime_enabled
```

---

## 8.1 `supported`

```text
supported=true
```

khi firmware build với:

```text
CONFIG_MCP_WS_BRIDGE=y
```

```text
supported=false
```

khi feature bị compile out.

---

## 8.2 `state`

Các state:

```text
disabled
wait_network
connecting
handshaking
connected
backoff
error
unsupported
```

Không dùng `unavailable` cho trường hợp:

```text
enabled=false
bridge chưa init
```

Trường hợp này phải trả:

```text
disabled
```

---

# 9. Restart required state

`restart_required` không được lưu riêng trong NVS.

Nó phải được tính:

```text
restart_required =
    persisted_enabled != runtime_enabled
```

Điều này tránh stale flags sau reboot/crash.

Ví dụ:

```text
boot:
persisted=false
runtime=false

user save:
persisted=true
runtime=false

=> restart_required=true
```

Sau reboot:

```text
persisted=true
runtime=true

=> restart_required=false
```

Nếu user đổi ON rồi đổi lại OFF trước reboot:

```text
persisted=false
runtime=false

=> restart_required=false
```

Case này phải có test bắt buộc.

---

# 10. Reconnect Xiaozhi API

Thêm endpoint:

```http
POST /api/settings/xiaozhi/reconnect
```

Đây là action endpoint, do đó dùng `POST`.

Không dùng `PUT` vì không phải update một resource configuration.

---

## 10.1 Luồng

```text
Browser
   |
POST /api/settings/xiaozhi/reconnect
   |
web_settings_api
   |
validate Xiaozhi runtime
   |
mcp_ws_bridge_reload()
   |
bridge task
   |
destroy current WS
   |
invalidate generation
   |
reconnect
   |
MCP initialize
   |
notifications/initialized
   |
READY
```

---

# 11. Mục đích của reconnect

Khi user:

```text
Add device
   |
Add MCP command exposure
   |
MCP tool registry thay đổi
```

Xiaozhi đang dùng session cũ có thể chưa nhận danh sách tools mới.

Thực hiện:

```text
Reconnect Xiaozhi
```

sẽ tạo MCP session mới.

Xiaozhi sau đó có thể gọi lại:

```text
tools/list
```

và nhận registry mới.

---

# 12. Dùng lại `mcp_ws_bridge_reload()`

Không cần tạo reconnect engine mới.

Component hiện đã có:

```c
esp_err_t mcp_ws_bridge_reload(void);
```

API mới chỉ cần gọi primitive này sau khi validate runtime state.

---

# 13. Validation cho reconnect API

## 13.1 Feature không hỗ trợ

Nếu:

```text
CONFIG_MCP_WS_BRIDGE=n
```

response:

```http
501 Not Implemented
```

```json
{
  "success": false,
  "message": "Xiaozhi is not supported by this firmware",
  "error": {
    "code": "xiaozhi_unsupported"
  }
}
```

---

## 13.2 Xiaozhi disabled

```http
409 Conflict
```

```json
{
  "success": false,
  "message": "Xiaozhi is disabled",
  "error": {
    "code": "xiaozhi_disabled"
  }
}
```

---

## 13.3 Restart pending

Nếu NVS đã enabled nhưng runtime vẫn disabled:

```http
409 Conflict
```

```json
{
  "success": false,
  "message": "Gateway restart is required before reconnecting Xiaozhi",
  "error": {
    "code": "restart_required"
  }
}
```

---

## 13.4 Endpoint chưa cấu hình

```http
409 Conflict
```

```json
{
  "success": false,
  "message": "Xiaozhi endpoint is not configured",
  "error": {
    "code": "xiaozhi_not_configured"
  }
}
```

---

## 13.5 Reconnect đang diễn ra

Có thể trả:

```http
409 Conflict
```

```json
{
  "success": false,
  "message": "Xiaozhi is currently connecting",
  "error": {
    "code": "xiaozhi_busy"
  }
}
```

Nếu state hiện tại:

```text
connecting
handshaking
```

---

## 13.6 Thành công

```http
200 OK
```

```json
{
  "success": true,
  "state": "connecting"
}
```

---

# 14. Web Settings UI

Đề xuất block:

```text
Xiaozhi
────────────────────────────────────

Enable Xiaozhi
[ OFF | ON ]

MCP WebSocket URL
[wss://................................]

Status
● Connected

Protocol
2024-11-05

[ Kết nối lại Xiaozhi ]

Thay đổi trạng thái bật/tắt yêu cầu khởi động lại gateway.
```

---

# 15. Behavior của toggle

Khi user đổi:

```text
OFF -> ON
```

hoặc:

```text
ON -> OFF
```

UI phải:

1. gọi `PUT /api/settings/xiaozhi`;
2. lưu config;
3. hiển thị:

```text
Restart required
```

4. không giả định subsystem đã thay đổi runtime.

---

# 16. Behavior của Reconnect button

Button chỉ active khi:

```text
supported == true
runtime_enabled == true
endpoint_configured == true
restart_required == false
state != connecting
state != handshaking
```

Server vẫn phải enforce toàn bộ rule này; UI disable chỉ là UX.

Nên disable khi state:

```text
connecting
handshaking
```

---

# 17. Naming

Tên button đề xuất:

```text
Kết nối lại Xiaozhi
```

English:

```text
Reconnect Xiaozhi
```

Không dùng:

```text
Refresh
```

vì thao tác thực tế là:

```text
terminate WebSocket session
+
establish new MCP session
```

---

# 18. File cần thay đổi

## `components/mcp_ws_bridge/Kconfig.projbuild`

Thêm:

```text
config MCP_WS_DEFAULT_ENABLED
    bool "Enable Xiaozhi by default"
    default n
    depends on MCP_WS_BRIDGE
```

---

## `sdkconfig.defaults`

Thêm:

```ini
# ==== Xiaozhi MCP WebSocket Bridge ====
CONFIG_MCP_WS_BRIDGE=y
CONFIG_MCP_WS_DEFAULT_ENABLED=n
```

---

## `components/mcp_ws_bridge/mcp_ws_bridge.c`

Cần:

- default config từ Kconfig;
- hỗ trợ đọc NVS khi bridge chưa init;
- không coi not initialized là runtime error đối với public config;
- bổ sung helper cần thiết cho boot lifecycle;
- giữ reload primitive.

---

## `components/mcp_ws_bridge/include/mcp_ws_bridge.h`

Có thể thêm:

```c
bool mcp_ws_bridge_is_supported(void);
esp_err_t mcp_ws_bridge_config_get(...);
```

hoặc một API boot-specific:

```c
esp_err_t mcp_ws_bridge_should_start(bool *out);
```

Ưu tiên giữ API nhỏ.

---

## `main/main.c`

Thay:

```c
mcp_ws_bridge_init();
mcp_ws_bridge_start();
```

bằng:

```text
read config
   |
enabled?
   |
   +-- no -> skip bridge
   |
   +-- yes
          |
          +-- init
          +-- start
```

---

## `components/web_server/web_settings_api.c`

Cần:

- update status schema;
- thêm `supported`;
- thêm `restart_required`;
- thay semantics của `PUT /api/settings/xiaozhi`;
- thêm reconnect handler;
- không tự động runtime reload khi enabled thay đổi.

---

## `components/web_server/web_server.c`

Cập nhật route budget/comment do thêm route:

```http
POST /api/settings/xiaozhi/reconnect
```

---

## Web UI source files

Chỉ sửa source modular:

```text
components/web_server/www_src/dashboard/views/settings.html
components/web_server/www_src/dashboard/js/features/settings.js
components/web_server/www_src/dashboard/js/core/i18n.js
components/web_server/www_src/dashboard/js/core/api.js
```

Không sửa trực tiếp:

```text
components/web_server/www/dashboard.html
```

vì đây là generated artifact.

Cần:

- toggle Xiaozhi;
- restart warning;
- `runtime_enabled` status;
- reconnect button;
- busy state;
- API errors;
- reuse restart API/UI hiện có;
- rebuild dashboard qua `tools/build_webui.py` / CMake pipeline.

---

# 19. Không tự reconnect khi MCP tools thay đổi ở giai đoạn này

Không nên tự động gọi reconnect mỗi khi:

```text
device added
device removed
tool exposure changed
command changed
```

vì có thể tạo nhiều connection churn liên tiếp.

Phase đầu chỉ sử dụng manual:

```text
Reconnect Xiaozhi
```

---

# 20. Mở rộng tương lai: tools dirty

Có thể bổ sung:

```text
tools_revision
```

hoặc:

```text
tools_dirty
```

Khi registry thay đổi:

```text
tools_dirty=true
```

UI hiển thị:

```text
MCP tools have changed.
Reconnect Xiaozhi to apply.
```

Nhưng chưa cần trong implementation hiện tại.

---

# 21. Phase 0 — Baseline & test preparation

## Mục tiêu

Đảm bảo behavior hiện tại được ghi nhận trước refactor.

## Checklist

- [x] Build firmware hiện tại thành công.
- [ ] Flash gateway test.
- [ ] Xác nhận Xiaozhi WS kết nối được.
- [ ] Xác nhận MCP initialize thành công.
- [ ] Xác nhận `tools/list` hoạt động.
- [ ] Xác nhận `mcp_ws_bridge_reload()` tạo lại session.
- [ ] Ghi lại free heap khi Xiaozhi enabled.
- [ ] Ghi lại free heap khi runtime `enabled=false`.
- [ ] Ghi lại số FreeRTOS tasks.
- [ ] Ghi lại largest free internal block.
- [ ] Lưu log baseline.

## Acceptance

Có số liệu baseline để so sánh sau refactor.

---

# 22. Phase 1 — Kconfig & runtime defaults

## Tasks

- [x] Thêm `CONFIG_MCP_WS_DEFAULT_ENABLED`.
- [x] Default `n`.
- [x] Giữ `CONFIG_MCP_WS_BRIDGE=y` trong production firmware.
- [x] Update `sdkconfig.defaults`.
- [x] Update comments/documentation.
- [x] Build với default config.
- [x] Build test với `CONFIG_MCP_WS_BRIDGE=n`.

## Acceptance

Factory firmware:

```text
supported=true
enabled=false
```

---

# 23. Phase 2 — Config access before bridge init

## Tasks

- [x] Kiểm tra `config_load()`.
- [x] Khi NVS không có `enabled`, dùng `CONFIG_MCP_WS_DEFAULT_ENABLED`.
- [x] Public config API hoạt động khi bridge chưa init.
- [x] Endpoint masking vẫn hoạt động.
- [x] Không expose Xiaozhi token trong logs/API.
- [ ] Unit test config missing.
- [ ] Unit test config enabled.
- [ ] Unit test invalid endpoint.

## Acceptance

Web Settings đọc được Xiaozhi config mà không cần bridge task chạy.

---

# 23.1 Init failure cleanup requirement

`mcp_ws_bridge_init()` phải dùng staged cleanup.

Nếu fail tại bất kỳ bước nào sau:

```text
mutex
queue
rx_buffer
reconnect_timer
handshake_timer
IP event handler
Wi-Fi event handler
task
```

thì tất cả resource đã tạo trước đó phải được release/unregister.

Yêu cầu:

- [x] no partial initialization leak;
- [x] `s_bridge.initialized` chỉ set `true` khi toàn bộ init thành công;
- [x] failure không ảnh hưởng BLE/Web/local MCP;
- [x] error path có thể gọi lại init sau reboot mà không để stale state.

---

# 24. Phase 3 — Lazy initialization

## Tasks

- [x] Refactor `main/main.c`.
- [x] Đọc Xiaozhi runtime config trước bridge init.
- [x] Nếu disabled: không init bridge.
- [x] Nếu enabled: init + start.
- [x] Nếu enabled nhưng endpoint invalid: fail gracefully.
- [x] Không ảnh hưởng BLE startup.
- [x] Không ảnh hưởng Web UI startup.
- [x] Không ảnh hưởng local MCP endpoint.

## Acceptance

Khi Xiaozhi disabled:

- [x] không có `mcp_ws_bridge` task;
- [x] không allocate RX buffer;
- [x] không tạo reconnect timer;
- [x] không tạo handshake timer;
- [x] không tạo WebSocket client;
- [x] không reconnect traffic.

---

# 25. Phase 4 — Settings enable/disable semantics

## Tasks

- [x] Refactor `PUT /api/settings/xiaozhi`.
- [x] Persist enabled state.
- [x] Không init bridge khi enable.
- [x] Không destroy/deinit bridge khi disable.
- [x] Detect runtime vs persisted mismatch.
- [x] Return `restart_required`.
- [x] UI hiển thị restart warning.
- [ ] Verify config survives reboot.

## Acceptance

OFF -> ON:

```text
persisted_enabled=true
runtime_enabled=false
restart_required=true
no bridge starts before reboot
```

ON -> OFF:

```text
persisted_enabled=false
runtime_enabled=true
restart_required=true
current runtime remains until reboot
```

ON -> ON + endpoint change:

```text
persist endpoint
runtime_enabled=true
reload WS immediately
restart_required=false
```

---

# 26. Phase 5 — Status API

## Tasks

- [x] Thêm `supported`.
- [x] `enabled` = persisted desired state.
- [x] Thêm `runtime_enabled`.
- [x] `restart_required = enabled != runtime_enabled`.
- [x] State disabled khi runtime không start.
- [x] State unsupported khi compile-out.
- [x] Không dùng unavailable cho normal disabled state.
- [x] Endpoint display vẫn masked.
- [x] Retry/error fields chỉ meaningful khi subsystem active.

## Acceptance

GET `/api/settings` trả state chính xác trong mọi lifecycle state.

---

# 27. Phase 6 — Reconnect API

## Tasks

- [x] Thêm `POST /api/settings/xiaozhi/reconnect`.
- [x] Validate supported.
- [x] Validate runtime enabled.
- [x] Validate endpoint.
- [x] Validate restart_required=false.
- [x] Validate state not busy.
- [x] Gọi `mcp_ws_bridge_reload()`.
- [x] Return stable API errors theo `{success,message,error:{code}}`.
- [x] Update route budget.

## Acceptance

Reconnect:

```text
existing WS
   ↓
closed
   ↓
new WebSocket
   ↓
initialize
   ↓
notifications/initialized
   ↓
READY
```

---

# 28. Phase 7 — Web UI

## Tasks

- [x] Add Xiaozhi enable toggle.
- [x] Add endpoint field.
- [x] Add status badge.
- [x] Add protocol version.
- [x] Add restart-required warning.
- [x] Add Reconnect button.
- [x] Disable reconnect while busy.
- [x] Display API errors.
- [x] Confirm reconnect action visually.
- [ ] Test mobile layout.

## Acceptance

User có thể quản lý Xiaozhi hoàn toàn từ Settings.

---

# 28.1 Component test structure bắt buộc

Tạo:

```text
components/mcp_ws_bridge/test/
    CMakeLists.txt
    test_mcp_ws_bridge_config.c
    test_mcp_ws_bridge_lifecycle.c
```

Các test tối thiểu:

- [ ] NVS missing + `DEFAULT_ENABLED=n`.
- [ ] persisted disabled.
- [ ] persisted enabled + endpoint.
- [ ] persist enable không thay runtime state.
- [ ] ON -> OFF trước reboot.
- [ ] OFF -> ON -> OFF trước reboot => `restart_required=false`.
- [ ] endpoint-only update khi runtime enabled.
- [ ] endpoint-only update khi runtime disabled.
- [ ] reload while READY.
- [ ] reload while CONNECTING.
- [ ] compile-out behavior.
- [ ] partial init failure cleanup.
- [ ] public status khi bridge chưa init.

---

# 29. Phase 8 — Functional integration tests

## Scenario A — Fresh gateway

- [ ] Flash clean firmware.
- [ ] NVS không có Xiaozhi config.
- [ ] Xiaozhi disabled.
- [ ] Bridge task không tồn tại.
- [ ] Web UI hoạt động bình thường.

---

## Scenario B — Enable Xiaozhi

- [ ] Input endpoint.
- [ ] Enable Xiaozhi.
- [ ] API trả restart_required=true.
- [ ] Bridge không start trước reboot.
- [ ] Restart gateway.
- [ ] WS connection được tạo.
- [ ] MCP handshake READY.

---

## Scenario C — Disable Xiaozhi

- [ ] Disable từ Settings.
- [ ] restart_required=true.
- [ ] Restart.
- [ ] Không có bridge task.
- [ ] Không có Xiaozhi network traffic.

---

## Scenario D — Tool refresh

- [ ] Xiaozhi connected.
- [ ] Add device.
- [ ] Add MCP exposure.
- [ ] Click Reconnect Xiaozhi.
- [ ] Session cũ đóng.
- [ ] Session mới READY.
- [ ] Xiaozhi thấy tools mới.

---

## Scenario E — Network loss

- [ ] Disconnect Wi-Fi.
- [ ] Bridge chuyển wait/backoff phù hợp.
- [ ] Wi-Fi reconnect.
- [ ] WS reconnect.
- [ ] MCP READY.

---

## Scenario F — Invalid endpoint

- [ ] Submit invalid URL.
- [ ] API reject.
- [ ] NVS không corrupt.
- [ ] Bridge không crash.

---

## Scenario G — Reconnect spam

- [ ] Click reconnect nhiều lần.
- [ ] Không crash.
- [ ] Không leak WebSocket client.
- [ ] Không duplicate bridge session.
- [ ] API trả busy/conflict khi phù hợp.

---

# 30. Phase 9 — Resource validation

## Compare

Đo hai trạng thái:

```text
Xiaozhi enabled
Xiaozhi disabled
```

## Metrics

- [ ] `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)`
- [ ] largest internal block
- [ ] PSRAM free size
- [ ] FreeRTOS task count
- [ ] bridge stack usage
- [ ] Wi-Fi stability
- [ ] BLE connection stability
- [ ] Web UI latency

## Acceptance

Xiaozhi disabled phải giảm resource consumption rõ rệt so với behavior cũ.

---

# 31. Regression checklist

- [ ] BLE central vẫn hoạt động.
- [ ] Device discovery vẫn hoạt động.
- [ ] MCP local endpoint vẫn hoạt động.
- [ ] MCP token settings không bị ảnh hưởng.
- [ ] Device command execution không bị ảnh hưởng.
- [ ] Tool exposure không bị ảnh hưởng.
- [ ] Wi-Fi provisioning không bị ảnh hưởng.
- [ ] OTA không bị ảnh hưởng.
- [ ] Web authentication không bị ảnh hưởng.
- [ ] Web route count không vượt giới hạn.
- [ ] Không log full Xiaozhi endpoint/token.
- [ ] Không có memory leak sau nhiều reconnect.

---

# 32. Security checklist

Xiaozhi endpoint có thể chứa token trong query string.

Do đó:

- [ ] không log full endpoint;
- [ ] không gửi full endpoint trong GET Settings;
- [ ] tiếp tục dùng `endpoint_display`;
- [ ] zeroize request body sau khi xử lý;
- [ ] không ghi token vào error logs;
- [ ] WebSocket library logs vẫn phải suppress nếu có nguy cơ expose URI.

---

# 33. Error codes đề xuất

| Code | HTTP | Ý nghĩa |
|---|---:|---|
| `invalid_request` | 400 | Payload sai |
| `invalid_endpoint` | 400 | Endpoint không hợp lệ |
| `xiaozhi_unsupported` | 501 | Firmware không build support |
| `xiaozhi_disabled` | 409 | Runtime Xiaozhi disabled |
| `restart_required` | 409 | Config mới chưa apply |
| `xiaozhi_not_configured` | 409 | Endpoint chưa có |
| `xiaozhi_busy` | 409 | Đang connecting/handshaking |
| `internal_error` | 500 | Internal failure |

---

# 34. API contract cuối cùng

## GET `/api/settings`

```json
{
  "success": true,
  "xiaozhi": {
    "supported": true,
    "enabled": true,
    "runtime_enabled": true,
    "restart_required": false,
    "endpoint_configured": true,
    "endpoint_display": "wss://example/...?...****",
    "state": "connected",
    "retry_count": 0,
    "last_error": 0,
    "last_http_status": 0,
    "last_ws_close_code": 0,
    "protocol_version": "2024-11-05"
  }
}
```

---

## PUT `/api/settings/xiaozhi`

Request:

```json
{
  "enabled": true,
  "endpoint": "wss://..."
}
```

Response:

```json
{
  "success": true,
  "restart_required": true
}
```

---

## PUT `/api/settings/xiaozhi` — endpoint-only update while runtime enabled

Request:

```json
{
  "endpoint": "wss://new-endpoint..."
}
```

Behavior:

```text
persist endpoint
apply endpoint to runtime config
reload WS
```

Response:

```json
{
  "success": true,
  "restart_required": false,
  "xiaozhi": {
    "enabled": true,
    "runtime_enabled": true,
    "state": "connecting"
  }
}
```

---

## POST `/api/settings/xiaozhi/reconnect`

Response:

```json
{
  "success": true,
  "state": "connecting"
}
```

---

# 35. Recommended implementation order

Thứ tự phát triển:

```text
Phase 0
Baseline

Phase 1
Kconfig defaults

Phase 2
Config without init

Phase 3
Lazy boot lifecycle

Phase 4
Restart-required semantics

Phase 5
Status schema

Phase 6
Reconnect API

Phase 7
Web UI

Phase 8
Integration tests

Phase 9
Resource validation
```

Không nên bắt đầu từ UI trước khi boot lifecycle hoàn chỉnh.

---

# 36. Definition of Done

Feature chỉ được coi hoàn tất khi toàn bộ điều kiện sau đạt:

- [x] Firmware production vẫn build Xiaozhi support.
- [x] Xiaozhi mặc định runtime OFF.
- [x] Xiaozhi OFF không tạo bridge task.
- [x] Xiaozhi OFF không allocate RX buffer.
- [x] Xiaozhi OFF không tạo WebSocket connection.
- [x] User bật Xiaozhi từ Settings.
- [x] Enable chỉ persist desired state và yêu cầu reboot.
- [x] User disable Xiaozhi từ Settings.
- [x] Disable chỉ persist desired state và yêu cầu reboot.
- [x] `enabled` và `runtime_enabled` được báo cáo riêng.
- [x] Endpoint-only update khi runtime active reconnect được mà không reboot.
- [ ] Sau reboot trạng thái runtime khớp NVS.
- [x] Reconnect API hoạt động.
- [ ] Reconnect tạo MCP session mới.
- [ ] Xiaozhi cập nhật tools sau reconnect.
- [x] Web UI hiển thị state chính xác.
- [x] Không expose endpoint token.
- [ ] Không memory leak sau reconnect lặp lại.
- [x] Không resource leak khi `mcp_ws_bridge_init()` fail giữa chừng.
- [ ] BLE gateway không regression.
- [ ] Local MCP endpoint không regression.
- [ ] Web server không vượt URI handler budget.
- [ ] Resource usage khi Xiaozhi disabled thấp hơn baseline cũ.

---

# 37. Kết luận

Kiến trúc đề xuất cuối cùng:

```text
CONFIG_MCP_WS_BRIDGE=y
        |
        +-- firmware supports Xiaozhi

CONFIG_MCP_WS_DEFAULT_ENABLED=n
        |
        +-- runtime factory default OFF

Persisted enabled=false
Runtime enabled=false
        |
        +-- boot skips bridge initialization
        +-- no WS task/buffer/timers

User enables Xiaozhi
        |
        +-- save NVS
        +-- restart_required=true

Gateway reboot
        |
        +-- enabled=true
        +-- init bridge
        +-- connect Xiaozhi

MCP tool registry changed
        |
        +-- user clicks "Reconnect Xiaozhi"
        |
POST /api/settings/xiaozhi/reconnect
        |
        +-- mcp_ws_bridge_reload()
        |
        +-- new WS session
        +-- new MCP initialize
        +-- Xiaozhi receives current tools
```

Đây là phương án cân bằng tốt giữa:

- hiệu suất ESP32-S3;
- độ đơn giản lifecycle;
- khả năng bật/tắt feature từ Web UI;
- khả năng cập nhật MCP tools mà không reboot;
- giảm nguy cơ memory leak do dynamic subsystem teardown;
- giữ compatibility với kiến trúc `mcp_ws_bridge` hiện tại.
