# Luồng kết nối Device với ESP32 BLE Gateway

> Trạng thái triển khai: gateway đã hỗ trợ đầy đủ luồng kết nối BLE Central và
> Device Capability Discovery protocol v3. Peripheral phải triển khai đúng
> service GATT và contract CBOR trong tài liệu này để gateway nhận được danh
> sách chức năng thực tế.

## 1. Mục đích và phạm vi

Tài liệu mô tả toàn bộ vòng đời từ lúc một BLE peripheral được phát hiện đến
khi gateway biết peripheral hỗ trợ những command nào và có thể gửi command đã
được kiểm tra.

Luồng chỉ khởi động khi gateway đã vào chế độ STA và nhận được địa chỉ IP. Trong
provisioning mode, `device_store`, `device_capabilities`, `command_dispatcher`,
`ble_central` và reconnect supervisor chưa được khởi tạo.

## 2. Tổng quan

```text
Peripheral                               ESP32 BLE Gateway
    |                                            |
    | advertise service 0xABF0                  |
    | -----------------------------------------> | scan và lọc thiết bị
    |                                            |
    | <----------------------------------------- | GAP connect
    |       MTU exchange + security/bonding      |
    | <----------------------------------------> |
    |                                            |
    | <----------------------------------------- | GATT discovery 0xABF0
    | <----------------------------------------- | tìm 0xABF1, 0xABF2, CCCD
    | <----------------------------------------- | ghi CCCD 0x0001
    |                                            | trạng thái READY
    |                                            |
    | <--- 0xABF1: describe_capabilities ------- |
    |                                            |
    | ---- 0xABF2: capabilities_begin --------> |
    | ---- 0xABF2: capability_item (0..N-1) --> |
    | ---- 0xABF2: capabilities_end ----------> |
    | ---- 0xABF2: device_ack ----------------> |
    |                                            | validate + commit RAM/NVS
    |                                            | Web UI/REST/MCP có capability
    |                                            |
    | <--- 0xABF1: device command hợp lệ ------- |
    | ---- 0xABF2: device_ack/device_event ----> |
```

## 3. Yêu cầu phía peripheral

### 3.1. Advertising và GATT

Peripheral phải advertise UUID `0xABF0` và cung cấp cấu trúc GATT sau:

```text
Service 0xABF0
├── COMMAND 0xABF1
│   └── Property bắt buộc: Write Without Response
└── STATUS 0xABF2
    └── Property bắt buộc: Notify
        └── CCCD 0x2902
```

Gateway sẽ kết thúc connection nếu thiếu service, characteristic, CCCD hoặc
characteristic không có đúng property.

### 3.2. BLE security

Gateway bật bonding và LE Secure Connections với IO capability `NO_IO`, không
yêu cầu MITM. Peripheral phải chấp nhận quy trình security này. GATT discovery
chỉ bắt đầu sau khi link encryption thành công.

### 3.3. Wire format

- Gateway ghi message CBOR vào `0xABF1`.
- Peripheral gửi message CBOR bằng Notify qua `0xABF2`.
- CBOR dùng numeric map key.
- Protocol capability sử dụng version `3`.
- Payload tối đa phía gateway là 256 byte và phải không vượt
  `negotiated_mtu - 3`.

## 4. Ghép device lần đầu

### 4.1. Scan

Web UI hoặc REST client bắt đầu scan bằng:

```http
POST /api/ble/scan
```

Gateway active-scan trong 6 giây và chỉ giữ advertisement có UUID `0xABF0`.
Đọc kết quả bằng:

```http
GET /api/ble/scan
```

Ví dụ kết quả:

```json
{
  "success": true,
  "scanning": false,
  "devices": [
    {
      "name": "Lamp-01",
      "ble_addr": "AA:BB:CC:DD:EE:FF",
      "addr_type": 0,
      "rssi": -48
    }
  ]
}
```

### 4.2. Đăng ký device

Client chọn kết quả scan và tạo device:

```http
POST /api/devices
Content-Type: application/json

{
  "device_id": "lamp-01",
  "name": "Đèn phòng khách",
  "device_type": "light",
  "ble_addr": "AA:BB:CC:DD:EE:FF",
  "ble_addr_type": 0
}
```

Gateway lưu metadata và BLE identity vào `device_store` trong NVS. Sau khi lưu
thành công, gateway yêu cầu kết nối ngay theo kiểu best-effort. Trường
`persisted: true` trong response xác nhận đã lưu; `connect_requested` cho biết
yêu cầu kết nối đã được khởi tạo hay chưa.

`device_id` là logical identifier do hệ thống đặt, không phải BLE address.

## 5. Thiết lập BLE connection

Connection đi qua state machine:

```text
FREE -> CONNECTING -> SECURING -> DISCOVERING -> READY
                      |              |
                      +--------------+----> DISCONNECTED/BACKOFF khi lỗi
```

Sau khi GAP connect thành công, gateway:

1. lưu canonical peer identity bất đồng bộ;
2. yêu cầu MTU exchange;
3. bắt đầu BLE security và bonding;
4. sau khi encryption thành công, discovery service `0xABF0`;
5. tìm `0xABF1` và xác nhận `WRITE_NO_RSP`;
6. tìm `0xABF2` và xác nhận `NOTIFY`;
7. tìm CCCD `0x2902` của `0xABF2`;
8. ghi `{0x01, 0x00}` vào CCCD để bật Notify;
9. chuyển connection sang `READY` và phát lifecycle callback.

Security và GATT discovery có timeout riêng 10 giây. Khi timeout hoặc validation
GATT thất bại, gateway terminate connection và reconnect supervisor thử lại.

## 6. Capability discovery sau READY

### 6.1. Request từ gateway

Sau lifecycle event `READY(device_id)`, capability worker tự enqueue discovery.
Gateway chỉ chạy một discovery tại một thời điểm; các device khác được xếp hàng.

Gateway gửi một `device_command` protocol v3 qua `0xABF1`:

```text
{
  0: 3,                         / protocol_version /
  1: "device_command",          / type /
  2: "lamp-01",                 / device_id /
  3: "describe_capabilities",   / command /
  4: 0,
  5: false,
 10: 421                        / request_id /
}
```

`describe_capabilities` là command dành riêng cho protocol và không được hiển
thị như một chức năng điều khiển cho người dùng.

### 6.2. Response bắt buộc từ peripheral

Peripheral phải Notify đúng thứ tự:

```text
capabilities_begin
capability_item, sequence = 0
capability_item, sequence = 1
...
capability_item, sequence = total - 1
capabilities_end
device_ack cho describe_capabilities
```

Ví dụ peripheral có hai chức năng `set_power` và `set_brightness`.

Begin:

```text
{
  0: 3, 1: "capabilities_begin", 2: "lamp-01",
  3: "describe_capabilities", 4: 0, 5: false,
 11: 88, 13: 2, 21: 7
}
```

Item boolean:

```text
{
  0: 3, 1: "capability_item", 2: "lamp-01",
  3: "set_power", 4: 0, 5: false,
 11: 88, 12: 0, 14: 1, 15: 1,
 19: "Nguồn", 20: ""
}
```

Item integer:

```text
{
  0: 3, 1: "capability_item", 2: "lamp-01",
  3: "set_brightness", 4: 0, 5: false,
 11: 88, 12: 1, 14: 2, 15: 1,
 16: 0, 17: 100, 18: 1,
 19: "Độ sáng", 20: "%"
}
```

End:

```text
{
  0: 3, 1: "capabilities_end", 2: "lamp-01",
  3: "describe_capabilities", 4: 0, 5: true,
 11: 88, 13: 2
}
```

Cuối cùng, peripheral gửi `device_ack` theo contract ACK hiện tại, với cùng
`request_id = 421`, command `describe_capabilities` và `bool_value = true`.

### 6.3. Các CBOR key của capability

| Key | Field | Kiểu | Ý nghĩa |
| ---: | --- | --- | --- |
| 11 | `snapshot_id` | uint32 | ID của một lần tạo snapshot |
| 12 | `sequence` | uint16 | Thứ tự item, bắt đầu từ 0 |
| 13 | `total` | uint16 | Tổng số capability |
| 14 | `value_type` | uint8 | 0 none, 1 bool, 2 integer |
| 15 | `flags` | uint8 | bit 0 idempotent, bit 1 destructive |
| 16 | `min_value` | int32 | Giá trị nhỏ nhất của integer |
| 17 | `max_value` | int32 | Giá trị lớn nhất của integer |
| 18 | `step` | uint32 | Bước hợp lệ, phải lớn hơn 0 |
| 19 | `label` | text | Nhãn hiển thị, tối đa 31 byte |
| 20 | `unit` | text | Đơn vị, tối đa 11 byte |
| 21 | `capability_revision` | uint32 | Tăng khi contract command thay đổi |

Gateway hỗ trợ tối đa 12 capability cho mỗi device.

## 7. Validation và commit snapshot

Gateway dựng response trong vùng staging và chỉ commit khi toàn bộ snapshot hợp
lệ. Các điều kiện chính:

- protocol version từ 3 trở lên;
- `device_id` đúng với connection gửi Notify;
- `snapshot_id` giống nhau trong begin, item và end;
- `sequence` liên tục từ 0 đến `total - 1`;
- số item nhận được đúng bằng `total`;
- command không rỗng, đúng ký tự cho phép và không trùng;
- integer có `min_value <= max_value` và `step > 0`;
- tổng số item không vượt 12.

Snapshot hợp lệ được commit vào RAM và persist trong namespace NVS `dev_caps`.
Nếu refresh lỗi hoặc thiếu Notify, snapshot tốt trước đó không bị ghi đè.

## 8. Gửi command sau discovery

Trước khi gửi một command xuống BLE, dispatcher kiểm tra snapshot của device:

1. command có nằm trong danh sách peripheral quảng bá không;
2. argument có đúng kiểu `none`, `bool` hoặc `integer` không;
3. integer có nằm trong `[min_value, max_value]` và đúng `step` không.

Command không hợp lệ bị từ chối tại gateway và không được ghi vào `0xABF1`.
Command hợp lệ được gắn `request_id`, encode CBOR và gửi. Peripheral phải trả
`device_ack` có cùng `device_id`, `command` và `request_id` qua `0xABF2`.

Mỗi device chỉ có một request đang chờ ACK. ACK timeout hiện tại là 2 giây.

Nếu chưa từng có capability snapshot, gateway giữ chính sách tương thích
`known_only`: command thông thường vẫn được gửi theo protocol v2 để hỗ trợ
firmware peripheral cũ. Sau khi có snapshot, command sử dụng protocol v3 và bị
kiểm tra nghiêm ngặt.

## 9. REST API capability

Đọc snapshot hiện tại:

```http
GET /api/capabilities?device_id=lamp-01
```

Yêu cầu discovery lại khi device đang kết nối:

```http
POST /api/capabilities/refresh
Content-Type: application/json

{
  "device_id": "lamp-01"
}
```

Refresh thành công được enqueue trả HTTP `202 Accepted`. Nếu device offline,
gateway trả lỗi `device_not_connected`; nếu discovery đang chạy, gateway trả
`device_busy`.

Các trạng thái capability có thể xuất hiện:

| State | Ý nghĩa |
| --- | --- |
| `unknown` | Chưa có kết quả trong BLE session hiện tại |
| `discovering` | Đang yêu cầu và nhận snapshot |
| `ready` | Snapshot mới nhất hoàn chỉnh và hợp lệ |
| `stale` | Có snapshot cũ nhưng refresh hiện tại lỗi |
| `unsupported` | Peripheral từ chối hoặc timeout discovery |
| `error` | Lỗi protocol, queue hoặc snapshot không hợp lệ |

## 10. Disconnect và reconnect

Khi mất kết nối:

1. connection slot được giải phóng;
2. capability manager kết thúc trạng thái discovery của session;
3. device runtime chuyển sang backoff;
4. reconnect supervisor thử lại theo chu kỳ `2 -> 4 -> 8 -> 16 -> 30` giây;
5. sau khi connection mới đạt `READY`, gateway discovery capability lại một lần.

Gateway chỉ giữ connection slot trong lifetime BLE đang hoạt động. Device
offline không chiếm một trong chín slot connection.

## 11. Pipeline nhận Notify

NimBLE host callback không decode CBOR và không chờ ACK:

```text
NOTIFY_RX
  -> kiểm tra connection generation, handle và độ dài
  -> copy payload vào queue không blocking
  -> notify worker decode CBOR
  -> capability manager xử lý begin/item/end
     hoặc command dispatcher xử lý ACK/event
```

Queue Notify có depth 8. Khi queue đầy, gateway drop message mới nhất để tránh
block NimBLE host; request liên quan sẽ timeout hoặc snapshot bị đánh dấu lỗi.

## 12. Checklist triển khai peripheral

- [ ] Advertise service UUID `0xABF0`.
- [ ] Cung cấp `0xABF1` với `Write Without Response`.
- [ ] Cung cấp `0xABF2` với `Notify` và CCCD `0x2902`.
- [ ] Hỗ trợ BLE security/bonding của gateway.
- [ ] Decode CBOR numeric-map nhận từ `0xABF1`.
- [ ] Nhận command `describe_capabilities` protocol v3.
- [ ] Notify begin, đủ item liên tục, end, rồi ACK theo đúng thứ tự.
- [ ] Giữ mỗi message không quá `min(256, negotiated_mtu - 3)` byte.
- [ ] Trả ACK đúng `request_id` cho mọi device command.
- [ ] Tăng `capability_revision` khi danh sách hoặc schema command thay đổi.
- [ ] Thử disconnect/reconnect và xác nhận discovery chạy lại.

## 13. Mã nguồn liên quan

- `components/ble_central/ble_central_scan.c`: scan và lọc UUID `0xABF0`.
- `components/ble_central/ble_central_gap.c`: connect, security và Notify RX.
- `components/ble_central/ble_central_gatt.c`: GATT discovery và bật CCCD.
- `components/device_capabilities/device_capabilities.c`: discovery, staging,
  validation, cache và NVS.
- `components/cbor_codec/cbor_codec.c`: schema CBOR protocol v3.
- `components/command_dispatcher/device_command.c`: validation, gửi lệnh và ACK.
- `components/web_server/web_ble_api.c`: REST API scan.
- `components/web_server/web_gateway_api.c`: REST API device và capability.
- `main/main.c`: wiring lifecycle callback giữa BLE và capability manager.

Thiết kế chi tiết capability xem thêm tại
`docs/Thiet_Ke_Device_Capability_Discovery.md`.
