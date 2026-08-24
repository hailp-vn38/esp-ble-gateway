# ESP32 BLE Gateway

Firmware ESP-IDF cho ESP32-S3, hoạt động như một BLE Central kết nối tới các
thiết bị DIY và cung cấp Web UI, REST API cùng endpoint JSON-RPC qua Wi-Fi.

## Chức năng

- Device Store lưu tối đa 16 thiết bị, metadata và địa chỉ BLE trong NVS; có
  migration schema và API snapshot an toàn khi nhiều task cùng truy cập.
- Wi-Fi dùng STA khi credentials đã được kiểm tra, tự fallback về SoftAP có
  captive DNS khi kết nối thất bại, và chuyển sang STA-only không cần restart.
- NimBLE Central quét theo service UUID, quản lý tối đa 9 link trên ESP32-S3,
  pairing/bonding, GATT discovery, subscribe CCCD, reconnect và discovery
  timeout.
- Message BLE là CBOR chuẩn qua QCBOR 1.6.1; JSON dùng cJSON 1.7.19~2.
- Dispatcher có registry động, định tuyến gateway/device command và chờ ACK
  riêng cho từng thiết bị.
- Web UI quản lý Wi-Fi, quét BLE, CRUD thiết bị, gửi lệnh và xem log/status.
- `POST /mcp` hỗ trợ subset JSON-RPC 2.0 gồm `list_tools`/`tools/list` và
  `call_tool`/`tools/call`, bao gồm notification không có `id`.

## Giao thức BLE

Thiết bị con cần quảng bá và triển khai các UUID 16-bit sau:

- Service: `0xABF0`
- Command characteristic (write without response): `0xABF1`
- Status characteristic (notify): `0xABF2`
- CCCD chuẩn: `0x2902`

Gateway dùng protocol version `1`. Payload CBOR là map có key số để giảm kích
thước; schema nằm trong `components/cbor_codec/cbor_codec.c`.

## Build và flash

Project được kiểm tra với ESP-IDF 6.1-beta1, target ESP32-S3 và flash 2 MiB.
QCBOR được ghim bằng Git submodule; cJSON được ESP-IDF Component Manager tải
theo manifest.

```sh
git submodule update --init --recursive
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

Nếu đổi target, cần kiểm tra lại giới hạn `CONFIG_BT_NIMBLE_MAX_CONNECTIONS`
và dung lượng partition.

## Wi-Fi lần đầu

Khi chưa có credentials hợp lệ, gateway tạo mạng provisioning:

- SSID: `ESP32-Gateway-Setup`
- Password: `gateway123`
- URL: `http://192.168.4.1/`

Gateway chỉ ghi credentials vào NVS sau khi STA thực sự nhận được IP. Sau khi
HTTP response được gửi xong, SoftAP và captive DNS được tắt, còn STA tiếp tục
hoạt động.

```sh
curl http://192.168.4.1/api/wifi/scan

curl -X POST http://192.168.4.1/api/wifi \
  -H 'Content-Type: application/json' \
  -d '{"ssid":"TEN_WIFI","password":"MAT_KHAU"}'
```

## REST API

| Method | Path | Mục đích |
|---|---|---|
| `GET` | `/` | Web UI |
| `GET` | `/api/status` | Trạng thái Wi-Fi/BLE, heap và uptime |
| `GET` | `/api/devices` | Danh sách thiết bị |
| `POST` | `/api/devices` | Thêm thiết bị |
| `PUT` | `/api/devices` | Sửa tên hoặc loại thiết bị |
| `DELETE` | `/api/devices?device_id=...` | Xóa thiết bị |
| `POST` | `/api/command` | Gửi lệnh tới thiết bị và chờ ACK |
| `GET` | `/api/logs` | Circular log gần đây |
| `GET` | `/api/wifi/scan` | Quét Wi-Fi trong provisioning mode |
| `POST` | `/api/wifi` | Kiểm tra và lưu credentials |
| `GET` | `/api/ble/scan` | Kết quả quét BLE đã cache |
| `POST` | `/api/ble/scan` | Bắt đầu quét BLE |
| `POST` | `/mcp` | JSON-RPC cho AI/tool client |

Ví dụ thêm thiết bị:

```sh
curl -X POST http://<GATEWAY_IP>/api/devices \
  -H 'Content-Type: application/json' \
  -d '{"device_id":"lamp-1","name":"Đèn bàn","type":"light","ble_addr":"11:22:33:44:55:66","ble_addr_type":0}'
```

## JSON-RPC

Liệt kê tool động từ command registry:

```sh
curl -X POST http://<GATEWAY_IP>/mcp \
  -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","method":"list_tools","id":1}'
```

Gọi theo dạng tương thích cũ:

```sh
curl -X POST http://<GATEWAY_IP>/mcp \
  -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","method":"call_tool","params":{"device_id":"lamp-1","command":"toggle","bool_value":true},"id":2}'
```

Hoặc theo dạng `name` + `arguments`:

```json
{
  "jsonrpc": "2.0",
  "method": "tools/call",
  "params": {
    "name": "get_status",
    "arguments": {}
  },
  "id": "status-1"
}
```

Endpoint này là JSON-RPC/MCP subset cho LAN, không phải MCP server đầy đủ và
chưa có authentication. Không nên expose trực tiếp ra Internet.

## Unit test

Test app riêng bao phủ Device Store/NVS, CBOR-QCBOR/JSON và Dispatcher. Test
được compile tách biệt khỏi firmware production:

```sh
cd test
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

Các test tự chạy một lượt khi boot rồi mở Unity menu. Test end-to-end Wi-Fi,
BLE discovery, reconnect và ACK vẫn cần board ESP32-S3 thật cùng một peripheral
triển khai service `0xABF0`.

## Cấu trúc

```text
main/                         Khởi động và nối các module
components/device_store/      NVS device registry
components/wifi_provisioning/ Wi-Fi STA/SoftAP và captive DNS
components/ble_central/       NimBLE Central/GATT Client
components/cbor_codec/        QCBOR và JSON codec
components/command_dispatcher/ Command registry, ACK routing
components/web_server/        Web UI và REST API
components/mcp_endpoint/      JSON-RPC endpoint
components/log_buffer/        Circular log thread-safe
components/qcbor_lib/         QCBOR 1.6.1 submodule wrapper
test/                         Unity unit-test application
docs/                         Thiết kế, kế hoạch module và test plan
```
