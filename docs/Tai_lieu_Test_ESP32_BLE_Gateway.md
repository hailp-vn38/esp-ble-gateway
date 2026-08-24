# Tài liệu Test — ESP32 BLE Gateway (Giai đoạn 1)

## Cấu trúc thư mục test

```
esp32-ble-gateway/
├── main/
│   ├── app_main.c
│   └── CMakeLists.txt
├── components/
│   ├── device_store/
│   │   ├── device_store.c
│   │   ├── include/
│   │   └── test/                    # Unit test cho component này
│   │       ├── CMakeLists.txt
│   │       └── test_device_store.c
│   ├── wifi_provisioning/
│   │   ├── wifi_prov.c
│   │   ├── dns_hijack.c
│   │   ├── include/
│   │   └── test/
│   │       ├── CMakeLists.txt
│   │       └── test_wifi_prov.c
│   ├── ble_central/
│   │   ├── ble_central.c
│   │   ├── include/
│   │   └── test/
│   │       ├── CMakeLists.txt
│   │       └── test_ble_central.c
│   ├── cbor_codec/
│   │   ├── cbor_codec.c
│   │   ├── include/
│   │   └── test/
│   │       ├── CMakeLists.txt
│   │       └── test_cbor_codec.c
│   ├── command_dispatcher/
│   │   ├── command_dispatcher.c
│   │   ├── include/
│   │   └── test/
│   │       ├── CMakeLists.txt
│   │       └── test_dispatcher.c
│   ├── web_server/
│   │   ├── web_server.c
│   │   ├── include/
│   │   └── test/
│   │       ├── CMakeLists.txt
│   │       └── test_web_server.c
│   └── mcp_endpoint/
│       ├── mcp_endpoint.c
│       ├── include/
│       └── test/
│           ├── CMakeLists.txt
│           └── test_mcp_endpoint.c
├── test/                                # Integration test (tuy chon)
│   ├── main/
│   │   ├── test_main.c
│   │   └── CMakeLists.txt
│   └── pytest_esp32_ble_gateway.py
├── CMakeLists.txt
├── sdkconfig.defaults
└── README.md
```

## Quy ước đặt tên

- File test trong component: `test_<component_name>.c` (ví´¹ dụ `test_device_store.c`).
- Mỗi file test có ít nhất 1 `TEST_CASE()` với tag `[<component_name>]` để lọc khi chạy[web:196][web:200][web:203].
- Test file phải `#include "unity.h"` và header của component cần test[web:203][web:206].

## Cách chạy test

### 1. Unit test cho từng component (chạy trên thiết bị thật)

```bash
cd esp32-ble-gateway

# Test 1 component cu the
idf.py -p /dev/ttyUSB0 test -T device_store flash monitor

# Test nhieu component cung luc
idf.py -p /dev/ttyUSB0 test -T "device_store cbor_codec command_dispatcher" flash monitor

# Test TAT CA component co test/ subdirectory
idf.py -p /dev/ttyUSB0 test -T all flash monitor

# Loc test theo tag (vi du chi chay test co tag [device_store])
idf.py -p /dev/ttyUSB0 test -T "device_store" flash monitor

# Loc test theo ten test case (string match)
idf.py -p /dev/ttyUSB0 test --test-filter "add and find" flash monitor
```

### 2. Chạy test trên QEMU (khong can phan cung, nhanh cho CI)

```bash
# Build test app cho QEMU
idf.py -T all build

# Chay tren QEMU (ESP32-S3)
idf.py -T all qemu
```

### 3. Integration test (tuy chon, cho test he thong tong the)

```bash
cd test
idf.py -p /dev/ttyUSB0 flash monitor
# Hoac chay pytest neu co script Python tu dong hoa
pytest pytest_esp32_ble_gateway.py
```

## Nội dung test cho từng module

### Module 1 — Device Store (NVS)

**File:** `components/device_store/test/test_device_store.c` (đã·¹ có sẵn từ Module 1 hoàn thiện)

**Test cases:**
- `add and find device` — add 1 thiết bị, tìm lại đúng.
- `add duplicate device_id fails` — thêm trùng ID phải trả lỗi.
- `delete device removes it from list` — xóa xong không tìm thấy nữa.
- `edit device updates name and type` — sửa tên/type đúng.
- `set ble addr persists has_ble_addr flag` — lưu MAC address đúng.
- `device list persists across re-init` — reboot vẫn còn dữ liệu.

**Lưu ý:** Các test này cần NVS đã được init (`nvs_flash_init()`) trước khi chạy — thường được gọi tự động từ Unity test app main, hoặc thêm `setUp()` riêng nếu cần.

### Module 2 — Wi-Fi Provisioning

**File:** `components/wifi_provisioning/test/test_wifi_prov.c` (cần tạo mới)

**Test cases đề xuất:**
- `load saved credentials on init` — nếu đã lưu SSID/pass, `wifi_prov_init()` phải ở mode STA.
- `no credentials starts softap` — chưa lưu gì thì khởi động ở SoftAP.
- `save_and_connect writes NVS and switches mode` — lưu credentials và chuyển mode ngay (kiểm tra qua `wifi_prov_get_state()`).
- `fallback to softap after retries` — giả lập disconnect liên tục (có·¹·thể mock qua event handler) để kiểm tra fallback về SoftAP.

**Lưu ý:** Test Wi-Fi cần phần cứng thật (QEMU không hỗ trợ Wi-Fi đầy đủ), và cần môi trường có AP thật để test kết nối STA.

### Module 3 — BLE Central

**File:** `components/ble_central/test/test_ble_central.c` (cần tạo mới)

**Test cases đề xuất:**
- `scan finds gateway service` — `ble_central_scan_start()` tìm thấy thiết bị quảng bá UUID `0xABF0`.
- `connect success stores ble_addr` — connect thành công gọi `device_store_set_ble_addr()` đúng.
- `reconnect supervisor starts task` — `ble_central_start_reconnect_supervisor()` tạo task và chạy.
- `send_command encodes and writes` — mock `ble_gattc_write_no_rsp_flat()` (qua function pointer) để kiểm tra encode đúng CBOR.

**Lưu ý:** BLE test cần ít nhất 2 board ESP32 (1 làm central, 1 làm peripheral giả lập) để test end-to-end. Có thể dùng `device-module` đã hoàn thiện làm peripheral test.

### Module 4 — CBOR Codec

**File:** `components/cbor_codec/test/test_cbor_codec.c` (cần tạo mới)

**Test cases đề xuất:**
- `encode decode round trip` — encode 1 message, decode lại, so sánh từng field.
- `encode with optional device_id` — test `has_device_id=0` (bỏ qua field) và `=1` (có·¹·field).
- `json to msg and back` — parse JSON thành `gw_message_t`, encode lại thành JSON, so sánh.
- `invalid cbor returns error` — decode buffer sai format phải trả `-1`.

**Lưu ý:** Đây là test thuần logic, không cần phần cứng — chạy được trên QEMU.

### Module 5 — Command Dispatcher

**File:** `components/command_dispatcher/test/test_dispatcher.c` (cần tạo mới)

**Test cases đề xuất:**
- `mutex protects concurrent calls` — tạo 2 task cùng gọi `command_dispatcher_handle()`, kiểm tra không có race condition (dựa vào log hoặc kết quả cuối cùng).
- `ack timeout waits for notify` — mock `ble_central_send_command()` và `command_dispatcher_on_device_notify()` để kiểm tra cơ chế chờ ACK.
- `list_devices returns correct json` — kiểm tra output của `cmd_list_devices()` đúng format JSON array.

**Lưu ý:** Test mutex cần FreeRTOS task, chạy được trên QEMU.

### Module 6 — Web Server

**File:** `components/web_server/test/test_web_server.c` (cần tạo mới)

**Test cases đề xuất:**
- `get devices returns json array` — gọi `devices_get_handler()` mock request, kiểm tra response JSON.
- `post device calls dispatcher` — mock `command_dispatcher_handle()` để kiểm tra `devices_post_handler()` gọi đúng.
- `command endpoint sends correct result` — test `command_post_handler()` với message hợp lệ và không hợp lệ.

**Lưu ý:** HTTP server test cần mock `httpd_req_t` structure — có thể dùng helper function từ ESP-IDF test framework hoặc tự tạo struct giả với các field cần thiết.

### Module 7 — MCP Endpoint

**File:** `components/mcp_endpoint/test/test_mcp_endpoint.c` (cần tạo mới)

**Test cases đề xuất:**
- `list_tools returns dynamic list` — kiểm tra `handle_list_tools()` trả về đúng danh sách từ registry.
- `call_tool with valid params` — test `handle_call_tool()` với params hợp lệ.
- `notification does not send response` — test request không có `id` không gửi response (kiểm tra `httpd_resp_send()` không được gọi).
- `invalid jsonrpc returns error` — test `jsonrpc` thiếu hoặc sai trả lỗi `-32600`.

## Integration test (tuy chọn)

**File:** `test/main/test_main.c` (test app chạy trên thiết bị thật, test toàn bộ hệ thống)

**Mục tiêu:** Test end-to-end từ Wi-Fi provisioning → BLE connect → send command → nhận ACK → log đúng.

**Kịch bản đề xuất:**
1. Boot gateway, kiểm tra ở SoftAP mode (chưa lưu Wi-Fi).
2. Giả lập client kết nối SoftAP, submit form Wi-Fi (có·¹·thể dùng script Python tự động qua HTTP POST).
3. Kiểm tra gateway chuyển sang STA và lấy IP.
4. Gateway scan và connect tới 1 thiết bị BLE giả lập (dũng `device-module` flash trên board khác).
5. Gửi lệnh `toggle` qua HTTP `/api/command` và kiểm tra thiết bị nhận được.
6. Kiểm tra log buffer có đúng entry `[SENT]` và `[ACK]`.

**File pytest:** `test/pytest_esp32_ble_gateway.py` (tự động hóa kịch bản trên qua serial/HTTP)

```python
import pytest
import requests
import time

GATEWAY_IP = "192.168.1.100"  # Can lay tu serial log hoac DHCP server

def test_gateway_boot_in_softap():
    # Kiem tra qua serial log hoac scan Wi-Fi
    pass

def test_switch_to_sta_after_wifi_config():
    resp = requests.post(f"http://192.168.4.1/api/wifi",
                         json={"ssid": "TestNetwork", "password": "testpass"})
    assert resp.json()["success"] == True
    time.sleep(5)  # Cho gateway chuyen mode va ket noi
    # Kiem tra gateway co IP moi
    resp = requests.get(f"http://{GATEWAY_IP}/api/status")
    assert resp.json()["wifi_connected"] == True

def test_send_toggle_command():
    resp = requests.post(f"http://{GATEWAY_IP}/api/command",
                         json={"device_id": "dev_A", "command": "toggle", "bool_value": True})
    assert resp.json()["success"] == True
    # Kiem tra log co entry [ACK]
    resp = requests.get(f"http://{GATEWAY_IP}/api/logs")
    logs = resp.json()
    assert any("[ACK]" in log["text"] for log in logs)
```

## CI/CD với idf-ci (tuy chọn)

Nếu muốn tích hợp vào GitHub Actions/GitLab CI, dùng `idf-ci` tool[web:197]:

```bash
# Cai dat idf-ci
pip install idf-ci

# Khoi tao cau hinh test
idf-ci test init

# Them vao GitHub Actions workflow
# .github/workflows/ci.yml
name: ESP32 BLE Gateway CI
on: [push, pull_request]
jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Set up ESP-IDF
        uses: espressif/esp-idf-action@v1
      - name: Run unit tests on QEMU
        run: idf.py -T all build && idf.py -T all qemu
```

## Checklist trước khi release

- [ ] Tất cả unit test component chạy thành công trên QEMU (trừ test cần Wi-Fi/BLE).
- [ ] Test Wi-Fi/BLE chạy thành công trên ít nhất 2 board ESP32-S3 thật.
- [ ] Integration test end-to-end chạy thành công (từ SoftAP → STA → BLE → ACK).
- [ ] Code coverage (nếu cần) đạt >80% cho các module logic (device_store, cbor_codec, command_dispatcher).
- [ ] Tài liệu test được cập nhật khi thêm test mới hoặc đổi logic.

## Lưu ý quan trọng

- Test files trong `components/*/test/` không được include vào build production — ESP-IDF tự động chỉ build khi chạy `idf.py test`[web:200][web:203][web:206].
- Ưu tiên test logic độc lập phần cứng (CBOR, dispatcher, NVS) chạy trên QEMU trước, chỉ test Wi-Fi/BLE trên phần cứng thật sau cùng — tiết kiệm thời gian CI.
- Khi thêm test mới, nhớ thêm tag `[<component_name>]` vào `TEST_CASE()` để dễ lọc khi chạy[web:196][web:203].
