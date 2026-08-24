# Kế hoạch triển khai Giai đoạn 1 — ESP32 BLE Gateway

**Phạm vi:** ESP32-S3 làm BLE Central, Web UI quản lý, MCP endpoint qua LAN, lưu trữ NVS + RAM.
**Không thuộc giai đoạn 1:** Vai trò Peripheral cho Mac (Swift/CoreBluetooth) — để giai đoạn 2.
**Framework:** ESP-IDF native + NimBLE.

---

## 1. Mục tiêu nghiệm thu giai đoạn 1

Giai đoạn 1 hoàn thành khi:
- ESP32-S3 giữ kết nối ổn định đồng thời với 5-10 thiết bị BLE DIY, gửi/nhận lệnh 2 chiều không rớt kết nối bất thường trong điều kiện có vật cản (tường, box).
- Người dùng có thể thiết lập Wi-Fi lần đầu qua SoftAP, sau đó truy cập Web UI qua IP nội bộ để add/delete/edit thiết bị và xem log.
- AI Agent gọi được vào endpoint `/mcp` qua LAN để bật/tắt thiết bị và kiểm tra trạng thái gateway, nhận phản hồi đúng định dạng JSON-RPC.
- Cấu hình (Wi-Fi, danh sách thiết bị) giữ nguyên qua reboot; log/buffer tạm không cần giữ qua reboot.
- Toàn bộ lệnh (Web UI và MCP) đi qua một lớp Command Dispatcher duy nhất.

---

## 2. Cấu trúc module firmware (ESP-IDF component layout)

```
esp32-ble-gateway/
├── main/
│   └── app_main.c                 # Khởi tạo NVS, Wi-Fi, NimBLE, HTTP server, dispatcher
├── components/
│   ├── ble_central/               # Quản lý scan/connect/GATT client tới thiết bị DIY
│   │   ├── ble_central.c/.h
│   │   └── ble_conn_pool.c/.h     # Quản lý danh sách kết nối, connection params
│   ├── cbor_codec/                # Encode/decode CBOR, chuyển đổi CBOR <-> JSON
│   │   └── cbor_codec.c/.h
│   ├── command_dispatcher/        # Lớp phân loại type message, registry hàm gateway
│   │   ├── dispatcher.c/.h
│   │   └── command_registry.c/.h
│   ├── device_store/               # Quản lý danh sách thiết bị, đọc/ghi NVS
│   │   └── device_store.c/.h
│   ├── wifi_provisioning/          # SoftAP + trang cấu hình Wi-Fi lần đầu
│   │   └── wifi_prov.c/.h
│   ├── web_server/                  # HTTP server, route Web UI + static file
│   │   ├── web_server.c/.h
│   │   └── routes_device.c
│   ├── mcp_endpoint/                # JSON-RPC handler tại /mcp
│   │   └── mcp_handler.c/.h
│   └── log_buffer/                  # Circular buffer RAM cho log/queue tạm
│       └── log_buffer.c/.h
└── CMakeLists.txt
```

Nguyên tắc: mỗi component độc lập, chỉ giao tiếp qua interface (.h) rõ ràng — để dễ thay thế/mở rộng khi lên multi-gateway hoặc thêm Peripheral role ở giai đoạn 2.

---

## 3. Lộ trình 7 module theo thứ tự triển khai

### Module 1 — Khởi tạo nền & NVS storage layer
**Nội dung:**
- Khởi tạo `nvs_flash_init()`, xử lý lỗi partition (erase + init lại nếu version mismatch)[web:59][web:60].
- Định nghĩa namespace NVS: `wifi_cfg` (SSID/password), `dev_list` (danh sách thiết bị), `dev_map` (mapping tên/type theo device ID).
- Viết `device_store` component: hàm `add_device()`, `delete_device()`, `edit_device()`, `list_devices()` — đọc/ghi qua `nvs_get_*`/`nvs_set_*` với namespace tương ứng.
- Giới hạn: NVS namespace tối đa 15 ký tự, cần đặt tên ngắn gọn nhất quán[web:59].

**Kết quả kiểm thử:** Ghi 5 thiết bị giả, reboot, đọc lại đúng danh sách.

### Module 2 — Wi-Fi Provisioning (SoftAP)
**Nội dung:**
- ESP32 khởi động ở SoftAP mode nếu NVS chưa có `wifi_cfg`.
- Trang cấu hình đơn giản (form nhập SSID/password) — không bắt buộc captive portal auto-redirect ở bước đầu, có thể bổ sung DNS hijack sau nếu cần UX tốt hơn.
- Sau khi nhận cấu hình, lưu vào NVS, chuyển sang STA mode, kết nối Wi-Fi LAN.
- Cơ chế fallback: nếu kết nối STA thất bại liên tục, quay lại SoftAP để cấu hình lại.

**Kết quả kiểm thử:** Từ máy tính/điện thoại kết nối SoftAP, nhập Wi-Fi, gateway chuyển sang LAN và có thể ping tới IP nội bộ.

### Module 3 — BLE Central core (NimBLE)
**Nội dung:**
- Khởi tạo NimBLE host, cấu hình `NIMBLE_MAX_CONNECTIONS` phù hợp (ESP-NimBLE hỗ trợ tới 70 kết nối tùy cấu hình SDK, nên đặt mức 10-16 cho giai đoạn 1 để có biên độ dư)[web:70].
- Viết `ble_central`: hàm scan, filter theo tên/UUID service của thiết bị DIY, connect, negotiate MTU (256-512 byte).
- Viết `ble_conn_pool`: quản lý mảng connection handle, ánh xạ `device_id ↔ conn_handle`, theo dõi trạng thái connected/disconnected.
- Cấu hình connection parameter mỗi kết nối: interval 15ms, peripheral latency 0, timeout 100-200ms (điều chỉnh nếu vật cản gây rớt kết nối giả).
- Đăng ký callback nhận Notify từ thiết bị (đẩy vào hàng đợi xử lý), gửi Write Command khi có lệnh.

**Kết quả kiểm thử:** Kết nối ổn định đồng thời 5+ thiết bị test (dùng board ESP32 khác chạy firmware GATT server giả lập), gửi lệnh và nhận notify đúng device_id tương ứng, thử tạo vật cản để kiểm tra độ ổn định reconnect.

### Module 4 — CBOR codec layer
**Nội dung:**
- Tích hợp thư viện CBOR nhẹ cho ESP-IDF (component `QCBOR` hoặc `libcbor`, add qua `idf_component.yml` hoặc submodule).
- Định nghĩa schema message nội bộ tối thiểu: `{type: string, device_id: string (optional), payload: map}`.
- Viết hàm `cbor_encode_message()`, `cbor_decode_message()`.
- Viết hàm chuyển đổi `cbor_to_json()` và `json_to_cbor()` để dùng ở lớp Web UI/MCP.

**Kết quả kiểm thử:** Encode một message mẫu (device command bật thiết bị A), decode lại đúng dữ liệu, convert qua JSON và so khớp.

### Module 5 — Command Dispatcher & Registry
**Nội dung:**
- Định nghĩa 2 loại message: `device_command` (có `device_id`, forward qua `ble_central`) và `gateway_command` (không có `device_id`, tra registry nội bộ).
- Viết `command_registry`: bảng ánh xạ tĩnh `command_name → function pointer`, đăng ký các lệnh gateway cơ bản: `add_device`, `delete_device`, `edit_device`, `list_devices`, `get_status`.
- Viết `dispatcher_handle_message()`: nhận CBOR message đã decode, kiểm tra `type`, route tới đúng nhánh xử lý.
- Đảm bảo dispatcher không phụ thuộc vào nguồn gọi (Web UI hay MCP) — chỉ nhận struct message chuẩn hóa.

**Kết quả kiểm thử:** Gửi message giả từ code test cho cả 2 loại type, xác nhận dispatcher route đúng và trả kết quả đúng format.

### Module 6 — Web Server & Web UI
**Nội dung:**
- Khởi tạo `esp_http_server`, đăng ký route: `GET /` (trang chính), `GET/POST /api/devices` (CRUD thiết bị), `GET /api/logs` (đọc log buffer RAM), `GET /api/status`.
- Trang HTML/JS đơn giản (có thể serve tĩnh từ SPIFFS hoặc embed vào flash qua `EMBED_FILES` trong CMakeLists) — hiển thị danh sách thiết bị, form add/edit, log viewer.
- Mỗi API endpoint gọi vào `command_dispatcher` để xử lý, trả JSON.

**Kết quả kiểm thử:** Từ browser trên máy tính/điện thoại cùng LAN, truy cập IP gateway, add/xóa thiết bị qua UI, thấy log cập nhật.

### Module 7 — MCP Endpoint (JSON-RPC tối giản)
**Nội dung:**
- Đăng ký route `POST /mcp` trên cùng HTTP server.
- Implement subset JSON-RPC 2.0: parse `method`, `params`, `id` từ request body.
- Hỗ trợ tối thiểu 2 method: `list_tools` (trả danh sách lệnh khả dụng từ command registry) và `call_tool` (gọi `dispatcher_handle_message()` với tham số tương ứng).
- Trả response theo format `{jsonrpc: "2.0", result: ..., id: ...}` hoặc `{jsonrpc: "2.0", error: ..., id: ...}`.

**Kết quả kiểm thử:** Gửi request JSON-RPC mẫu qua `curl`/Postman từ máy khác trong LAN, gọi `call_tool` bật thiết bị A, xác nhận thiết bị nhận lệnh qua BLE và trả kết quả đúng.

---

## 4. Bảng phụ thuộc module

| Module | Phụ thuộc vào |
|---|---|
| 1. NVS storage | Không (nền tảng) |
| 2. Wi-Fi Provisioning | Module 1 (lưu cấu hình) |
| 3. BLE Central core | Module 1 (đọc danh sách thiết bị đã lưu) |
| 4. CBOR codec | Không (độc lập, có thể phát triển song song Module 3) |
| 5. Command Dispatcher | Module 3 + Module 4 |
| 6. Web Server & UI | Module 2 (cần LAN) + Module 5 |
| 7. MCP Endpoint | Module 5 + Module 6 (dùng chung HTTP server) |

Gợi ý trình tự code thực tế: làm Module 1 → 3 → 4 song song → 5 → 2 → 6 → 7, để có thể test Central BLE sớm nhất (rủi ro kỹ thuật cao nhất) trước khi đầu tư vào Web UI.

---

## 5. Thiết kế dữ liệu (schema tối thiểu)

### 5.1 NVS namespace: `dev_list`
| Key | Type | Nội dung |
|---|---|---|
| `count` | u8 | Số lượng thiết bị đã đăng ký |
| `dev_<i>_id` | string | Device ID / MAC address |
| `dev_<i>_name` | string | Tên hiển thị do người dùng đặt |
| `dev_<i>_type` | string | Loại message/thiết bị (dùng để dispatcher biết cách parse payload) |

### 5.2 CBOR message format (nội bộ)
```
{
  "type": "device_command" | "gateway_command",
  "device_id": "<string, optional nếu type=gateway_command>",
  "command": "<string, ví dụ: 'toggle', 'set_value', 'add_device'>",
  "payload": { ... tham số tùy lệnh ... }
}
```

### 5.3 JSON-RPC request mẫu (MCP endpoint)
```json
{
  "jsonrpc": "2.0",
  "method": "call_tool",
  "params": {
    "command": "toggle",
    "device_id": "dev_A",
    "payload": { "state": true }
  },
  "id": 1
}
```

---

## 6. Kế hoạch kiểm thử độ ổn định

- **Test tải kết nối:** Tăng dần số thiết bị test từ 3 → 5 → 10, đo tỷ lệ mất kết nối trong 30 phút liên tục.
- **Test vật cản RF:** Đặt thiết bị sau tường/trong box, đo độ trễ round-trip lệnh và tỷ lệ timeout, tinh chỉnh connection interval/timeout nếu cần.
- **Test reboot:** Add/xóa thiết bị, reboot gateway, xác nhận danh sách thiết bị và Wi-Fi config giữ nguyên (NVS), log buffer reset (RAM, đúng thiết kế).
- **Test đồng thời Web UI + MCP:** Gọi lệnh cùng lúc từ Web UI và từ MCP endpoint, xác nhận dispatcher xử lý đúng thứ tự, không xung đột trạng thái thiết bị.

---

## 7. Rủi ro cụ thể trong giai đoạn 1 và hướng xử lý

| Rủi ro | Hướng xử lý |
|---|---|
| NimBLE giới hạn kết nối tùy cấu hình SDK — cần cấu hình đúng `NIMBLE_MAX_CONNECTIONS` qua `menuconfig`, không chỉ dùng default | Kiểm tra và tăng giá trị này chủ động trong `sdkconfig`, test thực tế trước khi tin vào số lý thuyết[web:70] |
| Vật cản RF gây rớt kết nối giả | Bắt đầu với timeout rộng (150-200ms), thu hẹp dần sau khi đo thực tế ổn định |
| Thư viện CBOR cho ESP-IDF chưa phổ biến bằng JSON | Test kỹ việc build/link thư viện (QCBOR/libcbor) trước khi phát triển các module phụ thuộc vào nó (Module 4 nên làm sớm, tách biệt) |
| Web UI và MCP cùng gọi dispatcher đồng thời gây race condition | Dùng FreeRTOS mutex/queue khi dispatcher truy cập `ble_conn_pool` hoặc `device_store` |
| Flash NVS hao mòn nếu vô tình ghi log thường xuyên | Rà soát code chắc chắn log/buffer luôn đi qua `log_buffer` (RAM), không lỡ tay gọi `nvs_set_*` cho dữ liệu tần suất cao |
