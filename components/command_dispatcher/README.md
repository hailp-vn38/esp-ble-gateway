# Command Dispatcher

## 1. Tổng quan

`command_dispatcher` là lớp trung gian chịu trách nhiệm nhận một `gw_message_t` và quyết định lệnh đó:

* được thực thi trực tiếp trên ESP32 Gateway, hoặc
* được chuyển tiếp qua BLE tới một device.

Component hiện hỗ trợ hai loại message:

```text
gateway_command
device_command
```

Luồng tổng quát:

```text
                     gw_message_t
                          │
                          ▼
              command_dispatcher_handle()
                          │
              ┌───────────┴───────────┐
              │                       │
      type=gateway_command     type=device_command
              │                       │
              ▼                       ▼
    gateway_command_handle()   device_command_handle()
              │                       │
              ▼                       ▼
      Command Registry          BLE Central
              │                       │
              ▼                       ▼
      handler function          BLE Device
                                      │
                                      ▼
                                     ACK
                                      │
                                      ▼
                     command_dispatcher_on_device_notify()
```

Dispatcher khởi tạo command registry, hệ thống ACK cho device command và đăng ký các gateway command mặc định trong `command_dispatcher_init()`.

---

# 2. Các file chính

Component hiện gồm:

```text
components/command_dispatcher/
│
├── command_dispatcher.c
├── command_dispatcher_internal.h
├── command_registry.c
├── device_command.c
├── device_request_manager.c
├── device_request_manager.h
├── gateway_commands.c
├── CMakeLists.txt
│
├── include/
│   └── command_dispatcher.h
│
└── test/
    ├── test_command_dispatcher.c
    └── test_device_request_manager.c
```

Vai trò:

| File                            | Chức năng                                    |
| ------------------------------- | -------------------------------------------- |
| `command_dispatcher.c`          | Entry point, validation boundary, phân loại message |
| `command_registry.c`            | Registry các gateway command (freeze sau init) |
| `gateway_commands.c`            | Implementation các command chạy trên Gateway |
| `device_command.c`              | Gửi command qua BLE và chờ ACK               |
| `device_request_manager.c/.h`   | Pending request table + ACK correlation bằng request_id |
| `command_dispatcher_internal.h` | API nội bộ                                   |
| `include/command_dispatcher.h`  | Public API                                   |

---

# 3. Public API

Public API được định nghĩa trong:

```c
#include "command_dispatcher.h"
```

Các hàm chính:

```c
int command_dispatcher_init(void);

int command_dispatcher_register(
    const char *command_name,
    gateway_command_fn_t fn
);

int command_dispatcher_freeze_registry(void);

int command_dispatcher_get_registered_names(
    char out_names[][GW_MSG_COMMAND_LEN],
    int max_names
);

bool command_dispatcher_is_registered(
    const char *command_name
);

void command_dispatcher_handle(
    const gw_message_t *msg,
    dispatch_result_t *result
);

void command_dispatcher_set_text_result(...);
void command_dispatcher_set_json_result(...);

void command_dispatcher_on_device_notify(
    const char *device_id,
    const gw_message_t *msg
);
```

Contract quan trọng:

* `command_dispatcher_init()` là **single-shot**: gọi lần hai trả `ESP_ERR_INVALID_STATE`.
* Registry phải được **freeze** bằng `command_dispatcher_freeze_registry()` trước khi `command_dispatcher_handle()` chấp nhận command; trước freeze, handle trả `DISPATCH_STATUS_INTERNAL_ERROR`.
* Sau freeze, `command_dispatcher_register()` trả lỗi.
* `command_dispatcher_get_registered_names()` là copy-out API: caller sở hữu bộ nhớ, không có pointer lifetime mơ hồ.

Các giới hạn quan trọng:

```c
#define DISPATCHER_MAX_RESULT_LEN 4096
#define DISPATCHER_MAX_COMMANDS     16
#define DISPATCHER_ACK_TIMEOUT_MS 2000
```

Nghĩa là:

* tối đa **16 gateway command** trong registry;
* message kết quả tối đa **4096 byte**;
* device command đợi ACK tối đa **2000 ms**.

---

# 4. Cấu trúc message

Dispatcher không định nghĩa protocol riêng mà sử dụng:

```c
gw_message_t
```

từ component `cbor_codec`.

Cấu trúc hiện tại:

```c
typedef struct {
    uint8_t protocol_version;

    char type[GW_MSG_TYPE_LEN];

    char device_id[GW_MSG_DEVICE_ID_LEN];

    char command[GW_MSG_COMMAND_LEN];

    uint32_t request_id;
    int has_request_id;

    int int_value;
    int bool_value;

    int has_device_id;

    char name[GW_MSG_NAME_LEN];

    char device_type[GW_MSG_DEVICE_TYPE_LEN];

    uint8_t ble_addr[6];
    uint8_t ble_addr_type;

    int has_ble_addr;
} gw_message_t;
```

Các giới hạn:

```text
message        : 256 bytes
type           : 24 chars
device_id      : 32 chars
command        : 32 chars
request_id     : uint32, != 0
name           : 32 chars
device_type    : 16 chars
protocol       : version 2
```

---

# 5. Hai loại command

## 5.1. Gateway Command

Gateway command được xử lý trực tiếp trên ESP32.

Ví dụ:

```json
{
    "type": "gateway_command",
    "command": "get_status"
}
```

Dispatcher thực hiện:

```text
command_dispatcher_handle()
        │
        ▼
gateway_command_handle()
        │
        ▼
command_registry_find("get_status")
        │
        ▼
cmd_get_status()
```

`gateway_command_handle()` tìm function tương ứng trong registry và gọi function đó. Nếu command không tồn tại:

```text
Unknown gateway command: <command>
```

---

# 6. Gateway command mặc định

Hiện tại dispatcher tự đăng ký 5 command khi khởi động:

```text
add_device
delete_device
edit_device
list_devices
get_status
```

Việc đăng ký xảy ra trong:

```c
gateway_commands_register_defaults();
```

---

## 6.1. `add_device`

Thêm BLE device vào gateway.

Ví dụ:

```json
{
    "type": "gateway_command",
    "command": "add_device",
    "device_id": "sensor_01",
    "name": "Temperature Sensor",
    "device_type": "sensor"
}
```

`device_id` là bắt buộc.

Nếu không truyền `name`:

```text
name = device_id
```

Nếu không truyền `device_type`:

```text
device_type = "generic"
```

Nếu có BLE address, dispatcher cũng lưu address và yêu cầu BLE Central kết nối tới device.

Ví dụ:

```json
{
    "type": "gateway_command",
    "command": "add_device",
    "device_id": "sensor_01",
    "name": "Living Room",
    "device_type": "sensor",
    "ble_addr": "AA:BB:CC:DD:EE:FF",
    "ble_addr_type": 0
}
```

Thành công: result là JSON payload mô tả kết quả persistence và connection side effect:

```json
{
    "device_id": "sensor_01",
    "persisted": true,
    "connect_requested": true
}
```

`add_device` là persistent operation; BLE connect chỉ là best-effort side effect. `status` tổng thể vẫn là `DISPATCH_STATUS_OK` nếu device được persist, kể cả khi `connect_requested = false`.

---

## 6.2. `delete_device`

Xóa device khỏi `device_store` và xóa BLE bond.

```json
{
    "type": "gateway_command",
    "command": "delete_device",
    "device_id": "sensor_01"
}
```

Nếu thành công:

```text
Device sensor_01 deleted
```

Thứ tự thao tác đảm bảo không để lại orphan bond (refactor plan §8):

```text
device_store_get(device_id)      // snapshot peer identity TRƯỚC khi xóa
        │
        ▼
ble_central_forget_peer(         // truyền addr rõ ràng, propagate lỗi bond
    existing.device_id,
    existing.ble_addr,
    existing.ble_addr_type,
    existing.has_ble_addr
)
        │
        ▼
device_store_delete(device_id)
```

Nếu `ble_central_forget_peer()` thất bại, store entry được giữ nguyên để retry; nếu bond đã xóa nhưng store delete thất bại, lỗi được log `[DEVICE_DELETE_FAILED]` và trả `DISPATCH_STATUS_INTERNAL_ERROR`.

---

## 6.3. `edit_device`

Sửa:

```text
name
device_type
```

Ví dụ:

```json
{
    "type": "gateway_command",
    "command": "edit_device",
    "device_id": "sensor_01",
    "name": "Bedroom Sensor"
}
```

Hoặc:

```json
{
    "type": "gateway_command",
    "command": "edit_device",
    "device_id": "sensor_01",
    "device_type": "temperature"
}
```

Ít nhất phải cung cấp `name` hoặc `device_type`.

---

## 6.4. `list_devices`

```json
{
    "type": "gateway_command",
    "command": "list_devices"
}
```

Kết quả là JSON array.

Ví dụ:

```json
[
    {
        "device_id": "sensor_01",
        "name": "Living Room",
        "type": "sensor",
        "connected": true,
        "has_ble_addr": true,
        "ble_addr": "AA:BB:CC:DD:EE:FF",
        "ble_addr_type": 0
    }
]
```

Các thông tin trả về gồm:

```text
device_id
name
type
connected
has_ble_addr
ble_addr
ble_addr_type
```

Kết quả phải nằm trong buffer `4096` byte. Nếu danh sách quá lớn dispatcher trả:

```text
Device list is too large
```

---

## 6.5. `get_status`

```json
{
    "type": "gateway_command",
    "command": "get_status"
}
```

Ví dụ response:

```json
{
    "status": "ok",
    "device_count": 3,
    "connected_count": 2,
    "ble_link_count": 2
}
```

Trong đó:

```text
device_count
    Tổng số device lưu trong Device Store.

connected_count
    Số device đang ở trạng thái connected.

ble_link_count
    Số BLE link thực tế đang active.
```

---

# 7. Đăng ký Gateway Command mới

Một điểm quan trọng của kiến trúc này là có thể mở rộng command mà không cần sửa dispatcher.

Handler phải có prototype:

```c
void handler(
    const gw_message_t *msg,
    dispatch_result_t *result
);
```

Ví dụ muốn thêm:

```text
restart_gateway
```

Có thể viết:

```c
static void cmd_restart_gateway(
    const gw_message_t *msg,
    dispatch_result_t *result)
{
    (void)msg;

    command_dispatcher_set_result(
        result,
        true,
        "Gateway restarting"
    );

    // esp_restart();
}
```

Sau đó đăng ký:

```c
command_dispatcher_register(
    "restart_gateway",
    cmd_restart_gateway
);
```

Từ thời điểm đó:

```c
command_dispatcher_is_registered("restart_gateway")
```

sẽ trả về true.

Registry dùng FreeRTOS mutex để bảo vệ truy cập đồng thời, từ chối tên trùng và giới hạn tối đa 16 command.

---

# 8. Device Command

`device_command` khác hoàn toàn `gateway_command`.

Ví dụ:

```json
{
    "type": "device_command",
    "device_id": "light_01",
    "command": "set_power",
    "bool_value": true
}
```

Command này không chạy trên gateway.

Nó được gửi tới device BLE:

```text
Client / MCP
      │
      ▼
ESP32 Gateway
      │
      │ device_command
      ▼
command_dispatcher_handle()
      │
      ▼
device_command_handle()
      │
      ▼
ble_central_send_command()
      │
      ▼
BLE Device
```

---

# 9. Luồng xử lý Device Command

`device_command_handle()` thực hiện tuần tự:

```text
1. Kiểm tra device_id
        │
        ▼
2. Kiểm tra BLE device connected
        │
        ▼
3. Allocate pending ACK
        │
        ▼
4. ble_central_send_command()
        │
        ▼
5. Chờ semaphore
        │
        │ maximum 2000 ms
        ▼
6. Nhận BLE notification
        │
        ▼
7. command_dispatcher_on_device_notify()
        │
        ▼
8. Match ACK
        │
        ▼
9. Wake task đang chờ
        │
        ▼
10. trả dispatch_result_t
```

---

# 10. Kiểm tra device connection

Trước khi gửi command:

```c
ble_central_is_connected(msg->device_id)
```

được gọi.

Nếu device chưa connected:

```text
Device <device_id> is not connected
```

Command sẽ không được gửi.

---

# 11. Cơ chế Pending ACK

Pending request được quản lý bởi module nội bộ `device_request_manager.c` (tách khỏi `device_command.c` từ refactor Phase 1).

Manager giữ một bảng:

```c
pending_request_t s_requests[];
```

Mỗi entry gồm:

```c
typedef struct {
    bool in_use;
    bool completed;

    uint32_t request_id;

    char device_id[GW_MSG_DEVICE_ID_LEN];

    char command[GW_MSG_COMMAND_LEN];

    gw_message_t response;

    SemaphoreHandle_t semaphore;
} pending_request_t;
```

Nó cho phép một task:

```text
send BLE command (request_id do manager sinh)
```

sau đó block chờ BLE notification tương ứng.

---

# 12. Giới hạn: một command đang chờ trên mỗi device

Đây là đặc điểm rất quan trọng của implementation hiện tại.

Khi allocate ACK, dispatcher kiểm tra:

```text
đã có pending command cho device_id này chưa?
```

Nếu có:

```text
Another command for <device_id> is pending
```

Do đó:

```text
Device A → command 1 ────── waiting ACK

Device A → command 2
               │
               └── REJECTED
```

Nhưng:

```text
Device A → command
Device B → command
Device C → command
```

có thể tồn tại đồng thời.

Số pending ACK tối đa bằng:

```c
DEVICE_STORE_MAX_DEVICES
```

---

# 13. Timeout ACK

Sau khi gửi BLE:

```c
ble_central_send_command(...)
```

dispatcher gọi:

```c
xSemaphoreTake(
    pending->semaphore,
    pdMS_TO_TICKS(DISPATCHER_ACK_TIMEOUT_MS)
);
```

với:

```text
DISPATCHER_ACK_TIMEOUT_MS = 2000
```

Nếu sau 2 giây không có response:

```text
No ACK from <device> within 2000 ms
```

---

# 14. BLE ACK quay lại Dispatcher

Trong `main.c`, callback BLE được kết nối như sau:

```c
static void on_device_notify(
    const char *device_id,
    const gw_message_t *msg)
{
    command_dispatcher_on_device_notify(
        device_id,
        msg
    );
}
```

và callback được truyền cho:

```c
ble_central_init(on_device_notify);
```

Do đó luồng response:

```text
BLE peripheral
      │
      │ notification
      ▼
BLE Central
      │
      ▼
on_device_notify()
      │
      ▼
command_dispatcher_on_device_notify()
```

---

# 15. Cách Dispatcher match ACK

ACK correlation được sở hữu bởi `device_request_manager` (module nội bộ). Một notification chỉ complete pending request khi **toàn bộ** điều kiện sau khớp:

```text
msg.type == "device_ack"
AND msg.device_id == pending.device_id
AND msg.request_id == pending.request_id   (primary correlation key)
AND msg.command  == pending.command        (validation bổ sung)
```

Quy tắc quan trọng:

* `request_id` là primary correlation key; `command` chỉ dùng để phát hiện protocol error (`[ACK_PROTOCOL_ERROR]`).
* Notification `type == "device_event"` **không bao giờ** complete pending command.
* ACK thiếu `request_id`, có `request_id == 0`, hoặc không match request nào bị log `[ACK_UNMATCHED]` và bị bỏ qua.
* Stale ACK (của request đã timeout) không thể complete request mới vì `request_id` không bao giờ reuse khi còn pending.
* Dispatcher sinh `request_id` (monotonic, != 0) và gán vào bản sao wire message; message của caller không bị sửa.

Sau khi match:

```text
pending->response = *msg
```

và semaphore được release để đánh thức task đang đợi.

---

# 16. Giá trị ACK

Dispatcher hiện sử dụng:

```c
response.bool_value
```

để xác định ACK thành công.

Nếu:

```text
bool_value = true
```

kết quả:

```text
Device <device> acknowledged '<command>'
```

Nếu:

```text
bool_value = false
```

kết quả:

```text
Device <device> rejected '<command>'
```

Ví dụ:

```text
Device light_01 acknowledged 'set_power'
```

---

# 17. Dispatcher và MCP Endpoint

Trong project hiện tại, `command_dispatcher` được sử dụng trực tiếp bởi:

```text
components/mcp_endpoint
```

Gateway expose:

```text
POST /mcp
```

theo JSON-RPC 2.0.

`mcp_endpoint` chuyển JSON request thành:

```c
gw_message_t
```

sau đó gọi:

```c
command_dispatcher_handle(
    &message,
    &dispatch_result
);
```

---

# 18. Liệt kê command qua MCP

MCP hỗ trợ:

```text
list_tools
```

hoặc:

```text
tools/list
```

Ví dụ request:

```json
{
    "jsonrpc": "2.0",
    "id": 1,
    "method": "tools/list"
}
```

Gateway lấy danh sách trực tiếp từ:

```c
command_dispatcher_get_registered_names()
```

Vì vậy mặc định client sẽ thấy:

```text
add_device
delete_device
edit_device
list_devices
get_status
```

---

# 19. Gọi Gateway Command qua `/mcp`

Ví dụ lấy status:

```json
{
    "jsonrpc": "2.0",
    "id": 1,
    "method": "tools/call",
    "params": {
        "name": "get_status",
        "arguments": {}
    }
}
```

MCP tự nhận ra:

```text
get_status
```

đã có trong Command Registry nên tự đặt:

```text
type = gateway_command
```

Response dạng:

```json
{
    "jsonrpc": "2.0",
    "id": 1,
    "result": {
        "success": true,
        "message": "{\"status\":\"ok\",\"device_count\":2,\"connected_count\":1,\"ble_link_count\":1}",
        "data": {
            "status": "ok",
            "device_count": 2,
            "connected_count": 1,
            "ble_link_count": 1
        }
    }
}
```

Nếu `dispatch_result.message` chứa JSON hợp lệ, MCP tự parse nó và thêm field:

```json
"data": ...
```

---

# 20. Gọi Device Command qua `/mcp`

Giả sử BLE device hỗ trợ command:

```text
set_power
```

và device ID:

```text
light_01
```

Có thể gọi:

```json
{
    "jsonrpc": "2.0",
    "id": 10,
    "method": "tools/call",
    "params": {
        "name": "set_power",
        "arguments": {
            "device_id": "light_01",
            "bool_value": true
        }
    }
}
```

Ở đây `set_power` không tồn tại trong Gateway Command Registry nhưng có `device_id`.

MCP tự suy luận:

```text
type = device_command
```

Luồng trở thành:

```text
HTTP POST /mcp

        │
        ▼

MCP Endpoint

        │
        │ JSON → gw_message_t
        ▼

Command Dispatcher

        │
        │ type=device_command
        ▼

Device Command

        │
        │ CBOR / BLE
        ▼

light_01

        │
        │ ACK
        ▼

BLE notification

        │
        ▼

Command Dispatcher

        │
        ▼

MCP response
```

Logic suy luận `gateway_command` hay `device_command` nằm trong `params_to_message()`.

---

# 21. Có thể ép loại command

Có thể truyền trực tiếp:

```json
"type": "device_command"
```

Ví dụ:

```json
{
    "jsonrpc": "2.0",
    "id": 11,
    "method": "tools/call",
    "params": {
        "name": "set_brightness",
        "arguments": {
            "type": "device_command",
            "device_id": "light_01",
            "int_value": 75
        }
    }
}
```

Hoặc:

```json
"type": "gateway_command"
```

Chỉ hai giá trị này được chấp nhận.

---

# 22. Quy tắc tự xác định command type của MCP

Nếu không truyền `type`, logic hiện tại gần tương đương:

```text
IF command có trong Gateway Registry
    → gateway_command

ELSE IF có device_id
    → device_command

ELSE
    → gateway_command
```

Ví dụ:

```text
get_status
```

được register:

```text
gateway_command
```

Trong khi:

```text
set_power + device_id=light_01
```

không nằm trong registry:

```text
device_command
```

Đây là cơ chế rất tiện vì các command đặc thù của BLE device không cần đăng ký vào dispatcher.

---

# 23. Khởi tạo trong ứng dụng

Trong `app_main()` trình tự hiện tại là:

```text
NVS
 │
 ▼
Wi-Fi
 │
 ▼
Device Store
 │
 ▼
Command Dispatcher
 │
 ▼
BLE Central
 │
 ▼
BLE Reconnect Supervisor
 │
 ▼
Web Server
 │
 ▼
MCP Endpoint
```

Cụ thể:

```c
device_store_init();

command_dispatcher_init();
command_dispatcher_freeze_registry();

ble_central_init(on_device_notify);

ble_central_start_reconnect_supervisor();

web_server_start();

mcp_endpoint_register(server);
```

Thứ tự này quan trọng vì dispatcher sử dụng `device_store`, trong khi BLE Central cần callback về dispatcher. `freeze_registry()` phải được gọi sau khi mọi custom command đã được register; trước freeze, `command_dispatcher_handle()` trả `DISPATCH_STATUS_INTERNAL_ERROR`.

---

# 24. `dispatch_result_t`

Mọi command trả về:

```c
typedef enum {
    DISPATCH_RESULT_TEXT = 0,
    DISPATCH_RESULT_JSON,
} dispatch_result_format_t;

typedef enum {
    DISPATCH_STATUS_OK = 0,
    DISPATCH_STATUS_INVALID_ARGUMENT,
    DISPATCH_STATUS_NOT_FOUND,
    DISPATCH_STATUS_BUSY,
    DISPATCH_STATUS_TIMEOUT,
    DISPATCH_STATUS_NOT_CONNECTED,
    DISPATCH_STATUS_TRANSPORT_ERROR,
    DISPATCH_STATUS_INTERNAL_ERROR,
    DISPATCH_STATUS_DEVICE_ERROR,
} dispatch_status_t;

typedef struct {
    dispatch_status_t status;
    dispatch_result_format_t format;
    char payload[4096];
} dispatch_result_t;
```

`status` là **single source of truth** — không có field `success` riêng để tránh trạng thái mâu thuẫn. Nếu cần boolean:

```c
if (dispatch_result_is_ok(&result)) { ... }
```

Transport layer map status deterministic sang HTTP:

```text
DISPATCH_STATUS_OK                -> 200
DISPATCH_STATUS_INVALID_ARGUMENT  -> 400
DISPATCH_STATUS_NOT_FOUND         -> 404
DISPATCH_STATUS_BUSY              -> 409
DISPATCH_STATUS_NOT_CONNECTED     -> 502
DISPATCH_STATUS_TRANSPORT_ERROR   -> 502
DISPATCH_STATUS_DEVICE_ERROR      -> 502
DISPATCH_STATUS_TIMEOUT           -> 504
DISPATCH_STATUS_INTERNAL_ERROR    -> 500
```

Handler dùng helper để set result:

```c
command_dispatcher_set_text_result(result, DISPATCH_STATUS_OK,
                                   "Device %s deleted", device_id);

command_dispatcher_set_json_result(result, DISPATCH_STATUS_OK,
                                   "{\"persisted\":true}");
```

Ví dụ sử dụng:

```c
dispatch_result_t result;

command_dispatcher_handle(
    &message,
    &result
);

if (dispatch_result_is_ok(&result)) {
    printf("OK: %s\n", result.payload);
} else {
    printf("ERROR (%d): %s\n", result.status, result.payload);
}
```

---

# 25. Sử dụng Dispatcher trực tiếp trong C

Không bắt buộc phải đi qua MCP.

Ví dụ:

```c
gw_message_t msg = {0};

strlcpy(
    msg.type,
    "gateway_command",
    sizeof(msg.type)
);

strlcpy(
    msg.command,
    "get_status",
    sizeof(msg.command)
);

dispatch_result_t result;

command_dispatcher_handle(
    &msg,
    &result
);

ESP_LOGI(
    "APP",
    "ok=%d result=%s",
    dispatch_result_is_ok(&result),
    result.payload
);
```

Luồng:

```text
application
     │
     ▼
gw_message_t
     │
     ▼
command_dispatcher_handle()
     │
     ▼
handler
     │
     ▼
dispatch_result_t
```

---

# 26. Xử lý lỗi

Một số lỗi dispatcher hiện có thể trả (kèm `dispatch_status_t` tương ứng):

```text
Null message                              INVALID_ARGUMENT
Invalid message (validation boundary)     INVALID_ARGUMENT
Unknown message type: ...                 NOT_FOUND
Unknown gateway command: ...              NOT_FOUND
Missing device_id                         INVALID_ARGUMENT / NOT_FOUND
Device ... is not connected               NOT_CONNECTED
Another command for ... is pending        BUSY
Could not send command to ...             TRANSPORT_ERROR
No ACK from ... within 2000 ms            TIMEOUT
Device ... rejected '...'                 DEVICE_ERROR
```

Điều này giúp phía MCP/Web/API không cần hiểu trực tiếp trạng thái của BLE Central.

---

# 27. Thread safety

Hai vùng chính được bảo vệ bằng FreeRTOS mutex.

Command Registry:

```text
s_registry_mutex
```

Command Registry:

```text
s_registry_mutex
```

Pending Request Manager (`device_request_manager.c`):

```text
s_request_mutex
```

Do đó registry lookup/register và pending request table được bảo vệ khi nhiều FreeRTOS task truy cập đồng thời. Mutex không bao giờ được giữ trong thời gian chờ BLE I/O hoặc chờ ACK: waiter chỉ take semaphore của slot, còn `device_request_complete()` chỉ giữ mutex trong lúc copy response.

---

# 28. Một số điểm cần chú ý trong thiết kế hiện tại

## 28.1. ACK correlation dùng request ID

Từ refactor Phase 1, mỗi `device_command` được dispatcher gán một `request_id` (uint32, monotonic, != 0) và peripheral phải echo chính xác ID đó trong `device_ack`. Nhờ đó:

* telemetry/event không thể bị hiểu nhầm thành ACK;
* stale ACK không thể complete request mới;
* hai device correlate độc lập với nhau.

Vẫn còn giới hạn Phase 1: **một pending command/device**. Nhiều command concurrent trên cùng peripheral cần mở rộng request manager (multi-slot per device).

## 28.2. Device command là synchronous

`device_command_handle()`:

```text
send
 ↓
wait semaphore
 ↓
ACK hoặc timeout
 ↓
return
```

nên thread gọi dispatcher có thể bị block tối đa khoảng:

```text
2 giây
```

cho mỗi device command.

Đây không phải asynchronous API.

---

## 28.4. Registry chỉ dành cho Gateway Commands

Không nên register tất cả command của BLE device.

Ví dụ:

```text
set_power
set_brightness
set_temperature
open
close
calibrate
```

không bắt buộc xuất hiện trong command registry.

Registry chủ yếu dành cho command thuộc gateway:

```text
add_device
delete_device
edit_device
list_devices
get_status
...
```

Device command được route dựa trên:

```text
type=device_command
+
device_id
```

---

# 29. Mô hình kiến trúc nên hiểu

Có thể hình dung toàn bộ hệ thống thành 4 tầng:

```text
┌───────────────────────────────────────┐
│               Client                  │
│      MCP / Web / Application          │
└──────────────────┬────────────────────┘
                   │
                   ▼
┌───────────────────────────────────────┐
│             MCP Endpoint              │
│                                       │
│ JSON-RPC → gw_message_t               │
└──────────────────┬────────────────────┘
                   │
                   ▼
┌───────────────────────────────────────┐
│          Command Dispatcher           │
│                                       │
│ gateway_command    device_command     │
│      │                   │            │
└──────┼───────────────────┼────────────┘
       │                   │
       ▼                   ▼
┌──────────────┐     ┌─────────────────┐
│Device Store  │     │   BLE Central   │
│Gateway Logic │     │                 │
└──────────────┘     └────────┬────────┘
                              │
                              ▼
                       ┌──────────────┐
                       │ BLE Devices  │
                       └──────────────┘
```

---

# 30. Tóm tắt cách dùng

### Khởi động

```c
command_dispatcher_init();
command_dispatcher_freeze_registry();
```

### Thực thi command

```c
command_dispatcher_handle(
    &message,
    &result
);
```

### Gateway command

```text
type = gateway_command
command = get_status
```

### Device command

```text
type = device_command
device_id = light_01
command = set_power
bool_value = true
```

### Nhận BLE ACK

BLE callback phải gọi:

```c
command_dispatcher_on_device_notify(
    device_id,
    &message
);
```

### Thêm gateway command mới

```c
command_dispatcher_register(
    "my_command",
    my_handler
);
```

---

# 31. Kết luận

`command_dispatcher` đang đóng vai trò **routing layer** giữa giao diện điều khiển bên ngoài và hai domain khác nhau:

```text
Gateway operations
        +
BLE Device operations
```

Điểm mạnh của thiết kế hiện tại là:

```text
MCP
 │
 ▼
gw_message_t
 │
 ▼
Command Dispatcher
 │
 ├── Gateway Registry
 │
 └── BLE Device Command + ACK
```

Nhờ vậy MCP/Web/API không cần biết chi tiết BLE và BLE Central cũng không cần biết JSON-RPC.

Về mặt trách nhiệm component:

```text
command_dispatcher
    = routing
    + gateway command registry
    + BLE command request/ACK synchronization
```

Đây là ranh giới kiến trúc hợp lý cho gateway hiện tại.
