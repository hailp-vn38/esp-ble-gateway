# Tài liệu khung dự án: ESP32 BLE Gateway

**Phiên bản:** 1.0 (Draft khung — Giai đoạn 1)
**Ngày tạo:** 24/08/2026
**Chip mục tiêu:** ESP32-S3
**Framework:** ESP-IDF native (NimBLE stack)

---

## 1. Tổng quan dự án

### 1.1 Mục tiêu
Xây dựng một gateway trung gian dựa trên ESP32-S3, đóng vai trò cầu nối giữa nhiều thiết bị BLE DIY (peripheral) và các hệ thống điều khiển/giám sát phía trên (web UI quản lý, Mac app qua BLE, AI Agent qua MCP-over-LAN). Gateway xử lý giao tiếp 2 chiều: nhận dữ liệu/sự kiện từ thiết bị, và chuyển tiếp lệnh điều khiển xuống đúng thiết bị.

### 1.2 Phạm vi theo giai đoạn

| Giai đoạn | Nội dung | Trạng thái |
|---|---|---|
| Giai đoạn 1 | ESP32 làm BLE Central kết nối nhiều thiết bị DIY, web quản lý cục bộ, MCP endpoint qua LAN, lưu trữ cấu hình vào NVS | Đang thiết kế khung |
| Giai đoạn 2 | Thêm vai trò Peripheral để Mac app (Swift/CoreBluetooth) kết nối trực tiếp qua BLE | Chưa bắt đầu |
| Giai đoạn 3 (định hướng) | Mở rộng đa gateway / chia sẻ cộng đồng maker (MakerWorld, GitHub public) | Định hướng dài hạn |

### 1.3 Nguyên tắc thiết kế xuyên suốt
- Khung code phải tái sử dụng được và dễ bảo trì — ưu tiên module hóa rõ ràng hơn tối ưu sớm.
- Ưu tiên "chạy ổn định trước, mở rộng tính năng sau".
- Gateway không phải là server thông minh — chỉ là bộ định tuyến lệnh (dispatcher) và cầu nối giao tiếp.

---

## 2. Kiến trúc tổng thể

```
                     ┌─────────────────────────┐
   AI Agent  ───LAN──▶  ESP32-S3 Gateway         │◀──BLE (peripheral)── 1..N Thiết bị DIY
 (JSON-RPC tối giản) │  - NimBLE Central role    │
                     │  - HTTP Web Server        │
   Browser  ───LAN──▶  - MCP endpoint (/mcp)     │
 (Web UI quản lý)    │  - Command Dispatcher     │
                     │  - NVS (config) + RAM buf │
                     └─────────────────────────┘
                                │ (Giai đoạn 2)
                                ▼
                     ESP32 Peripheral role ◀──BLE── Mac app (Swift/CoreBluetooth)
```

### 2.1 Vai trò ESP32 theo giai đoạn
- **Giai đoạn 1:** Chỉ đóng vai trò BLE **Central**. Kết nối đồng thời tới nhiều thiết bị DIY (mục tiêu vận hành ổn định ở quy mô ~10 thiết bị — giới hạn hiệu suất thực tế của NimBLE trên một ESP32-S3).
- **Giai đoạn 2:** Bổ sung vai trò **Peripheral** (dual-role) để Mac kết nối trực tiếp qua BLE, chạy song song hai FreeRTOS task (Central task + Peripheral task), tận dụng lõi kép của ESP32-S3.

### 2.2 Định hướng mở rộng (vượt quá ~10 thiết bị)
Khi vượt ngưỡng hiệu suất một gateway, kiến trúc chuyển sang mô hình multi-gateway: nhiều ESP32 Central con, mỗi con phụ trách một nhóm thiết bị, bridge dữ liệu lên gateway chính qua WiFi (HTTP nội bộ hoặc MQTT). Khung phần mềm giai đoạn 1 cần thiết kế command dispatcher và message layer độc lập với số lượng kết nối vật lý, để dễ tái sử dụng khi chuyển sang multi-gateway.

---

## 3. Thiết bị & phần cứng

| Mục | Quyết định |
|---|---|
| Gateway chip | ESP32-S3 (ưu tiên hiệu suất, không ràng buộc chi phí) |
| Nguồn gateway | Cố định |
| Vị trí gateway | Cố định, có anten ngoài |
| Chip thiết bị DIY | ESP32 (dùng ESP-IDF), có thể đa dạng chip trong tương lai |
| Nguồn thiết bị DIY | Tùy thiết bị — pin hoặc cố định, quyết định theo từng loại thiết bị khi thiết kế |
| Số lượng thiết bị mục tiêu | Giai đoạn 1: đến ~10 thiết bị (giới hạn hiệu suất một gateway) |
| Điều kiện RF | Có thể bị cản bởi tường, hộp đựng thiết bị (box) |
| Giao tiếp phụ (ngoài BLE) | Tùy thiết bị, có thể cần UART/I2C/WiFi riêng — thiết kế firmware thiết bị theo module riêng cho từng loại giao tiếp |

### 3.1 Cân nhắc kỹ thuật liên quan
- Vì có vật cản (tường, box), cần đặt **connection interval và timeout** đủ dung sai để tránh disconnect giả khi tín hiệu suy giảm tạm thời.
- Vì thiết bị có thể dùng pin, mỗi thiết bị cần cấu hình connection parameter riêng (interval, peripheral latency) theo yêu cầu điện năng — không dùng cấu hình chung cứng cho tất cả.

---

## 4. Giao thức & định dạng dữ liệu

### 4.1 Lựa chọn format: CBOR
- **CBOR** (Concise Binary Object Representation) dùng làm format trao đổi nội bộ giữa gateway ↔ thiết bị DIY qua BLE.
- Lý do: nhỏ gọn hơn JSON (quan trọng vì MTU BLE hạn chế), tự miêu tả (self-describing, không cần schema cứng như Protobuf), dễ ánh xạ 1-1 sang JSON khi hiển thị lên web UI hoặc trả kết quả qua MCP endpoint.
- **JSON** dùng cho lớp giao diện phía trên: Web UI và MCP endpoint (JSON-RPC) — vì browser và AI Agent xử lý JSON tự nhiên.
- Lớp chuyển đổi CBOR ↔ JSON nằm ngay trong command dispatcher của gateway.

### 4.2 Bảo mật
- **BLE pairing/bonding** cơ bản giữa gateway và thiết bị DIY (chống thiết bị lạ tự động kết nối).
- Không yêu cầu mã hóa tầng ứng dụng hay bảo vệ web UI bằng mật khẩu ở giai đoạn 1 — hệ thống chỉ vận hành trong mạng LAN nội bộ, tin cậy vào cách ly mạng.
- Việc nâng cấp bảo mật (TLS cho web, auth cho MCP endpoint, mã hóa BLE Level cao hơn) để lại cho giai đoạn ổn định hóa sau này.

---

## 5. Thiết lập & quản lý gateway

### 5.1 Onboarding Wi-Fi (lần đầu thiết lập)
- ESP32-S3 phát **SoftAP** giống các thiết bị IoT tiêu chuẩn khác.
- Người dùng kết nối vào AP, mở trình duyệt để cấu hình SSID/password Wi-Fi mạng LAN.
- Sau khi cấu hình xong, gateway kết nối vào mạng LAN, chuyển sang phục vụ Web UI qua IP nội bộ (không còn cần captive portal).

### 5.2 Web UI quản lý (chạy trên HTTP server nội bộ của gateway)
Chức năng cần có:
- Thêm / xóa / sửa (add-delete-edit) thiết bị BLE đã kết nối.
- Cấu hình tên (name) và loại message (type) cho từng thiết bị.
- Xem log message realtime/gần thời gian thực từ thiết bị gửi lên qua gateway.
- Xem trạng thái kết nối của từng thiết bị (connected/disconnected, thời gian kết nối gần nhất).

### 5.3 Lưu trữ dữ liệu (NVS + RAM)
| Loại dữ liệu | Nơi lưu | Lý do |
|---|---|---|
| Cấu hình Wi-Fi | NVS (flash) | Cần giữ qua reboot |
| Danh sách thiết bị (device list, tên, type mapping) | NVS (flash) | Cấu hình quan trọng, cần giữ qua reboot |
| Buffer dữ liệu tạm khi mất kết nối (queue message chưa gửi được) | RAM | Tần suất ghi cao, không cần giữ qua reboot, tránh hao mòn flash |
| Log message gần đây (hiển thị lên Web UI) | RAM (circular buffer) | Chỉ cần xem tức thời, không cần lưu vĩnh viễn |

Ghi chú kỹ thuật: NVS trên ESP32 chịu được khoảng 100,000 lần ghi/xóa mỗi sector — đủ dùng nhiều năm nếu chỉ ghi khi có thay đổi cấu hình (add/delete/edit device), không ghi liên tục theo sự kiện dữ liệu.

---

## 6. Lớp xử lý lệnh (Command Dispatcher)

### 6.1 Nguyên tắc thiết kế
Gateway không phải là MCP server đầy đủ, không phải business logic server — chỉ là **bộ định tuyến lệnh (dispatcher)** dựa trên trường `type` trong message. Toàn bộ lệnh, bất kể đến từ Web UI hay từ MCP endpoint, đều đi qua **cùng một lớp dispatcher nội bộ** — hai lối vào (entry point) khác nhau, một lớp xử lý chung.

### 6.2 Hai nhóm loại lệnh (message type)

**Nhóm 1 — Device Command:** lệnh dành cho thiết bị DIY con.
- Gateway parse `device_id` (hoặc identifier tương đương) trong message.
- Forward payload (dạng CBOR) xuống đúng thiết bị qua BLE Central (Write Command / Notify tùy chiều).
- Ví dụ: bật/tắt thiết bị A, set giá trị số, đọc trạng thái cảm biến.

**Nhóm 2 — Gateway Command:** lệnh dành cho chính gateway.
- Tra vào bảng hàm nội bộ đã đăng ký sẵn (registry pattern), chạy trực tiếp không cần forward.
- Ví dụ: kiểm tra trạng thái gateway, list thiết bị đang kết nối, add/delete/edit device.

### 6.3 Cơ chế đăng ký hàm (đơn giản, dễ mở rộng)
- Dùng một bảng ánh xạ `type → function pointer` (command registry), đăng ký lúc khởi động (`app_main`).
- Thêm lệnh mới = thêm một entry vào registry + viết hàm xử lý, không cần sửa logic dispatcher trung tâm.
- Đây là điểm quan trọng để đảm bảo "khung dễ bảo trì, dễ mở rộng" như yêu cầu ban đầu.

---

## 7. Tích hợp MCP (AI Agent)

### 7.1 Mô hình kết nối
AI Agent kết nối tới ESP32 gateway **qua mạng LAN/WiFi** (không qua BLE trực tiếp), dùng chung hạ tầng HTTP server với Web UI quản lý.

### 7.2 Endpoint
- Gateway expose một endpoint HTTP riêng, ví dụ `/mcp` hoặc `/api/mcp`.
- Nhận request theo **JSON-RPC 2.0 tối giản** — chỉ implement phần cần thiết cho tool-call cơ bản (ví dụ `list_tools`, `call_tool`), không cần triển khai đầy đủ spec MCP (không cần resources, prompts, streaming/SSE).
- Trả kết quả qua HTTP response theo format JSON-RPC.

### 7.3 Luồng xử lý
1. AI Agent gửi JSON-RPC request tới `/mcp` (ví dụ: gọi tool "toggle_device" với tham số device_id).
2. Gateway parse request, chuyển thành message nội bộ (type + payload, dạng CBOR).
3. Đưa vào **Command Dispatcher** (mục 6) — dùng chung với Web UI.
4. Dispatcher xác định là Device Command hay Gateway Command, thực thi tương ứng.
5. Trả kết quả ngược lại AI Agent dưới dạng JSON-RPC response.

### 7.4 Vai trò MCP layer thật
Gateway **không phải MCP server đầy đủ** — chỉ đóng vai trò thực thi (executor) nhận lệnh đã được chuẩn hóa. Việc quản lý session/protocol MCP chuẩn (nếu cần đầy đủ hơn) có thể đặt ở một lớp trung gian phía trên (máy chủ khác) trong tương lai; hiện tại AI Agent gọi trực tiếp vào subset JSON-RPC tối giản này.

---

## 8. Ngăn xếp công nghệ (Tech Stack)

| Lớp | Công nghệ |
|---|---|
| Framework firmware | ESP-IDF native |
| BLE stack | NimBLE (Central role, giai đoạn 2 thêm Peripheral) |
| Web server | ESP-IDF HTTP Server (`esp_http_server`) |
| Wi-Fi onboarding | SoftAP + trang cấu hình cơ bản (không bắt buộc captive portal auto-redirect, có thể bổ sung sau) |
| Format dữ liệu nội bộ | CBOR |
| Format dữ liệu giao diện (Web UI / MCP) | JSON |
| Lưu trữ cấu hình | NVS (flash) |
| Buffer tạm | RAM (circular buffer / queue) |
| Mac app (giai đoạn 2) | Swift + CoreBluetooth |
| Bảo mật BLE | Pairing/Bonding cơ bản (không mã hóa tầng ứng dụng ở giai đoạn 1) |

---

## 9. Lộ trình phát triển (Roadmap)

1. **Nền tảng Central BLE**: Thiết lập NimBLE Central, kết nối/quản lý nhiều thiết bị DIY, đọc/ghi GATT characteristic cơ bản (Write Command + Notify).
2. **Command Dispatcher & Registry**: Xây dựng lớp phân loại message theo `type`, bảng đăng ký hàm cho Gateway Command, cơ chế forward cho Device Command.
3. **CBOR encode/decode layer**: Tích hợp thư viện CBOR (ví dụ `QCBOR` hoặc `libcbor` cho ESP-IDF), xây lớp chuyển đổi CBOR ↔ JSON.
4. **NVS storage layer**: Lưu cấu hình Wi-Fi, danh sách thiết bị, mapping tên/type; RAM buffer cho log và queue tạm.
5. **SoftAP Wi-Fi onboarding**: Flow thiết lập Wi-Fi lần đầu chuẩn IoT.
6. **Web UI quản lý**: Trang quản lý thiết bị (add/delete/edit), xem log, xem trạng thái kết nối.
7. **MCP endpoint**: Implement JSON-RPC tối giản tại `/mcp`, nối vào Command Dispatcher.
8. **Kiểm thử ổn định**: Test độ trễ, độ ổn định kết nối với 5-10 thiết bị đồng thời, tinh chỉnh connection interval/timeout theo điều kiện RF thực tế trong phòng.
9. **Giai đoạn 2 — Dual role BLE**: Thêm Peripheral role, phát triển Mac app Swift/CoreBluetooth.
10. **Giai đoạn 3 — Định hướng mở rộng**: Đánh giá kiến trúc multi-gateway nếu vượt ngưỡng thiết bị, chuẩn bị tài liệu/mã nguồn nếu chia sẻ cộng đồng maker.

---

## 10. Rủi ro kỹ thuật cần theo dõi

- **Giới hạn kết nối đồng thời của NimBLE**: cần benchmark thực tế trên ESP32-S3 khi số thiết bị tiến gần 10, theo dõi airtime contention và điều chỉnh connection interval động theo số lượng kết nối.
- **Ảnh hưởng vật cản RF** (tường, box đựng thiết bị): cần thiết lập timeout/retry hợp lý để tránh disconnect giả, có thể cần tăng công suất phát hoặc vị trí anten.
- **Hao mòn flash (NVS)**: giới hạn ghi chỉ cho dữ liệu cấu hình thay đổi không thường xuyên; log/queue tần suất cao giữ ở RAM.
- **Đồng bộ hai vai trò BLE (giai đoạn 2)**: cần kiểm tra kỹ hiệu năng khi ESP32-S3 chạy đồng thời Central (nhiều thiết bị) và Peripheral (Mac) — có thể cần phân chia rõ task/core để tránh nghẽn.
