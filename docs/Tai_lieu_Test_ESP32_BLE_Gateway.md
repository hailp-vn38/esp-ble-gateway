# Tài liệu Test --- ESP32 BLE Gateway (Giai đoạn 1)

**Phiên bản:** 2.1\
**Cập nhật:** 26/08/2026 (đã đối chiếu với mã nguồn)\
**Target:** ESP32-S3\
**Framework:** ESP-IDF native + NimBLE + Unity\
**Mục tiêu:** Chuẩn hóa chiến lược kiểm thử từ unit test đến end-to-end,
stress/soak và hardware-in-the-loop cho ESP32 BLE Gateway.

> Ghi chú phiên bản 2.1: toàn bộ mục "trạng thái hiện tại" đã được rà
> lại trực tiếp trên mã nguồn (`components/*/test/`,
> `test/CMakeLists.txt`, `sdkconfig.defaults`, endpoint thật của
> `web_server`). Các bảng đề xuất được đánh dấu ✅ (đã có test tương
> ứng) hoặc ⬜ (cần bổ sung).

------------------------------------------------------------------------

## 1. Mục tiêu kiểm thử

Hệ thống test phải chứng minh được không chỉ từng component hoạt động
đúng, mà toàn bộ luồng Gateway hoạt động ổn định:

``` text
Web UI / MCP
      |
      v
 HTTP / JSON
      |
      v
Command Dispatcher
      |
      +---- Gateway Command
      |
      +---- Device Command
               |
               v
          CBOR Codec
               |
               v
          BLE Central
               |
               v
       BLE Peripheral
               |
          Notify / ACK
               |
               v
        Gateway response
```

Các mục tiêu chính:

1.  Phát hiện lỗi logic sớm bằng unit test.
2.  Kiểm tra boundary/error handling của parser, registry, NVS và
    buffer.
3.  Kiểm tra interaction giữa các component.
4.  Kiểm tra BLE/Wi-Fi trên phần cứng thật.
5.  Kiểm tra end-to-end từ HTTP/MCP đến thiết bị BLE.
6.  Kiểm tra recovery sau disconnect/reboot.
7.  Kiểm tra độ ổn định với nhiều thiết bị BLE.
8.  Theo dõi memory leak, watchdog, deadlock và degradation dài hạn.

------------------------------------------------------------------------

## 2. Trạng thái test hiện tại của repository

Đã đối chiếu ngày 26/08/2026. Repository hiện có **77 test case đã
wiring vào test app**, cộng thêm **23 test case Wi-Fi DNS chưa wiring**.

### 2.1 Test component đã có

``` text
components/cbor_codec/test/
└── test_cbor_codec.c              (5 test case)

components/command_dispatcher/test/
├── test_command_dispatcher.c      (20 test case)
└── test_device_request_manager.c  (10 test case — ACK correlation)

components/device_store/test/
└── test_device_store.c            (5 test case)

components/log_buffer/test/
└── test_log_buffer.c              (3 test case)

components/mcp_endpoint/test/
├── test_mcp_endpoint.c            (24 test case)
├── test_mcp_conformance.c         (5 test case — wire mode 2026-07-28)
├── test_mcp_stress.c              (5 test case — heap stability)
└── test_mcp_transport.h           (mock transport nội bộ, không cần HTTP server thật)
```

### 2.2 Test tồn tại nhưng CHƯA wiring vào test app

``` text
components/wifi_provisioning/test/
├── test_dns_parser.c              (16 test case)
└── test_dns_lifecycle.c           (7 test case)
```

Component `wifi_provisioning` đã có unit test cho phần DNS hijack
(parser + lifecycle), nhưng thư mục này **chưa được thêm vào**
`TEST_COMPONENTS` trong `test/CMakeLists.txt` nên chưa được biên dịch
và chạy. Việc wiring là việc nhỏ, cần làm sớm.

### 2.3 Chưa có test

``` text
ble_central    — khoảng trống lớn nhất (xem mục 10)
web_server     — chưa có test component (xem mục 13)
wifi_provisioning — riêng phần provisioning/radio (DNS đã có, xem mục 12)
```

### 2.4 Test application

``` text
test/
├── CMakeLists.txt
├── sdkconfig.defaults
└── main/
    ├── CMakeLists.txt
    ├── idf_component.yml
    └── test_main.c
```

`test/main/test_main.c` gọi `unity_run_all_tests()` lúc boot (tự chạy
toàn bộ một lần) sau đó rơi vào `unity_run_menu()` để chọn test thủ
công qua UART.

`test/CMakeLists.txt` hiện cấu hình:

``` cmake
set(TEST_COMPONENTS
    "device_store;cbor_codec;command_dispatcher;log_buffer;mcp_endpoint"
    CACHE STRING
    "Gateway components whose unit tests are included")
```

Lưu ý vận hành:

-   Dispatcher có seam test chính thức:
    `device_command_set_hooks()` (inject mock `send_command` /
    `is_connected`), `command_dispatcher_on_device_notify()` (mô phỏng
    notify/ACK), `command_dispatcher_reset_for_test()` (reset state
    giữa các test). Đây là API nội bộ, không dùng trong production.
-   MCP endpoint test dùng mock transport nội bộ (`test_mcp_transport.h`)
    nên không cần HTTP server thật khi chạy Unity.
-   Hai project firmware (repo root) và test app (`test/`) là hai ESP-IDF
    project độc lập, mỗi bên cần `idf.py set-target esp32s3` riêng.

------------------------------------------------------------------------

## 3. Mô hình test

Test được chia thành 5 tầng.

  Level   Loại test               Mục tiêu                   Môi trường
  ------- ----------------------- -------------------------- -----------------
  L0      Build / Static          Compile, warning, config   CI/PC
  L1      Unit Test               Logic từng component       ESP32-S3 / host
  L2      Component Integration   Interaction giữa module    ESP32-S3
  L3      System / E2E            HTTP/MCP → BLE → ACK       2+ board
  L4      Stress / Soak / RF      Độ ổn định thực tế         nhiều board

Không được xem firmware Unity test PASS (chạy tự động lúc boot) là đủ
để release firmware.

------------------------------------------------------------------------

# 4. L0 --- Build và Static Check

## 4.1 Production build

``` bash
idf.py set-target esp32s3
idf.py build
```

Điều kiện PASS:

-   Không compile error.
-   Không linker error.
-   Không thiếu component/dependency.
-   Firmware production build thành công.

Lưu ý: cả hai project dùng `MINIMAL_BUILD ON` — component mới chỉ được
biên dịch nếu nằm trong graph dependency của `main`. Một component build
được nhưng không được link thường do thiếu `REQUIRES` trong CMakeLists
của nó.

## 4.2 Test firmware build

``` bash
cd test
idf.py set-target esp32s3
idf.py build
```

Điều kiện PASS:

-   Test application build thành công.
-   Tất cả test component được link đúng.

## 4.3 Compiler warnings

Khuyến nghị CI kiểm tra nghiêm:

``` text
-Wall
-Wextra
-Werror
```

Ít nhất áp dụng cho code do dự án quản lý.

------------------------------------------------------------------------

# 5. L1 --- Unit Test

## 5.1 CBOR Codec

**File hiện tại**

``` text
components/cbor_codec/test/test_cbor_codec.c
```

### Test hiện có

-   Encode/decode giữ nguyên mọi message field (kể cả `request_id`).
-   Optional device fields bị bỏ qua khi absent.
-   JSON conversion validate và giữ nguyên giá trị.
-   Reject malformed CBOR và unsupported protocol version (gộp một test).

### Ma trận test cần bổ sung / trạng thái

  ID         Test                            Trạng thái
  ---------- ------------------------------- ------------
  CBOR-001   encode/decode round trip        ✅
  CBOR-002   optional `device_id` absent     ✅
  CBOR-003   optional BLE address absent     ✅
  CBOR-004   JSON → message → JSON           ✅
  CBOR-005   malformed CBOR                  ✅
  CBOR-006   truncated CBOR                  ⬜
  CBOR-007   missing required field          ⬜
  CBOR-008   wrong CBOR value type           ⬜
  CBOR-009   unsupported protocol version    ✅
  CBOR-010   output buffer too small         ⬜
  CBOR-011   maximum `device_id` length      ⬜
  CBOR-012   maximum command length          ⬜
  CBOR-013   invalid BLE MAC                 ⬜
  CBOR-014   JSON null                       ⬜
  CBOR-015   JSON array thay vì object       ⬜
  CBOR-016   integer boundary values         ⬜

Schema wire format nằm tại `components/cbor_codec/cbor_codec.c`
(protocol version 1, map key dạng số).

### Coverage mục tiêu

``` text
>= 90%
```

CBOR codec cũng là ứng viên tốt cho fuzz testing.

------------------------------------------------------------------------

# 6. Device Store

**File hiện tại**

``` text
components/device_store/test/test_device_store.c
```

### Test hiện có

-   Add/find, reject duplicate.
-   Edit/delete có persistence.
-   BLE address persist qua re-init.
-   Compaction sau delete.
-   MAC device ID được migrate theo thứ tự address NimBLE.

`DEVICE_STORE_MAX_DEVICES = 16` (`device_store.h`).

### Ma trận test cần bổ sung / trạng thái

  ID          Test                                  Trạng thái
  ----------- ------------------------------------- ------------
  STORE-001   add/find                              ✅
  STORE-002   duplicate device                      ✅
  STORE-003   edit                                  ✅
  STORE-004   delete                                ✅
  STORE-005   BLE address persistence               ✅
  STORE-006   compaction                            ✅
  STORE-007   fill `DEVICE_STORE_MAX_DEVICES` (16)  ⬜
  STORE-008   add khi đầy                           ⬜
  STORE-009   delete device không tồn tại           ⬜
  STORE-010   edit device không tồn tại             ⬜
  STORE-011   empty device ID                       ⬜
  STORE-012   maximum ID/name/type length           ⬜
  STORE-013   repeated init                         ✅ (qua re-init)
  STORE-014   snapshot buffer nhỏ hơn store         ⬜
  STORE-015   missing NVS key                       ⬜
  STORE-016   corrupted/partial NVS data            ⬜
  STORE-017   persistence sau reboot                ✅ (re-init; power-cycle thật xem dưới)

### Hardware persistence test

Thực hiện:

``` text
write device
    ↓
restart/reboot (power-cycle thật)
    ↓
device_store_init()
    ↓
read device
```

Điều kiện PASS:

-   Dữ liệu vẫn chính xác.
-   Không duplicate entry.
-   Không mất BLE address.

### Coverage mục tiêu

``` text
>= 90%
```

------------------------------------------------------------------------

# 7. Command Dispatcher

**File hiện tại**

``` text
components/command_dispatcher/test/test_command_dispatcher.c       (20 test)
components/command_dispatcher/test/test_device_request_manager.c   (10 test)
```

### Test hiện có

-   Init single-shot, reset-for-test, freeze registry.
-   Register default commands, reject invalid/duplicate registration.
-   Reject null message, unsupported protocol version/message type,
    device command thiếu id/command.
-   Unknown gateway command → not found.
-   `list_devices` trả JSON format; text/json result truncate payload
    an toàn.
-   Device command hoàn tất khi ACK khớp; timeout nhả slot; stale ACK
    không hoàn tất request mới; ACK sai thiết bị/sai command bị từ chối;
    hai thiết bị correlate độc lập; command thứ hai trên cùng thiết bị
    → busy; send failure nhả slot; malformed ACK bị reject.

ACK timeout hiện tại: `DISPATCHER_ACK_TIMEOUT_MS = 2000 ms`
(`command_dispatcher.h`), khớp với `CONFIG_HTTPD_RECV_TIMEOUT_SEC=5`.

### Ma trận test cần bổ sung / trạng thái

  ID         Test                               Trạng thái
  ---------- ---------------------------------- ------------
  DISP-001   init                               ✅ (single-shot)
  DISP-002   register command                   ✅
  DISP-003   duplicate command                  ✅
  DISP-004   registry full                      ⬜
  DISP-005   unknown gateway command            ✅
  DISP-006   unsupported message type           ✅
  DISP-007   unsupported protocol version       ✅
  DISP-008   device command thiếu device ID     ✅
  DISP-009   nonexistent device                 ⬜
  DISP-010   disconnected device                ⬜ (mock hiện luôn connected)
  DISP-011   BLE send success                   ✅ (mock capture wire)
  DISP-012   BLE send failure                   ✅ (nhả slot)
  DISP-013   ACK success                        ✅
  DISP-014   ACK timeout                        ✅
  DISP-015   wrong-device ACK                   ✅ (case 3 request manager)
  DISP-016   late/stale ACK                     ✅ (case 6 + stale request_id)
  DISP-017   duplicate ACK                      ⬜
  DISP-018   concurrent dispatcher calls        ✅ (busy per-device)
  DISP-019   repeated init                      ✅ (reset_for_test)

## 7.1 Dependency injection cho BLE — ĐÃ CÓ SẴN

Dispatcher **đã tách** NimBLE ra khỏi unit test bằng hook injection
(`command_dispatcher_internal.h`, mặc định trỏ vào BLE thật trong
production):

``` c
typedef struct {
    int (*send_command)(const char *device_id, const gw_message_t *msg);
    int (*is_connected)(const char *device_id);
} device_command_hooks_t;

void device_command_set_hooks(const device_command_hooks_t *hooks);
void command_dispatcher_on_device_notify(const char *device_id, const gw_message_t *msg);
void command_dispatcher_reset_for_test(void);   // test-only
```

Unit test hiện tại đã inject mock (`mock_send`, `mock_is_connected`) và
kiểm tra: packet gửi (capture wire), số lần gửi, error propagation
(send failure), ACK matching/timeout/stale. Các test còn thiếu ở bảng
trên chỉ cần viết thêm case dùng đúng seam này — không cần refactor
thêm interface.

### Coverage mục tiêu

``` text
>= 90%
```

------------------------------------------------------------------------

# 8. Log Buffer

**File hiện tại**

``` text
components/log_buffer/test/test_log_buffer.c
```

### Test hiện có

-   Push 30 entry rồi đọc `log_buffer_get_recent(…, 5)` trả đúng cửa sổ
    mới nhất theo thứ tự (entry-25 … entry-29) — bao phủ gián tiếp
    wrap-around và ordering.
-   Validate limit (`limit=0`, buffer NULL) → lỗi.
-   Capture log thật của ESP-IDF qua `ESP_LOGI`.

### Ma trận test cần bổ sung / trạng thái

  ID        Test                          Trạng thái
  --------- ----------------------------- --------------------------
  LOG-001   empty buffer                  ⬜
  LOG-002   one entry                     ⬜
  LOG-003   exactly capacity              ⬜
  LOG-004   capacity + 1                  ⬜ (30 push hiện chưa chạm biên)
  LOG-005   oldest entry overwritten      ⬜ (chưa assert trực tiếp)
  LOG-006   repeated wrap-around          ⬜ (chỉ bao phủ một phần)
  LOG-007   long message                  ⬜
  LOG-008   reset/init                    ⬜
  LOG-009   ordering                      ✅
  LOG-010   concurrent writer/readers     ⬜

Cần kiểm tra concurrency vì BLE callback và HTTP/Web UI có thể cùng truy
cập log.

------------------------------------------------------------------------

# 9. MCP Endpoint

**File hiện tại**

``` text
components/mcp_endpoint/test/
├── test_mcp_endpoint.c      (24 test)
├── test_mcp_conformance.c   (5 test — conformance matrix wire mode)
├── test_mcp_stress.c        (5 test — heap stability loop)
└── test_mcp_transport.h     (mock transport, không cần httpd thật)
```

### Đặc tả thực tế của endpoint

-   Methods hỗ trợ: `tools/list` (alias cũ `list_tools`),
    `tools/call` (alias cũ `call_tool`), `server/discover`.
-   Wire mode "2026-07-28": client gửi header protocol version; version
    không hỗ trợ → error `-32022` kèm HTTP 400. Chế độ legacy vẫn chạy
    khi không có header.
-   Error codes: `-32700` Parse, `-32600` Invalid Request, `-32601`
    Method not found, `-32602` Invalid params, `-32022` version.
-   Transport guard đã có: oversize body → 413 + Connection close;
    content-type sai → 415; host ngoài allowlist → 403 (bỏ qua port/
    hoa-thường); cross-origin → forbidden; rate limiter burst → 429;
    queue đầy → 503 không drop socket; recv timeout retry giới hạn 3 lần.
-   Notification không có id → 204 No Content. Id string/number/null
    được echo lại.

### Ma trận test / trạng thái

  ID        Test                                 Trạng thái
  --------- ------------------------------------ ---------------------------
  MCP-001   valid JSON-RPC                       ✅
  MCP-002   invalid JSON                         ✅ (-32700)
  MCP-003   wrong `jsonrpc` version              ✅ (-32600)
  MCP-004   missing method                       ⬜
  MCP-005   unknown method                       ✅ (-32601)
  MCP-006   missing params                       ⬜ (chỉ có missing command)
  MCP-007   invalid params                       ⬜ (một phần qua -32602)
  MCP-008   numeric ID                           ✅
  MCP-009   string ID                            ✅
  MCP-010   notification                         ✅ (204)
  MCP-011   `list_tools` / `tools/list`          ✅ (cả hai alias)
  MCP-012   valid `call_tool`                    ✅ (+ legacy `params.command`)
  MCP-013   dispatcher failure                   ✅ (tool error, không phải protocol error)
  MCP-014   oversized request                    ✅ (413 + Connection close)
  MCP-015   truncated HTTP body                  ✅ (peer error mid-body)
  MCP-016   `server/discover` identity/caps      ✅ (conformance)
  MCP-017   wire mode 2026-07-28 header          ✅ (conformance)
  MCP-018   unsupported version -32022           ✅ (conformance)
  MCP-019   host allowlist / CORS / rate limit   ✅ (403 / 429)
  MCP-020   queue full → 503, recv timeout bound ✅ (stress)
  MCP-021   heap ổn định qua vòng lặp tools/call ⬜ mở rộng dần

Các JSON-RPC error code chuẩn được verify trong test:

``` text
-32700 Parse error
-32600 Invalid Request
-32601 Method not found
-32602 Invalid params
-32603 Internal error
(-32022 Protocol version not supported — extension của gateway)
```

Endpoint expose tại `POST /mcp`, không auth — chỉ dùng trong LAN.

------------------------------------------------------------------------

# 10. BLE Central

Hiện đây là khoảng trống test quan trọng nhất.

Trạng thái mã nguồn: `components/ble_central/` đã tách module thành
`ble_central_gap.c`, `ble_central_gatt.c`, `ble_central_scan.c`,
`ble_central_state.c`, `ble_central_supervisor.c` — thuận lợi cho việc
test từng phần, nhưng **chưa có adapter layer**: các file gọi thẳng API
NimBLE (`nimble_port_init`, GAP/GATT callbacks). Chưa tồn tại các hàm
`ble_port_*`.

Cần tạo:

``` text
components/ble_central/test/
├── CMakeLists.txt
└── test_ble_central.c
```

và bổ sung vào `TEST_COMPONENTS`.

## 10.1 BLE logic tests

Không yêu cầu peripheral thật nếu NimBLE được đặt sau adapter/mock
interface.

  ID        Test                        Trạng thái
  --------- --------------------------- ------------
  BLE-001   INIT → SCANNING             ⬜
  BLE-002   SCANNING → CONNECTING       ⬜
  BLE-003   CONNECTING → CONNECTED      ⬜
  BLE-004   disconnect updates state    ⬜
  BLE-005   reconnect scheduled         ⬜
  BLE-006   duplicate discovery ignored ⬜
  BLE-007   unknown device ignored      ⬜
  BLE-008   max connection limit (= 9)  ⬜
  BLE-009   connection table add/remove ⬜
  BLE-010   invalid connection handle   ⬜
  BLE-011   retry counter               ⬜
  BLE-012   reconnect backoff           ⬜
  BLE-013   supervisor start/stop       ⬜
  BLE-014   GATT write success          ⬜
  BLE-015   GATT write failure          ⬜
  BLE-016   notify dispatch             ⬜

Giới hạn kết nối đồng thời: `CONFIG_BT_NIMBLE_MAX_CONNECTIONS=9`
(`sdkconfig.defaults`); `BLE_CENTRAL_MAX_CONN` lấy từ config này. Khi
đổi target/chip phải kiểm tra lại.

## 10.2 NimBLE adapter

Cần refactor để cô lập các API sau đây (hiện gọi trực tiếp trong
component):

``` text
scan start/stop
connect / disconnect
GATT write / subscribe notify
port init/deinit
```

Production adapter gọi NimBLE thật.

Unit test adapter sử dụng mock.

------------------------------------------------------------------------

# 11. BLE Hardware Integration

Contract giao thức với peripheral (cứng, simulator phải tuân theo):

``` text
Service UUID : 0xABF0
Char write   : 0xABF1 (gateway → peripheral, CBOR protocol version 1)
Char notify  : 0xABF2 (peripheral → gateway, CBOR protocol version 1)
```

Cần tối thiểu:

``` text
Board A: ESP32-S3 Gateway DUT
Board B: BLE Peripheral Simulator
```

Peripheral simulator nên cung cấp:

-   Service UUID cố định `0xABF0`.
-   RX characteristic `0xABF1`.
-   TX notify characteristic `0xABF2`.
-   Echo command.
-   ACK (map key CBOR đúng schema, protocol version 1).
-   Sequence number / `request_id` echo.
-   Configurable ACK delay.
-   Configurable disconnect.

### Test cases

  ID           Test                        Trạng thái
  ------------ --------------------------- ------------
  BLE-HW-001   scan discovers peripheral   ⬜
  BLE-HW-002   connect                     ⬜
  BLE-HW-003   service discovery           ⬜
  BLE-HW-004   subscribe notify            ⬜
  BLE-HW-005   write command               ⬜
  BLE-HW-006   receive ACK                 ⬜
  BLE-HW-007   peripheral disconnect       ⬜
  BLE-HW-008   automatic reconnect         ⬜
  BLE-HW-009   peripheral reboot           ⬜
  BLE-HW-010   gateway reboot              ⬜
  BLE-HW-011   malformed packet            ⬜
  BLE-HW-012   delayed ACK (> 2000 ms)     ⬜
  BLE-HW-013   missing ACK                 ⬜

### Recovery scenario bắt buộc

``` text
gateway running
peripheral OFF
gateway scans
peripheral ON
gateway discovers
gateway connects
```

và:

``` text
connected
power-cycle peripheral
gateway detects disconnect
gateway reconnects
```

Không được yêu cầu reboot gateway để recover.

------------------------------------------------------------------------

# 12. Wi-Fi Provisioning

### Trạng thái hiện tại

`components/wifi_provisioning/test/` đã có **23 test case** cho phần
DNS hijack:

``` text
test_dns_parser.c     (16 test) — parse/generate DNS packet, query A/AAAA
test_dns_lifecycle.c  ( 7 test) — lifecycle captive DNS
```

Việc cần làm ngay: thêm `wifi_provisioning` vào `TEST_COMPONENTS` trong
`test/CMakeLists.txt` (kiểm tra dependency `esp_netif`, `lwip` đã khai
báo trong CMakeLists của test dir) để các test này thực sự chạy.

### Phần chưa có test

Logic provisioning STA/AP (`wifi_prov.c`) — radio test bắt buộc chạy
trên phần cứng thật:

  ID         Test                             Trạng thái
  ---------- -------------------------------- ------------
  WIFI-001   empty NVS starts SoftAP          ⬜ (HW)
  WIFI-002   saved credentials starts STA     ⬜ (HW)
  WIFI-003   valid credentials connect        ⬜ (HW)
  WIFI-004   wrong password                   ⬜ (HW)
  WIFI-005   AP unavailable                   ⬜ (HW)
  WIFI-006   retry                            ⬜ (HW)
  WIFI-007   retry exhausted                  ⬜ (HW)
  WIFI-008   fallback to SoftAP               ⬜ (HW)
  WIFI-009   credentials survive reboot       ⬜ (HW)
  WIFI-010   erase credentials                ⬜ (HW)
  WIFI-011   STA disconnect                   ⬜ (HW)
  WIFI-012   AP reboot/recovery               ⬜ (HW)

Lưu ý kiến trúc: chế độ provisioning (không có credential hợp lệ) chỉ
khởi tạo NVS/Wi-Fi APSTA/captive DNS/HTTP config routes; Device Store,
Dispatcher, BLE Central, reconnect supervisor, MCP endpoint chỉ init sau
khi STA có IP. Test provisioning phải cover cả hai mode boot này.

------------------------------------------------------------------------

# 13. Web Server

Không nên mock sâu `httpd_req_t` cho toàn bộ API.

Ưu tiên chạy HTTP server thật trên Gateway và kiểm tra từ pytest.

### Endpoint thật của Gateway (đã rà soát mã nguồn)

``` text
GET    /                    dashboard/setup HTML (embedded, gzipped)
GET    /api/status          gateway status (có field "wifi_connected")
GET    /api/devices         list devices
POST   /api/devices         add device (tự động connect BLE sau khi thêm)
PUT    /api/devices         edit device
DELETE /api/devices         delete device
POST   /api/command         send command đến thiết bị
POST   /api/ble/scan        start scan (timeout worker nội bộ)
GET    /api/ble/scan        kết quả scan cache
DELETE /api/ble/scan        stop scan
GET    /api/logs            logs gần nhất từ log_buffer
GET    /api/wifi            trạng thái/credential Wi-Fi
POST   /api/wifi            submit credentials (provisioning)
GET    /api/wifi/scan       scan AP xung quanh
POST   /api/restart         reboot gateway
+ các route captive portal: /generate_204, /connecttest.txt,
  /hotspot-detect.html, /ncsi.txt, /favicon.ico, assets
```

Lưu ý: **không có endpoint "BLE connect" riêng** — kết nối diễn ra tự
động sau khi device được đăng ký qua `POST /api/devices` (reconnect
supervisor phụ trách). Contract test tương ứng phải xác minh hành vi tự
kết nối này chứ không gọi một API connect.

### Contract tests

  ID         Endpoint/Test                                        Trạng thái
  ---------- ---------------------------------------------------- ------------
  HTTP-001   GET `/api/status` (có `wifi_connected`)              ⬜
  HTTP-002   GET `/api/devices`                                   ⬜
  HTTP-003   POST `/api/devices` (add)                            ⬜
  HTTP-004   PUT `/api/devices` (edit)                            ⬜
  HTTP-005   DELETE `/api/devices`                                ⬜
  HTTP-006   POST/GET/DELETE `/api/ble/scan`                      ⬜
  HTTP-007   auto-connect sau khi add device                      ⬜
  HTTP-008   POST `/api/command`                                  ⬜
  HTTP-009   GET `/api/logs`                                      ⬜
  HTTP-010   POST `/mcp`                                          ⬜
  HTTP-011   invalid JSON                                         ⬜
  HTTP-012   wrong HTTP method                                    ⬜
  HTTP-013   unknown endpoint                                     ⬜
  HTTP-014   oversized body                                       ⬜
  HTTP-015   POST `/api/wifi` provisioning flow                   ⬜
  HTTP-016   GET `/api/wifi/scan`                                 ⬜
  HTTP-017   POST `/api/restart` (gateway quay lại online)        ⬜

Ví dụ pytest (khớp contract thật):

``` python
def test_status(gateway):
    response = requests.get(gateway + "/api/status")
    assert response.status_code == 200

    data = response.json()
    assert "wifi_connected" in data
```

Chú ý: web assets (`dashboard.html`, `setup.html`, CSS, font) được
embed vào firmware qua `EMBED_FILES`; sửa asset cần rebuild + reflash
đầy đủ trước khi chạy contract test.

------------------------------------------------------------------------

# 14. L2 --- Component Integration

Các integration tests cần xác minh interaction giữa:

``` text
device_store + dispatcher
dispatcher + cbor_codec
dispatcher + ble_central
mcp_endpoint + dispatcher
web_server + dispatcher
BLE notify + dispatcher + log_buffer
```

Ví dụ:

``` text
Device Command
    ↓
Dispatcher
    ↓
CBOR encode
    ↓
mock BLE transport
```

Test phải xác nhận payload cuối cùng, không chỉ return code.

Dispatcher hiện đã có sẵn seam cho integration test kiểu này:
`device_command_hooks_t` cho phép thay transport bằng mock capture
payload CBOR thật (encode qua `cbor_codec` trước khi so sánh).

------------------------------------------------------------------------

# 15. L3 --- End-to-End

Đây là test quan trọng nhất để xác nhận Gateway hoạt động đúng.

## E2E-001 --- HTTP → BLE → ACK

``` text
HTTP POST /api/command
    ↓
Web Server
    ↓
Command Dispatcher
    ↓
CBOR
    ↓
BLE Central
    ↓
Peripheral Simulator
    ↓
ACK Notify (0xABF2)
    ↓
Dispatcher
    ↓
HTTP Response
```

Điều kiện PASS:

-   HTTP request hợp lệ.
-   Peripheral nhận đúng command.
-   Device ID đúng.
-   Payload đúng (decode CBOR phía peripheral).
-   ACK đúng (`request_id` khớp).
-   Gateway trả success.

## E2E-002 --- MCP → BLE → ACK

``` text
JSON-RPC call_tool (hoặc tools/call)
    ↓
MCP Endpoint (POST /mcp)
    ↓
Dispatcher
    ↓
BLE
    ↓
Peripheral
    ↓
ACK
    ↓
JSON-RPC result
```

## E2E-003 --- Gateway Command

``` text
HTTP/MCP
    ↓
Dispatcher
    ↓
Gateway handler
```

Không được gửi BLE packet.

------------------------------------------------------------------------

# 16. Wi-Fi End-to-End

Flow:

``` text
fresh flash
    ↓
SoftAP (provisioning mode)
    ↓
submit Wi-Fi credentials (POST /api/wifi)
    ↓
STA connect
    ↓
DHCP IP
    ↓
Web Server accessible
```

Test phải xác nhận:

1.  Gateway khởi động SoftAP khi chưa có credentials.
2.  Credentials được lưu.
3.  Gateway chuyển STA.
4.  Gateway lấy IP.
5.  Web server truy cập được.
6.  Credentials tồn tại sau reboot.
7.  Sau khi STA có IP, toàn bộ module runtime (Device Store, Dispatcher,
    BLE Central, MCP) được khởi tạo.

------------------------------------------------------------------------

# 17. Pytest Integration Framework

Khuyến nghị:

``` text
tests/
├── integration/
│   ├── test_http.py
│   ├── test_mcp.py
│   ├── test_wifi.py
│   └── test_ble.py
├── stress/
│   ├── test_ble_scale.py
│   ├── test_command_load.py
│   └── test_soak.py
└── fixtures/
    └── peripheral_simulator/
```

Pytest chịu trách nhiệm:

-   flash DUT nếu cần;
-   đọc UART;
-   tìm IP;
-   gửi HTTP request;
-   điều khiển peripheral simulator;
-   kiểm tra response;
-   thu metrics.

------------------------------------------------------------------------

# 18. L4 --- Stress Test

## 18.1 Connection scale

⚠️ Giới hạn phần cứng/config hiện tại: `CONFIG_BT_NIMBLE_MAX_CONNECTIONS
= 9` nên **tối đa 9 kết nối BLE đồng thời**. Registry cho phép nhiều hơn
(`DEVICE_STORE_MAX_DEVICES = 16`) — các thiết bị vượt quota sẽ ở trạng
thại chờ kết nối.

Test theo các mức:

``` text
1 device
2 devices
5 devices
8 devices
9 devices   ← mức tối đa với config hiện tại
```

(Nếu muốn test mức 10+, phải nâng `CONFIG_BT_NIMBLE_MAX_CONNECTIONS`
trong `sdkconfig.defaults` và kiểm tra lại RAM/partition.)

Thu metrics:

``` text
connection attempts
connection successes
connection failures
unexpected disconnects
reconnects
free heap
minimum free heap
task stack watermark
```

## 18.2 Command load

Ví dụ:

``` text
9 devices
1 command/device/second
```

Sau đó tăng tải:

``` text
2 commands/sec
5 commands/sec
10 commands/sec
```

Ràng buộc thời gian: ACK timeout 2000 ms/command — tải cao có thể đẩy
latency p99 sát hoặc vượt ngưỡng này, cần ghi nhận tỷ lệ timeout thay vì
coi là bug ngay.

Đo:

``` text
sent
acked
failed
timeout
latency p50
latency p95
latency p99
```

Không chỉ sử dụng average latency.

------------------------------------------------------------------------

# 19. Soak Test

Release candidate nên chạy tối thiểu:

``` text
8–9 BLE devices (giới hạn NimBLE hiện tại)
24 hours
```

Trong thời gian test:

-   periodic commands;
-   periodic notifications;
-   Web UI polling;
-   MCP requests;
-   random peripheral disconnect;
-   peripheral reboot;
-   Wi-Fi traffic.

Điều kiện PASS:

``` text
no panic
no watchdog reset
no deadlock
no permanent BLE failure
no continuous heap degradation
all disconnected devices eventually recover
```

Khi dự án ổn định hơn có thể nâng lên:

``` text
48–72 hours
```

------------------------------------------------------------------------

# 20. RF Test

Do gateway vận hành trong môi trường có tường và enclosure, cần test:

### RF-A

``` text
same room
line of sight
```

### RF-B

``` text
one wall
```

### RF-C

``` text
peripheral inside target enclosure
```

Thu:

``` text
RSSI
disconnect/hour
reconnect time
ACK timeout rate
p95 latency
```

Kết quả RF phải được lưu theo firmware version để so sánh regression.

------------------------------------------------------------------------

# 21. Fault Injection

Các lỗi cần chủ động tạo:

  ID          Fault                                  Trạng thái liên quan
  ----------- -------------------------------------- ------------------------
  FAULT-001   peripheral disappears during command   ⬜ (DISP-010/014 có nền)
  FAULT-002   peripheral reboot                      ⬜
  FAULT-003   delayed ACK                            ⬜ (timeout đã test unit)
  FAULT-004   duplicate ACK                          ⬜ (DISP-017)
  FAULT-005   malformed BLE notify                   ✅ unit (malformed ACK/CBOR); ⬜ HW
  FAULT-006   Wi-Fi disconnect                       ⬜
  FAULT-007   AP unavailable                         ⬜
  FAULT-008   HTTP client disconnect                 ✅ unit (peer mid-body); ⬜ E2E
  FAULT-009   malformed JSON                         ✅ unit (MCP)
  FAULT-010   corrupted/missing NVS                  ⬜ (STORE-015/016)
  FAULT-011   concurrent HTTP requests               ✅ một phần (queue full 503); ⬜ E2E
  FAULT-012   repeated BLE scan/start/stop           ⬜

Điều kiện chung:

> Fault không được làm Gateway panic, deadlock hoặc mất khả năng tự phục
> hồi ngoài các trường hợp lỗi không thể phục hồi đã được định nghĩa rõ.

------------------------------------------------------------------------

# 22. Host Tests và Sanitizer

Các module logic thuần C nên cân nhắc build trên host:

``` text
cbor_codec        ← khả thi cao (QCBOR là C thuần, portable)
log_buffer        ← cần mock/thay thế FreeRTOS API nếu có dùng
registry helpers  ← khả thi
parser helpers    ← khả thi
device_store      ← khó: phụ thuộc NVS, cần abstraction layer trước
```

Compiler flags:

``` text
-fsanitize=address,undefined
-fno-omit-frame-pointer
```

Mục tiêu phát hiện:

-   buffer overflow;
-   out-of-bounds;
-   use-after-free;
-   undefined behavior;
-   invalid integer operations.

Ước lượng công sức: `cbor_codec` + parser là ứng viên host-test đầu tiên;
`device_store` cần bọc NVS behind interface trước khi mang lên host.

------------------------------------------------------------------------

# 23. CI

CI software chạy trên mọi push/pull request.

**Trạng thái hiện tại:** repository chưa có `.github/workflows/` — toàn
bộ pipeline dưới đây cần tạo mới.

Pipeline đề xuất:

``` text
checkout
   ↓
git submodule update --init --recursive   (QCBOR là submodule — bắt buộc)
   ↓
setup ESP-IDF (5.4.x)
   ↓
idf.py set-target esp32s3 && production build
   ↓
cd test && set-target esp32s3 && test firmware build
   ↓
unit tests / host tests
   ↓
static checks
```

Hardware test không nên bắt buộc trong CI cloud nếu chưa có hardware
runner.

------------------------------------------------------------------------

# 24. Hardware-in-the-loop (HIL)

HIL runner cần:

``` text
ESP32-S3 Gateway
ESP32 BLE Peripheral Simulator (service 0xABF0)
USB serial access
test Wi-Fi AP
pytest runner
```

Pipeline:

``` text
flash peripheral
flash gateway
wait boot
verify BLE connect
send HTTP command
verify peripheral received command
verify ACK
reboot peripheral
verify reconnect
collect logs
generate result
```

------------------------------------------------------------------------

# 25. Cấu trúc test đề xuất cuối cùng

``` text
esp32-ble-gateway/
├── components/
│   ├── cbor_codec/test/          ✅ có
│   ├── command_dispatcher/test/  ✅ có (2 file)
│   ├── device_store/test/        ✅ có
│   ├── log_buffer/test/          ✅ có
│   ├── mcp_endpoint/test/        ✅ có (3 file + mock transport)
│   ├── wifi_provisioning/test/   ✅ có DNS — ⬜ cần wiring vào TEST_COMPONENTS
│   ├── ble_central/test/         ⬜ cần tạo
│   └── web_server/test/          ⬜ cần tạo (ưu tiên pytest thay vì Unity)
│
├── test/                         ✅ có (unity app, TEST_COMPONENTS cần cập nhật)
│
├── tests/                        ⬜ cần tạo
│   ├── host/
│   ├── integration/
│   │   ├── test_http.py
│   │   ├── test_mcp.py
│   │   ├── test_wifi.py
│   │   └── test_ble.py
│   ├── stress/
│   │   ├── test_ble_scale.py
│   │   ├── test_command_load.py
│   │   └── test_soak.py
│   └── fixtures/
│       └── peripheral_simulator/
│
└── .github/                      ⬜ cần tạo
    └── workflows/
        ├── build.yml
        └── test.yml
```

Lưu ý AGENTS.md: khi thêm test dir mới cho một component, phải cập nhật
chuỗi `TEST_COMPONENTS` trong `test/CMakeLists.txt`.

------------------------------------------------------------------------

# 26. Release Gate --- Phase 1

Firmware chỉ được xem là Phase-1 Ready khi:

-   [ ] Production firmware build thành công.
-   [ ] Test firmware build thành công.
-   [ ] 100% unit tests PASS (77 case hiện có + các case bổ sung).
-   [ ] CBOR logic coverage mục tiêu \>= 90%.
-   [ ] Dispatcher logic coverage mục tiêu \>= 90%.
-   [ ] Device Store logic coverage mục tiêu \>= 90%.
-   [ ] Web API contract tests PASS (theo endpoint thật, mục 13).
-   [ ] MCP JSON-RPC tests PASS (bao gồm wire mode 2026-07-28).
-   [ ] BLE scan/connect/discovery/write/notify PASS.
-   [ ] BLE reconnect sau peripheral reboot PASS.
-   [ ] Wi-Fi provisioning/recovery PASS.
-   [ ] HTTP → BLE → ACK E2E PASS.
-   [ ] MCP → BLE → ACK E2E PASS.
-   [ ] Multi-device test 8--9 thiết bị PASS (giới hạn
        `CONFIG_BT_NIMBLE_MAX_CONNECTIONS=9`).
-   [ ] 24-hour soak test PASS.
-   [ ] Không watchdog/panic/deadlock.
-   [ ] Không có xu hướng giảm heap liên tục.
-   [ ] RF test với enclosure/vật cản đã thực hiện.

------------------------------------------------------------------------

# 27. Thứ tự triển khai

Cập nhật theo trạng thái thực tế:

1.  Wiring `wifi_provisioning` vào `TEST_COMPONENTS` (việc nhỏ, làm
    ngay) và hoàn thiện edge/boundary test cho 5 component hiện có
    (các ô ⬜ trong mục 5–9).
2.  Refactor BLE Central tách NimBLE adapter để mock được (file đã tách
    module, còn gọi thẳng NimBLE).
3.  Tạo `components/ble_central/test/` và cập nhật `TEST_COMPONENTS`.
4.  Bổ sung logic test provisioning STA/AP (phần radio).
5.  Xây Web API contract tests bằng pytest theo endpoint thật (mục 13).
6.  Tạo BLE Peripheral Simulator đúng contract `0xABF0/ABF1/ABF2`.
7.  Xây HTTP → BLE → ACK E2E.
8.  Xây MCP → BLE → ACK E2E.
9.  Thêm software CI (tạo `.github/workflows/` từ đầu).
10. Xây HIL runner.
11. Thực hiện multi-device stress (tối đa 9 kết nối).
12. Thực hiện 24-hour soak và RF test.

------------------------------------------------------------------------

# 28. Định nghĩa "Test hoàn thành"

Đối với ESP32 BLE Gateway, test không được xem là hoàn thành chỉ vì
Unity unit tests PASS lúc boot.

Điều kiện cuối cùng phải chứng minh được:

``` text
HTTP / MCP command
        ↓
Command Dispatcher
        ↓
CBOR
        ↓
BLE Central
        ↓
real/simulated BLE peripheral (service 0xABF0)
        ↓
ACK
        ↓
Gateway response
```

đồng thời Gateway phải:

-   tự phục hồi sau BLE disconnect;
-   tự phục hồi sau peripheral reboot;
-   xử lý được Wi-Fi reconnect;
-   không panic/watchdog/deadlock;
-   duy trì memory ổn định;
-   vận hành ổn định với tải multi-device mục tiêu (≤ 9 kết nối BLE
    đồng thời với config hiện tại).

Đây là tiêu chí kiểm thử phù hợp với mục tiêu Giai đoạn 1 của ESP32 BLE
Gateway.
