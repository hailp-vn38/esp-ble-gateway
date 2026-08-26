# Tài liệu Test --- ESP32 BLE Gateway (Giai đoạn 1)

**Phiên bản:** 2.0\
**Cập nhật:** 26/08/2026\
**Target:** ESP32-S3\
**Framework:** ESP-IDF native + NimBLE + Unity\
**Mục tiêu:** Chuẩn hóa chiến lược kiểm thử từ unit test đến end-to-end,
stress/soak và hardware-in-the-loop cho ESP32 BLE Gateway.

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

Repository hiện đã có test cho:

``` text
components/cbor_codec/test/
components/command_dispatcher/test/
components/device_store/test/
components/log_buffer/test/
components/mcp_endpoint/test/
```

Test application:

``` text
test/
├── CMakeLists.txt
├── sdkconfig.defaults
└── main/
    ├── CMakeLists.txt
    ├── idf_component.yml
    └── test_main.c
```

`test/CMakeLists.txt` hiện cấu hình:

``` cmake
set(TEST_COMPONENTS
    "device_store;cbor_codec;command_dispatcher;log_buffer;mcp_endpoint"
    CACHE STRING
    "Gateway components whose unit tests are included")
```

Ba component quan trọng chưa có test component đầy đủ:

``` text
ble_central
wifi_provisioning
web_server
```

Đây là phần cần ưu tiên bổ sung.

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

Không được xem `idf.py test` PASS là đủ để release firmware.

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

-   Encode/decode giữ nguyên message fields.
-   Optional device fields.
-   JSON conversion.
-   Invalid CBOR.
-   Unsupported protocol version.

### Test cần bổ sung

  ID         Test
  ---------- ------------------------------
  CBOR-001   encode/decode round trip
  CBOR-002   optional `device_id` absent
  CBOR-003   optional BLE address absent
  CBOR-004   JSON → message → JSON
  CBOR-005   malformed CBOR
  CBOR-006   truncated CBOR
  CBOR-007   missing required field
  CBOR-008   wrong CBOR value type
  CBOR-009   unsupported protocol version
  CBOR-010   output buffer too small
  CBOR-011   maximum `device_id` length
  CBOR-012   maximum command length
  CBOR-013   invalid BLE MAC
  CBOR-014   JSON null
  CBOR-015   JSON array instead of object
  CBOR-016   integer boundary values

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

-   Add/find.
-   Reject duplicate.
-   Edit/delete.
-   BLE address persistence.
-   Entry compaction.
-   MAC device ID migration.

### Test cần bổ sung

  ID          Test
  ----------- ------------------------------------
  STORE-001   add/find
  STORE-002   duplicate device
  STORE-003   edit
  STORE-004   delete
  STORE-005   BLE address persistence
  STORE-006   compaction
  STORE-007   fill `DEVICE_STORE_MAX_DEVICES`
  STORE-008   add when full
  STORE-009   delete nonexistent device
  STORE-010   edit nonexistent device
  STORE-011   empty device ID
  STORE-012   maximum ID/name/type length
  STORE-013   repeated init
  STORE-014   snapshot buffer smaller than store
  STORE-015   missing NVS key
  STORE-016   corrupted/partial NVS data
  STORE-017   persistence after reboot

### Hardware persistence test

Thực hiện:

``` text
write device
    ↓
restart/reboot
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
components/command_dispatcher/test/test_command_dispatcher.c
```

### Test hiện có

-   Dynamic registry.
-   Duplicate registration rejection.
-   `list_devices`.
-   Unknown gateway command.

### Test cần bổ sung

  ID         Test
  ---------- ----------------------------------
  DISP-001   init
  DISP-002   register command
  DISP-003   duplicate command
  DISP-004   registry full
  DISP-005   unknown gateway command
  DISP-006   unsupported message type
  DISP-007   unsupported protocol version
  DISP-008   device command without device ID
  DISP-009   nonexistent device
  DISP-010   disconnected device
  DISP-011   BLE send success
  DISP-012   BLE send failure
  DISP-013   ACK success
  DISP-014   ACK timeout
  DISP-015   wrong-device ACK
  DISP-016   late ACK
  DISP-017   duplicate ACK
  DISP-018   concurrent dispatcher calls
  DISP-019   repeated dispatcher init

## 7.1 Dependency injection cho BLE

Dispatcher không nên phụ thuộc trực tiếp vào NimBLE trong unit test.

Khuyến nghị tạo interface tương đương:

``` c
typedef int (*dispatcher_ble_send_fn_t)(
    const char *device_id,
    const uint8_t *data,
    size_t len);
```

Unit test inject mock BLE sender để kiểm tra:

-   packet gửi;
-   số lần gửi;
-   error propagation;
-   ACK/timeout behavior.

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

Test cần bao phủ ring-buffer semantics:

  ID        Test
  --------- ---------------------------
  LOG-001   empty buffer
  LOG-002   one entry
  LOG-003   exactly capacity
  LOG-004   capacity + 1
  LOG-005   oldest entry overwritten
  LOG-006   repeated wrap-around
  LOG-007   long message
  LOG-008   reset/init
  LOG-009   ordering
  LOG-010   concurrent writer/readers

Đặc biệt cần kiểm tra concurrency vì BLE callback và HTTP/Web UI có thể
cùng truy cập log.

------------------------------------------------------------------------

# 9. MCP Endpoint

**File hiện tại**

``` text
components/mcp_endpoint/test/test_mcp_endpoint.c
```

Test cần bao phủ JSON-RPC boundary:

  ID        Test
  --------- -------------------------
  MCP-001   valid JSON-RPC
  MCP-002   invalid JSON
  MCP-003   wrong `jsonrpc` version
  MCP-004   missing method
  MCP-005   unknown method
  MCP-006   missing params
  MCP-007   invalid params
  MCP-008   numeric ID
  MCP-009   string ID
  MCP-010   notification
  MCP-011   `list_tools`
  MCP-012   valid `call_tool`
  MCP-013   dispatcher failure
  MCP-014   oversized request
  MCP-015   truncated HTTP body

Các JSON-RPC error code cần kiểm tra:

``` text
-32700 Parse error
-32600 Invalid Request
-32601 Method not found
-32602 Invalid params
-32603 Internal error
```

------------------------------------------------------------------------

# 10. BLE Central

Hiện đây là khoảng trống test quan trọng nhất.

Cần tạo:

``` text
components/ble_central/test/
├── CMakeLists.txt
└── test_ble_central.c
```

## 10.1 BLE logic tests

Không yêu cầu peripheral thật nếu NimBLE được đặt sau adapter/mock
interface.

  ID        Test
  --------- -----------------------------
  BLE-001   INIT → SCANNING
  BLE-002   SCANNING → CONNECTING
  BLE-003   CONNECTING → CONNECTED
  BLE-004   disconnect updates state
  BLE-005   reconnect scheduled
  BLE-006   duplicate discovery ignored
  BLE-007   unknown device ignored
  BLE-008   max connection limit
  BLE-009   connection table add/remove
  BLE-010   invalid connection handle
  BLE-011   retry counter
  BLE-012   reconnect backoff
  BLE-013   supervisor start/stop
  BLE-014   GATT write success
  BLE-015   GATT write failure
  BLE-016   notify dispatch

## 10.2 NimBLE adapter

Khuyến nghị cô lập các API:

``` text
ble_port_scan_start()
ble_port_scan_stop()
ble_port_connect()
ble_port_disconnect()
ble_port_gatt_write()
```

Production adapter gọi NimBLE thật.

Unit test adapter sử dụng mock.

------------------------------------------------------------------------

# 11. BLE Hardware Integration

Cần tối thiểu:

``` text
Board A: ESP32-S3 Gateway DUT
Board B: BLE Peripheral Simulator
```

Peripheral simulator nên cung cấp:

-   Service UUID cố định.
-   RX characteristic.
-   TX notify characteristic.
-   Echo command.
-   ACK.
-   Sequence number.
-   Configurable ACK delay.
-   Configurable disconnect.

### Test cases

  ID           Test
  ------------ ---------------------------
  BLE-HW-001   scan discovers peripheral
  BLE-HW-002   connect
  BLE-HW-003   service discovery
  BLE-HW-004   subscribe notify
  BLE-HW-005   write command
  BLE-HW-006   receive ACK
  BLE-HW-007   peripheral disconnect
  BLE-HW-008   automatic reconnect
  BLE-HW-009   peripheral reboot
  BLE-HW-010   gateway reboot
  BLE-HW-011   malformed packet
  BLE-HW-012   delayed ACK
  BLE-HW-013   missing ACK

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

Cần tạo test cho:

``` text
components/wifi_provisioning/
```

Các testcase chính:

  ID         Test
  ---------- ------------------------------
  WIFI-001   empty NVS starts SoftAP
  WIFI-002   saved credentials starts STA
  WIFI-003   valid credentials connect
  WIFI-004   wrong password
  WIFI-005   AP unavailable
  WIFI-006   retry
  WIFI-007   retry exhausted
  WIFI-008   fallback to SoftAP
  WIFI-009   credentials survive reboot
  WIFI-010   erase credentials
  WIFI-011   STA disconnect
  WIFI-012   AP reboot/recovery

Wi-Fi radio test phải chạy trên phần cứng thật.

------------------------------------------------------------------------

# 13. Web Server

Không nên mock sâu `httpd_req_t` cho toàn bộ API.

Ưu tiên chạy HTTP server thật trên Gateway và kiểm tra từ pytest.

Các contract tests:

  ID         Endpoint/Test
  ---------- -------------------
  HTTP-001   gateway status
  HTTP-002   list devices
  HTTP-003   add device
  HTTP-004   edit device
  HTTP-005   delete device
  HTTP-006   BLE scan
  HTTP-007   BLE connect
  HTTP-008   send command
  HTTP-009   logs
  HTTP-010   MCP endpoint
  HTTP-011   invalid JSON
  HTTP-012   wrong HTTP method
  HTTP-013   unknown endpoint
  HTTP-014   oversized body

Ví dụ pytest:

``` python
def test_status(gateway):
    response = requests.get(gateway + "/api/status")
    assert response.status_code == 200

    data = response.json()
    assert "wifi_connected" in data
```

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

------------------------------------------------------------------------

# 15. L3 --- End-to-End

Đây là test quan trọng nhất để xác nhận Gateway hoạt động đúng.

## E2E-001 --- HTTP → BLE → ACK

``` text
HTTP POST
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
ACK Notify
    ↓
Dispatcher
    ↓
HTTP Response
```

Điều kiện PASS:

-   HTTP request hợp lệ.
-   Peripheral nhận đúng command.
-   Device ID đúng.
-   Payload đúng.
-   ACK đúng.
-   Gateway trả success.

## E2E-002 --- MCP → BLE → ACK

``` text
JSON-RPC call_tool
    ↓
MCP Endpoint
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
SoftAP
    ↓
submit Wi-Fi credentials
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

Test theo các mức:

``` text
1 device
2 devices
5 devices
8 devices
10 devices
```

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
10 devices
1 command/device/second
```

Sau đó tăng tải:

``` text
2 commands/sec
5 commands/sec
10 commands/sec
```

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
8–10 BLE devices
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

  ID          Fault
  ----------- --------------------------------------
  FAULT-001   peripheral disappears during command
  FAULT-002   peripheral reboot
  FAULT-003   delayed ACK
  FAULT-004   duplicate ACK
  FAULT-005   malformed BLE notify
  FAULT-006   Wi-Fi disconnect
  FAULT-007   AP unavailable
  FAULT-008   HTTP client disconnect
  FAULT-009   malformed JSON
  FAULT-010   corrupted/missing NVS
  FAULT-011   concurrent HTTP requests
  FAULT-012   repeated BLE scan/start/stop

Điều kiện chung:

> Fault không được làm Gateway panic, deadlock hoặc mất khả năng tự phục
> hồi ngoài các trường hợp lỗi không thể phục hồi đã được định nghĩa rõ.

------------------------------------------------------------------------

# 22. Host Tests và Sanitizer

Các module logic thuần C nên cân nhắc build trên host:

``` text
cbor_codec
registry
message validation
ring buffer
parser helpers
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

------------------------------------------------------------------------

# 23. CI

CI software chạy trên mọi push/pull request.

Pipeline đề xuất:

``` text
checkout
   ↓
setup ESP-IDF
   ↓
production build
   ↓
test firmware build
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
ESP32 BLE Peripheral Simulator
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
esp-ble-gateway/
├── components/
│   ├── cbor_codec/test/
│   ├── command_dispatcher/test/
│   ├── device_store/test/
│   ├── log_buffer/test/
│   ├── mcp_endpoint/test/
│   ├── ble_central/test/
│   ├── wifi_provisioning/test/
│   └── web_server/test/
│
├── test/
│   ├── CMakeLists.txt
│   ├── sdkconfig.defaults
│   └── main/
│
├── tests/
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
└── .github/
    └── workflows/
        ├── build.yml
        └── test.yml
```

------------------------------------------------------------------------

# 26. Release Gate --- Phase 1

Firmware chỉ được xem là Phase-1 Ready khi:

-   [ ] Production firmware build thành công.
-   [ ] Test firmware build thành công.
-   [ ] 100% unit tests PASS.
-   [ ] CBOR logic coverage mục tiêu \>= 90%.
-   [ ] Dispatcher logic coverage mục tiêu \>= 90%.
-   [ ] Device Store logic coverage mục tiêu \>= 90%.
-   [ ] Web API contract tests PASS.
-   [ ] MCP JSON-RPC tests PASS.
-   [ ] BLE scan/connect/discovery/write/notify PASS.
-   [ ] BLE reconnect sau peripheral reboot PASS.
-   [ ] Wi-Fi provisioning/recovery PASS.
-   [ ] HTTP → BLE → ACK E2E PASS.
-   [ ] MCP → BLE → ACK E2E PASS.
-   [ ] Multi-device test 8--10 thiết bị PASS.
-   [ ] 24-hour soak test PASS.
-   [ ] Không watchdog/panic/deadlock.
-   [ ] Không có xu hướng giảm heap liên tục.
-   [ ] RF test với enclosure/vật cản đã thực hiện.

------------------------------------------------------------------------

# 27. Thứ tự triển khai

Ưu tiên triển khai theo thứ tự:

1.  Hoàn thiện edge/boundary test cho 5 component hiện có.
2.  Refactor BLE Central để NimBLE dependency có thể mock.
3.  Tạo `components/ble_central/test/`.
4.  Bổ sung test Wi-Fi provisioning.
5.  Xây Web API contract tests bằng pytest.
6.  Tạo BLE Peripheral Simulator.
7.  Xây HTTP → BLE → ACK E2E.
8.  Xây MCP → BLE → ACK E2E.
9.  Thêm software CI.
10. Xây HIL runner.
11. Thực hiện multi-device stress.
12. Thực hiện 24-hour soak và RF test.

------------------------------------------------------------------------

# 28. Định nghĩa "Test hoàn thành"

Đối với ESP32 BLE Gateway, test không được xem là hoàn thành chỉ vì
Unity unit tests PASS.

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
real/simulated BLE peripheral
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
-   vận hành ổn định với tải multi-device mục tiêu.

Đây là tiêu chí kiểm thử phù hợp với mục tiêu Giai đoạn 1 của ESP32 BLE
Gateway.
