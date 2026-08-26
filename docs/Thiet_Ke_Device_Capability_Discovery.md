# Thiết kế Device Capability Discovery

> Trạng thái: gateway side đã được triển khai. Peripheral vẫn phải triển khai
> contract CBOR v3 ở mục 5 thì gateway mới nhận được capability thực tế.

## 1. Mục tiêu

Tài liệu này mô tả cách bổ sung chức năng để gateway tự biết một BLE peripheral
hỗ trợ những lệnh nào, thay vì Web UI hoặc MCP client phải biết trước chuỗi lệnh
như `toggle` hay `set_brightness`.

Sau khi triển khai:

- peripheral tự mô tả các command mà nó hỗ trợ;
- gateway discovery lại capability sau mỗi lần BLE session chuyển sang `READY`;
- gateway lưu snapshot capability cuối cùng theo `device_id`;
- REST và Web UI hiển thị đúng control tương ứng với từng command;
- dispatcher có thể từ chối command không được device công bố;
- MCP vẫn giữ allowlist bảo mật, đồng thời kiểm tra capability của device;
- device firmware cũ vẫn kết nối và nhận command theo chính sách tương thích.

Phạm vi phiên bản đầu chỉ bao gồm **command capability** với argument thuộc một
trong ba kiểu: không có argument, boolean hoặc integer. String, float, enum nhiều
giá trị, telemetry/property và firmware update chưa thuộc phạm vi.

## 2. Hiện trạng và các ràng buộc

### 2.1. Gateway hiện chưa có capability model

`device_entry_t` chỉ lưu `device_id`, tên, loại device và BLE identity. REST
`POST /api/command` nhận một chuỗi command rồi dispatcher chuyển nguyên chuỗi đó
qua BLE. Peripheral là nơi duy nhất biết command hợp lệ và chỉ báo lại bằng ACK.

Command registry trong `command_dispatcher` là registry của **gateway command**
(`add_device`, `list_devices`, ...), không phải command của từng peripheral.

### 2.2. Wire format hiện tại

- Service: `0xABF0`.
- Gateway ghi CBOR vào characteristic `0xABF1`.
- Peripheral notify CBOR qua characteristic `0xABF2`.
- `GW_PROTOCOL_VERSION` trong code hiện tại là `2`.
- CBOR dùng numeric map key và `GW_MSG_MAX_LEN = 256`.
- Mỗi BLE packet còn bị giới hạn bởi `negotiated_mtu - 3`.
- Codec hiện bắt buộc có `type`, `command`, `int_value` và `bool_value`.
- Dispatcher chỉ cho phép một command đang chờ ACK trên mỗi device.

Vì các giới hạn trên, không gửi toàn bộ capability trong một mảng CBOR lớn.
Snapshot phải được truyền thành `begin -> nhiều item -> end`, mỗi item không quá
256 byte và không quá `MTU - 3`.

## 3. Kiến trúc đề xuất

Thêm component mới `device_capabilities`:

```text
BLE session READY
       |
       v
device_capabilities worker
       |
       | device_command: describe_capabilities
       v
ABF1 ----------------------------------------------------> Peripheral
                                                               |
ABF2 <---- capabilities_begin / capability_item* / end --------+
       |
       v
staging snapshot -- validate/commit --> capability cache + NVS
       |
       +--> REST --> Web UI
       +--> dispatcher validation
       +--> MCP validation
```

Các nguyên tắc:

1. Discovery không chạy trong NimBLE callback hoặc HTTPD task.
2. Snapshot chỉ thay thế dữ liệu cũ khi đã nhận đủ `begin`, tất cả `item` và
   `end` hợp lệ.
3. Snapshot dở dang không làm mất snapshot cuối cùng đã biết.
4. Capability do device công bố không tự động vượt qua policy bảo mật MCP.
5. Không trộn capability vào `device_entry_t`; dùng store/component riêng để
   tránh làm NVS schema của Device Store phình to.

## 4. Mô hình dữ liệu gateway

### 4.1. Giới hạn đề xuất

```c
#define DEVICE_CAP_MAX_PER_DEVICE  12
#define DEVICE_CAP_LABEL_LEN       32
#define DEVICE_CAP_UNIT_LEN        12
#define DEVICE_CAP_DISCOVERY_TIMEOUT_MS 3000
```

12 capability x 16 device đủ cho phiên bản đầu và giữ RAM ở mức dự đoán được.
Không cấp phát động cho cache chính.

### 4.2. Kiểu dữ liệu

```c
typedef enum {
    DEVICE_CAP_VALUE_NONE = 0,
    DEVICE_CAP_VALUE_BOOL = 1,
    DEVICE_CAP_VALUE_INT  = 2,
} device_cap_value_type_t;

enum {
    DEVICE_CAP_FLAG_IDEMPOTENT  = 1u << 0,
    DEVICE_CAP_FLAG_DESTRUCTIVE = 1u << 1,
};

typedef struct {
    char command[GW_MSG_COMMAND_LEN];
    char label[DEVICE_CAP_LABEL_LEN];
    char unit[DEVICE_CAP_UNIT_LEN];
    device_cap_value_type_t value_type;
    uint8_t flags;
    int32_t min_value;
    int32_t max_value;
    uint32_t step;
} device_capability_t;

typedef enum {
    DEVICE_CAP_STATE_UNKNOWN = 0,
    DEVICE_CAP_STATE_DISCOVERING,
    DEVICE_CAP_STATE_READY,
    DEVICE_CAP_STATE_STALE,
    DEVICE_CAP_STATE_UNSUPPORTED,
    DEVICE_CAP_STATE_ERROR,
} device_cap_state_t;

typedef struct {
    char device_id[DEVICE_ID_MAX_LEN];
    device_cap_state_t state;
    uint32_t revision;
    uint32_t snapshot_id;
    int64_t updated_at_ms;
    size_t count;
    device_capability_t items[DEVICE_CAP_MAX_PER_DEVICE];
} device_capability_snapshot_t;
```

Validation cho từng item:

- `command` không rỗng, nhỏ hơn `GW_MSG_COMMAND_LEN` và chỉ gồm
  `[A-Za-z0-9_.-]`;
- không cho phép command trùng nhau trong cùng snapshot;
- `label` và `unit` phải là UTF-8 hợp lệ, không chứa control character;
- kiểu `NONE` không dùng min/max/step;
- kiểu `BOOL` không dùng min/max/step;
- kiểu `INT` phải có `min <= max`, `step > 0`, và range chia được theo step;
- reject snapshot vượt `DEVICE_CAP_MAX_PER_DEVICE`, không truncate âm thầm;
- flag chưa biết phải được bỏ qua khi đọc nhưng giữ nguyên khi persist nếu cần
  forward compatibility.

## 5. Protocol BLE/CBOR

### 5.1. Nâng protocol version

Tăng `GW_PROTOCOL_VERSION` từ 2 lên 3. Gateway v3 vẫn nhận message version 1 và
2 như hiện tại. Peripheral mới dùng version 3 cho capability messages.

Không tái sử dụng key cũ với ý nghĩa mới. Bổ sung key:

| Key | Tên | Kiểu | Ghi chú |
| ---: | --- | --- | --- |
| 11 | `snapshot_id` | uint32 | ID khác 0, đổi ở mỗi snapshot |
| 12 | `sequence` | uint16 | Index item, bắt đầu từ 0 |
| 13 | `total` | uint16 | Tổng số item |
| 14 | `value_type` | uint8 | 0 none, 1 bool, 2 int |
| 15 | `flags` | uint8 | idempotent/destructive |
| 16 | `min_value` | int32 | Chỉ dùng cho integer |
| 17 | `max_value` | int32 | Chỉ dùng cho integer |
| 18 | `step` | uint32 | Chỉ dùng cho integer |
| 19 | `label` | text | Tối đa 31 byte UTF-8 |
| 20 | `unit` | text | Tối đa 11 byte UTF-8 |
| 21 | `capability_revision` | uint32 | Tăng khi schema command đổi |

Để giữ tương thích với codec envelope hiện tại, mọi capability message vẫn có
key `3` (`command`), `4` (`int_value`) và `5` (`bool_value`). Với field không
dùng, peripheral gửi `0`/`false`.

### 5.2. Gateway yêu cầu discovery

Gateway gửi một `device_command` bình thường:

```text
{
  0: 3,                         / protocol_version /
  1: "device_command",
  2: "lamp-1",
  3: "describe_capabilities",
  4: 0,
  5: false,
 10: 421                        / request_id /
}
```

`describe_capabilities` là command dành riêng cho protocol, không hiển thị như
một chức năng người dùng.

### 5.3. Peripheral trả snapshot

Peripheral phải gửi theo đúng thứ tự sau trên `0xABF2`:

1. `capabilities_begin`;
2. `total` message `capability_item` có sequence liên tục `0..total-1`;
3. `capabilities_end`;
4. cuối cùng gửi `device_ack` cho request `describe_capabilities`.

Ví dụ begin:

```text
{
  0: 3, 1: "capabilities_begin", 2: "lamp-1",
  3: "describe_capabilities", 4: 0, 5: false,
 11: 88, 13: 2, 21: 7
}
```

Item boolean:

```text
{
  0: 3, 1: "capability_item", 2: "lamp-1",
  3: "set_power", 4: 0, 5: false,
 11: 88, 12: 0, 14: 1, 15: 1,
 19: "Power", 20: ""
}
```

Item integer:

```text
{
  0: 3, 1: "capability_item", 2: "lamp-1",
  3: "set_brightness", 4: 0, 5: false,
 11: 88, 12: 1, 14: 2, 15: 1,
 16: 0, 17: 100, 18: 1,
 19: "Brightness", 20: "%"
}
```

End:

```text
{
  0: 3, 1: "capabilities_end", 2: "lamp-1",
  3: "describe_capabilities", 4: 0, 5: true,
 11: 88, 13: 2
}
```

ACK cuối cùng dùng contract hiện tại, cùng `request_id=421`, command
`describe_capabilities`, `bool_value=true`.

Nếu peripheral không hỗ trợ discovery, nó nên ACK `bool_value=false`. Device
firmware cũ có thể timeout; gateway chuyển state sang `UNSUPPORTED` sau lần thử
đầu và không retry liên tục trong cùng BLE session.

### 5.4. MTU và mất packet

- Peripheral phải kiểm tra encoded item vừa với `negotiated_mtu - 3`.
- Gateway vẫn reject packet lớn hơn 256 byte.
- Phiên bản đầu không có application fragmentation. Nếu item không vừa MTU,
  peripheral rút ngắn `label`/`unit`; nếu vẫn không vừa thì ACK fail.
- Gateway dùng `snapshot_id`, `sequence` và `total` để phát hiện thiếu, trùng hoặc
  lẫn hai snapshot.
- Duplicate item giống hệt có thể bỏ qua; duplicate cùng sequence nhưng nội dung
  khác làm snapshot lỗi.
- Nếu notify queue đầy hoặc timeout, bỏ staging snapshot và giữ cache cũ ở state
  `STALE`.

## 6. Luồng xử lý gateway

### 6.1. Khi BLE session READY

1. `ble_central` phát lifecycle event `READY(device_id)` bằng callback nhẹ.
2. Callback enqueue device ID vào worker của `device_capabilities` rồi trả ngay.
3. Worker đặt state `DISCOVERING` và submit command `describe_capabilities` qua
   `command_executor`.
4. Peripheral gửi begin/items/end; notify worker decode rồi chuyển cho capability
   manager.
5. Capability manager build staging snapshot.
6. Khi nhận end hợp lệ, manager atomically commit cache và enqueue NVS write.
7. ACK hoàn tất command executor. Nếu ACK OK nhưng chưa có end hợp lệ, coi là
   protocol error.

Không gọi `ble_central_send_command()` trực tiếp từ lifecycle callback. Đi qua
executor/dispatcher để giữ invariant một pending command trên mỗi device.

### 6.2. Routing notification

Đổi callback trong `main/main.c` theo dạng:

```c
static void on_device_notify(const char *device_id, const gw_message_t *msg)
{
    if (device_capabilities_on_notify(device_id, msg)) {
        return; // begin/item/end đã được consume
    }
    command_dispatcher_on_device_notify(device_id, msg); // ACK/event hiện tại
}
```

`device_capabilities_on_notify()` chỉ copy dữ liệu nhỏ/enqueue và không ghi NVS.
`device_ack` vẫn phải đi tới dispatcher để đánh thức pending request.

### 6.3. Reconnect và refresh thủ công

- Mỗi BLE session chỉ auto-discover tối đa một lần.
- Nếu revision và nội dung không đổi, không cần ghi NVS lại.
- REST có thể yêu cầu refresh thủ công nhưng trả `409` nếu device đang có command
  khác pending hoặc discovery đang chạy.
- Disconnect trong lúc discovery: bỏ staging, giữ snapshot cũ với state `STALE`.

## 7. Persistence

Tạo namespace NVS riêng `dev_caps`, không sửa generated `sdkconfig` và không
nhúng capability vào `device_entry_t`.

Triển khai dùng tối đa 16 slot `cap00`..`cap15`. Mỗi value là một blob có
header version cố định và chỉ serialize đúng số item thực tế (không ghi đủ 12
slot rỗng) để tiết kiệm partition NVS. Blob có:

- schema version của capability store;
- `device_id`;
- revision;
- danh sách capability đã validate.

Khi init, đọc tất cả slot, kiểm tra schema/version/size rồi dựng RAM cache. Khi
xóa device, xóa snapshot tương ứng. Ghi theo worker riêng hoặc work queue để
không block BLE notify task.

Không persist `DISCOVERING` hoặc `ERROR`; sau reboot snapshot đã lưu được load là
`STALE` cho tới khi discovery mới xác nhận lại.

## 8. Public API của component mới

```c
esp_err_t device_capabilities_init(void);

// Non-blocking lifecycle input.
esp_err_t device_capabilities_on_ready(const char *device_id);
void device_capabilities_on_disconnect(const char *device_id);

// true nếu message capability đã được consume.
bool device_capabilities_on_notify(const char *device_id,
                                   const gw_message_t *msg);

esp_err_t device_capabilities_refresh(const char *device_id);

esp_err_t device_capabilities_get(const char *device_id,
                                  device_capability_snapshot_t *out);

bool device_capabilities_supports(const char *device_id,
                                  const char *command,
                                  const device_capability_t **out_capability);

esp_err_t device_capabilities_validate_command(const gw_message_t *msg);
void device_capabilities_forget(const char *device_id);
```

API trả bản copy, không trả pointer vào cache nội bộ. Nếu cần tránh copy toàn bộ,
dùng callback snapshot dưới mutex rồi copy ra JSON ngoài mutex; không giữ mutex
trong lúc gọi BLE, NVS hoặc HTTP.

## 9. REST API

Không nhúng toàn bộ capabilities vào `GET /api/devices` vì payload hiện có giới
hạn 4096 byte. Bổ sung hai endpoint:

### 9.1. Đọc capability

```http
GET /api/capabilities?device_id=lamp-1
```

Response:

```json
{
  "success": true,
  "data": {
    "device_id": "lamp-1",
    "state": "ready",
    "revision": 7,
    "stale": false,
    "commands": [
      {
        "name": "set_power",
        "label": "Power",
        "value_type": "boolean",
        "idempotent": true,
        "destructive": false
      },
      {
        "name": "set_brightness",
        "label": "Brightness",
        "value_type": "integer",
        "minimum": 0,
        "maximum": 100,
        "step": 1,
        "unit": "%"
      }
    ]
  }
}
```

State chưa discovery vẫn trả HTTP 200 với `state: "unknown"` và mảng rỗng.
Device ID không tồn tại trả 404.

### 9.2. Refresh capability

```http
POST /api/capabilities/refresh
Content-Type: application/json

{"device_id":"lamp-1"}
```

Trả `202 Accepted` khi đã enqueue. Kết quả được đọc lại qua GET. Device offline
trả 409 hoặc 502 theo mapping hiện có; queue đầy trả 503.

### 9.3. Validate `POST /api/command`

Trước khi submit executor:

- state `READY`: command phải tồn tại và argument phải đúng type/range;
- state `STALE`: cho phép command có trong snapshot cũ, response/log ghi rõ stale;
- state `UNKNOWN` hoặc `UNSUPPORTED`: dùng policy tương thích;
- command không tồn tại: HTTP 400, code `unsupported_command`;
- sai argument: HTTP 400, code `invalid_command_argument`.

Policy mặc định nên là `known_only`: enforce khi có snapshot, nhưng vẫn cho
device firmware cũ chạy như trước. Có thể thêm Kconfig:

```text
CONFIG_DEVICE_CAPABILITY_ENFORCEMENT_KNOWN_ONLY=y
# CONFIG_DEVICE_CAPABILITY_ENFORCEMENT_STRICT is not set
```

Strict mode từ chối command khi chưa có snapshot; chỉ bật sau khi toàn bộ
peripheral đã hỗ trợ protocol v3.

## 10. MCP

Giữ một tool chung `device_command`; không tạo tool động cho từng command vì
`tools/list` là global trong khi capability phụ thuộc `device_id` và có cache.

Bổ sung tool read-only:

```text
list_device_capabilities(device_id)
```

Khi gọi `device_command`, command phải thỏa cả hai điều kiện:

```text
CONFIG_MCP_DEVICE_COMMAND_ALLOWLIST
            AND
device capability/policy
```

Capability là dữ liệu do peripheral cung cấp nên **không được** dùng để mở rộng
allowlist. Ví dụ device quảng bá `factory_reset` nhưng allowlist không có thì MCP
vẫn phải từ chối.

Schema của `device_command` tiếp tục nhận `bool_value` hoặc integer `value`.
Validate type/range dựa trên snapshot sau khi parse params và trước khi enqueue.

## 11. Web UI

Trong màn hình chi tiết device:

- `NONE`: render button;
- `BOOL`: render switch hoặc hai nút on/off;
- `INT`: render number input/slider theo min, max, step và unit;
- destructive flag: hiện confirm dialog;
- state unknown/discovering: hiện trạng thái và nút Refresh;
- state stale: vẫn hiển thị control nhưng có badge “Last known”;
- unsupported: giữ ô custom command để tương thích device cũ;
- ready: mặc định ẩn custom command khỏi người dùng phổ thông.

Không đưa `label` từ peripheral vào `innerHTML`. Dùng `textContent` hoặc escape
đầy đủ vì capability là input không tin cậy.

## 12. Thay đổi theo component

### `components/cbor_codec`

- tăng protocol version lên 3;
- thêm field/key capability vào model hoặc tạo `gw_capability_message_t` union;
- làm field capability optional theo `type`, không bắt field không liên quan;
- thêm encode/decode và boundary validation;
- bảo đảm decoder v3 vẫn đọc message v1/v2.

Khuyến nghị tách validation theo message type thay vì tiếp tục làm
`gw_message_t` phình vô hạn.

### `components/device_capabilities` (mới)

- RAM cache + staging snapshot;
- worker queue cho ready/refresh/persist;
- state machine, timeout, sequence validation;
- NVS codec/store;
- API query/validate/forget;
- `CMakeLists.txt` khai báo dependency thực tế để không bị loại bởi
  `MINIMAL_BUILD`.

### `components/ble_central`

- phát lifecycle callback/event khi slot thật sự `READY`, không phải chỉ ACL
  connected;
- phát disconnect event để capability manager hủy staging;
- expose negotiated MTU nếu cần chẩn đoán item quá lớn;
- callback chỉ enqueue, không discovery/NVS trực tiếp.

### `components/command_dispatcher`

- cho phép reserved query `describe_capabilities` đi qua flow ACK hiện tại;
- gọi capability validator trước command người dùng;
- thêm status/error cho unsupported command và invalid argument nếu cần;
- khi `delete_device` thành công, gọi `device_capabilities_forget()`.

Tránh dependency cycle. Một cách sạch là dispatcher phụ thuộc interface nhỏ
`device_capabilities`; component này không gọi dispatcher trực tiếp mà nhận hook
submit được inject từ `main` hoặc một application coordinator.

### `components/command_executor`

- dùng executor cho auto-discovery để không block BLE/HTTP task;
- completion callback cập nhật state unsupported/timeout/error;
- bảo đảm worker đã init trước khi reconnect supervisor có thể phát READY.

### `components/web_server`

- thêm GET capability và POST refresh;
- validate REST command;
- cập nhật response error code;
- đăng ký URI mới và kiểm tra `CONFIG_HTTPD_MAX_URI_HANDLERS`.

### `components/mcp_endpoint`

- thêm `list_device_capabilities` vào strict MCP registry;
- intersect allowlist với capability policy;
- sinh JSON schema/result có giới hạn, không trả pointer cache nội bộ.

### `main`

Thứ tự init đề xuất trong STA mode:

```text
device_store
device_capabilities
command_dispatcher + freeze
command_executor
ble_central
reconnect supervisor
web server + MCP
```

Provisioning mode không init Device Store, Dispatcher, BLE hoặc capability
manager, đúng kiến trúc boot hiện tại. Các HTTP route capability không được đăng
ký trong provisioning server, hoặc phải trả service-not-ready mà không truy cập
module chưa init.

## 13. State machine

```text
             READY event / manual refresh
 UNKNOWN ----------------------------------> DISCOVERING
    ^                                             |
    |                                             | begin/items/end hợp lệ
    |                                             v
 UNSUPPORTED <--- ACK reject/legacy timeout     READY
                                                  |
                                                  | disconnect / partial refresh fail
                                                  v
                                                STALE
                                                  |
                                                  +---- refresh ----> DISCOVERING

 DISCOVERING -- malformed/overflow/timeout with no old cache --> ERROR
```

Nếu đã có cache, mọi lỗi refresh dẫn tới `STALE`, không xóa items.

## 14. Kế hoạch test

### 14.1. Unit test `cbor_codec`

- round-trip begin/item/end v3;
- vẫn decode fixtures v1/v2;
- reject version > 3;
- reject oversized string, invalid value type, invalid range;
- packet đúng 256 byte và packet 257 byte;
- optional key/unknown key không làm hỏng decoder.

### 14.2. Unit test `device_capabilities`

- happy path 0, 1 và 12 capability;
- overflow 13 item không truncate;
- missing sequence, out-of-order, duplicate identical, duplicate conflicting;
- snapshot ID đổi giữa chừng;
- begin mới hủy staging cũ nhưng không xóa committed cache;
- timeout/disconnect giữ cache và đánh stale;
- command validation NONE/BOOL/INT và boundary min/max/step;
- concurrent read trong lúc commit;
- NVS reload, corrupt blob, schema quá mới và delete cleanup.

### 14.3. Unit test dispatcher/REST/MCP

- READY + supported command được gửi;
- READY + unknown command bị từ chối trước BLE;
- sai bool/int/range bị từ chối;
- unknown/unsupported theo known-only và strict policy;
- MCP allowlist có nhưng device không advertise: từ chối;
- device advertise nhưng allowlist không có: từ chối;
- capability endpoint không tồn tại/offline/discovering/ready/stale;
- body/output limit và escaping label độc hại.

Nhớ thêm component test mới vào `TEST_COMPONENTS` trong `test/CMakeLists.txt` vì
test project dùng danh sách hardcode.

### 14.4. Hardware integration

Dùng ESP32-S3 gateway và peripheral simulator `0xABF0/ABF1/ABF2`:

1. connect, secure, discover GATT, subscribe;
2. xác nhận gateway tự gửi `describe_capabilities` sau READY;
3. peripheral trả hai capability và ACK;
4. GET REST trả đúng snapshot;
5. UI render toggle + slider;
6. gửi supported command và nhận ACK;
7. gửi command ngoài list và xác nhận không có BLE write;
8. drop một item, gateway giữ snapshot cũ ở stale;
9. disconnect/reconnect và revision thay đổi;
10. thử MTU thấp để xác nhận lỗi được báo rõ, không overflow;
11. reboot gateway và xác nhận snapshot NVS load ở stale rồi được refresh;
12. device firmware v2 không hỗ trợ discovery vẫn dùng command theo known-only.

## 15. Lộ trình triển khai

### Giai đoạn 1 — Protocol và cache

1. Chốt numeric keys và peripheral contract.
2. Mở rộng codec v3 và fixtures v1/v2.
3. Tạo component/cache/state machine và unit test.
4. Chưa enforce command; chỉ log capability nhận được.

### Giai đoạn 2 — Discovery lifecycle và persistence

1. Thêm BLE READY/disconnect event.
2. Auto-submit `describe_capabilities` qua executor.
3. Commit snapshot và persist NVS.
4. Hardware test packet loss, reconnect và MTU.

### Giai đoạn 3 — REST và Web UI

1. Thêm GET/refresh endpoint.
2. Render control động.
3. Bật validation `known_only` cho REST.

### Giai đoạn 4 — MCP và strict rollout

1. Thêm tool đọc capability.
2. Intersect capability với MCP allowlist.
3. Nâng toàn bộ peripheral lên v3.
4. Sau giai đoạn quan sát, cân nhắc bật strict enforcement.

## 16. Tiêu chí hoàn thành

Chức năng được xem là hoàn thành khi:

- gateway tự discovery sau BLE READY mà không block NimBLE/HTTPD;
- snapshot thiếu hoặc lỗi không thay thế snapshot tốt trước đó;
- REST, MCP và UI cùng đọc từ một capability source of truth;
- command được validate đúng type/range trước khi BLE write;
- MCP luôn giữ allowlist độc lập với dữ liệu peripheral;
- firmware v1/v2 tiếp tục hoạt động theo policy đã chọn;
- test app build được và các test codec/cache/dispatcher/REST/MCP chạy trên board;
- root firmware build với ESP-IDF 5.4.4, target ESP32-S3 và không bị component
  mới loại khỏi dependency graph bởi `MINIMAL_BUILD`.
