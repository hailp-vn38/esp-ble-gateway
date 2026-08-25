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
├── gateway_commands.c
├── CMakeLists.txt
│
├── include/
│   └── command_dispatcher.h
│
└── test/
```

Vai trò:

| File                            | Chức năng                                    |
| ------------------------------- | -------------------------------------------- |
| `command_dispatcher.c`          | Entry point, phân loại message               |
| `command_registry.c`            | Registry các gateway command                 |
| `gateway_commands.c`            | Implementation các command chạy trên Gateway |
| `device_command.c`              | Gửi command qua BLE và chờ ACK               |
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

int command_dispatcher_get_registered_names(
    const char **out_names,
    int max_names
);

int command_dispatcher_is_registered(
    const char *command_name
);

void command_dispatcher_handle(
    const gw_message_t *msg,
    dispatch_result_t *result
);

void command_dispatcher_on_device_notify(
    const char *device_id,
    const gw_message_t *msg
);
```

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
name           : 32 chars
device_type    : 16 chars
protocol       : version 1
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

Thành công:

```text
Device sensor_01 added
```

---

## 6.2. `delete_device`

Xóa device khỏi `device_store`.

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

Ngoài xóa khỏi store, dispatcher còn gọi:

```c
ble_central_forget_device(msg->device_id);
```

do đó BLE Central cũng ngừng quản lý device này.

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

Dispatcher giữ một bảng:

```c
pending_ack_t s_pending_acks[];
```

Mỗi entry gồm:

```c
typedef struct {
    bool in_use;

    char device_id[GW_MSG_DEVICE_ID_LEN];

    char command[GW_MSG_COMMAND_LEN];

    gw_message_t response;

    SemaphoreHandle_t semaphore;
} pending_ack_t;
```

Nó cho phép một task:

```text
send BLE command
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

Khi BLE notification tới, dispatcher tìm pending ACK thỏa:

```text
pending.device_id == device_id
```

và:

```text
msg.command == pending.command
```

Hoặc nếu ACK không chứa command:

```text
msg.command == ""
```

thì cũng được chấp nhận.

Logic tương đương:

```c
if (
    pending->in_use &&
    strcmp(pending->device_id, device_id) == 0 &&
    (
        msg->command[0] == '\0' ||
        strcmp(pending->command, msg->command) == 0
    )
)
```

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

ble_central_init(on_device_notify);

ble_central_start_reconnect_supervisor();

web_server_start();

mcp_endpoint_register(server);
```

Thứ tự này quan trọng vì dispatcher sử dụng `device_store`, trong khi BLE Central cần callback về dispatcher.

---

# 24. `dispatch_result_t`

Mọi command trả về:

```c
typedef struct {
    int success;
    char message[4096];
} dispatch_result_t;
```

Ví dụ:

```c
dispatch_result_t result;

command_dispatcher_handle(
    &message,
    &result
);

if (result.success) {
    printf("OK: %s\n", result.message);
} else {
    printf("ERROR: %s\n", result.message);
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
    "success=%d result=%s",
    result.success,
    result.message
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

Một số lỗi dispatcher hiện có thể trả:

```text
Null message

Unknown message type: ...

Unknown gateway command: ...

Missing device_id

Device ... is not connected

Another command for ... is pending

Could not send command to ...

No ACK from ... within 2000 ms

Could not read ACK from ...

Device ... rejected '...'
```

Điều này giúp phía MCP/Web/API không cần hiểu trực tiếp trạng thái của BLE Central.

---

# 27. Thread safety

Hai vùng chính được bảo vệ bằng FreeRTOS mutex.

Command Registry:

```text
s_registry_mutex
```

Device ACK:

```text
s_ack_mutex
```

Do đó registry lookup/register và pending ACK table được bảo vệ khi nhiều FreeRTOS task truy cập đồng thời.

---

# 28. Một số điểm cần chú ý trong thiết kế hiện tại

## 28.1. Không có request ID cho Device Command

ACK hiện được correlate chủ yếu bằng:

```text
device_id
+
command
```

chứ chưa có:

```text
transaction_id
request_id
sequence_number
```

Do hệ thống chỉ cho phép **một pending command/device**, cách này hiện vẫn hoạt động.

Nhưng nếu tương lai muốn:

```text
Device A
 ├── command 1
 ├── command 2
 └── command 3
```

chạy song song thì cần bổ sung transaction ID.

---

## 28.2. ACK không có command vẫn được match

Logic hiện tại cho phép:

```text
msg.command == ""
```

match với pending request của device.

Điều này thuận tiện cho firmware peripheral đơn giản.

Tuy nhiên nó cũng đồng nghĩa một notification không có `command` có khả năng bị hiểu thành ACK nếu device đang có pending command.

Đây là điểm cần đặc biệt lưu ý khi BLE device đồng thời phát telemetry/notification tự do.

---

## 28.3. Device command là synchronous

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
