# ESP32 BLE Gateway

Firmware ESP-IDF cho ESP32-S3, đóng vai trò BLE Central kết nối tới các thiết bị DIY và cung cấp Web UI cùng endpoint JSON-RPC qua Wi-Fi.

## Chức năng hiện có

- NimBLE Central/GATT Client, hỗ trợ tối đa 9 kết nối trên cấu hình ESP32-S3 hiện tại.
- Wi-Fi STA nếu đã có thông tin đăng nhập trong NVS; nếu chưa có, thiết bị mở SoftAP để cấu hình.
- HTTP Web UI quản lý danh sách thiết bị và xem log gần đây.
- Endpoint `/mcp` dùng JSON-RPC tối giản với `list_tools` và `call_tool`.
- Lưu cấu hình Wi-Fi và danh sách thiết bị trong NVS.
- Mã hóa message nhị phân tạm thời qua lớp `cbor_codec`.

## Cấu trúc

```text
main/
  main.c                     Điểm khởi động ứng dụng
components/
  ble_central/               NimBLE Central và GATT Client
  cbor_codec/                Chuyển đổi message nhị phân/JSON
  command_dispatcher/        Registry và định tuyến lệnh
  device_store/              Danh sách thiết bị trong NVS
  log_buffer/                Circular buffer trong RAM
  mcp_endpoint/              JSON-RPC endpoint tại /mcp
  web_server/                Web UI và REST API
  wifi_provisioning/         Wi-Fi STA/SoftAP provisioning
old_code/                    Bản source cũ được giữ lại để đối chiếu
```

## Build và flash

Project đang đặt target mặc định là ESP32-S3 và dùng partition single-app 1500 KiB trên flash 2 MiB.

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

Nếu dùng chip khác, chạy lại `idf.py set-target <chip>` và kiểm tra giới hạn kết nối NimBLE của target đó.

## Wi-Fi lần đầu

Gateway thử kết nối bằng Wi-Fi credentials trong NVS khi khởi động. Nếu chưa có
credentials hoặc không kết nối được trong thời gian cho phép, gateway tự chuyển
sang provisioning mode và tạo access point:

- SSID: `ESP32-Gateway-Setup`
- Password: `gateway123`
- Web cấu hình: `http://192.168.4.1/`

Web cấu hình cho phép quét/chọn SSID, nhập mật khẩu và kiểm tra kết nối. Gateway
chỉ lưu credentials sau khi nhận được IP từ mạng Wi-Fi đã chọn; sau đó tự khởi
động lại và chạy ở STA mode. Nếu kiểm tra thất bại, SoftAP vẫn hoạt động để người
dùng thử lại.

Có thể thao tác bằng API:

```sh
curl http://192.168.4.1/api/wifi/scan

curl -X POST http://192.168.4.1/api/wifi \
  -H 'Content-Type: application/json' \
  -d '{"ssid":"TEN_WIFI","password":"MAT_KHAU"}'
```

## HTTP API

- `GET /`: Web UI.
- `GET /api/devices`: danh sách thiết bị.
- `POST /api/devices`: thêm thiết bị.
- `DELETE /api/devices?device_id=...`: xóa thiết bị.
- `GET /api/logs`: log gần đây.
- `GET /api/status`: trạng thái gateway.
- `GET /api/wifi/scan`: quét Wi-Fi khi đang ở provisioning mode.
- `POST /api/wifi`: test Wi-Fi, lưu credentials nếu thành công và tự restart.
- `POST /mcp`: JSON-RPC tối giản.

Ví dụ gọi MCP:

```sh
curl -X POST http://<GATEWAY_IP>/mcp \
  -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","method":"list_tools","id":1}'
```

## Trạng thái triển khai

Source đã được tích hợp vào cấu trúc component chuẩn và build thành công. `device_store` có thể nhận `device_id` ở dạng địa chỉ MAC (`AA:BB:CC:DD:EE:FF`), lưu địa chỉ vào NVS và thử khôi phục kết nối BLE khi khởi động lại. Hiện Web UI chưa có trường MAC riêng, vì vậy cần dùng chính địa chỉ MAC làm `device_id` nếu muốn dùng luồng này.

`cbor_codec` hiện dùng binary layout nội bộ để kiểm thử end-to-end, chưa phải CBOR chuẩn. Khi ghép với firmware thiết bị con, cần thay bằng QCBOR/libcbor hoặc đảm bảo hai phía dùng cùng layout.
