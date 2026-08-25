# `components/ble_central`

## 1. Tổng quan

`ble_central` là component chịu trách nhiệm toàn bộ phía **BLE Central / GATT Client** của ESP32 BLE Gateway.

Nhiệm vụ chính của component:

* Khởi tạo NimBLE host.
* Scan các BLE peripheral tương thích với gateway.
* Kết nối tới nhiều thiết bị BLE.
* Thiết lập security/bonding.
* Discover GATT service và characteristic.
* Gửi command từ gateway tới thiết bị.
* Nhận status/notification từ thiết bị.
* Quản lý trạng thái từng kết nối.
* Tự động reconnect các thiết bị đã đăng ký.
* Theo dõi timeout trong quá trình security/GATT discovery.

Public API của component được khai báo trong:

```text
components/ble_central/include/ble_central.h
```

Component gồm 6 implementation file riêng biệt: `ble_central.c`, `ble_central_gap.c`, `ble_central_gatt.c`, `ble_central_scan.c`, `ble_central_state.c` và `ble_central_supervisor.c`.

---

# 2. Kiến trúc

Có thể hình dung kiến trúc như sau:

```text
                  Application
                      │
                      │
                ble_central.h
                      │
        ┌─────────────┼──────────────┐
        │             │              │
        ▼             ▼              ▼
   ble_central     BLE Scan       Supervisor
        │
        ├──────────────┐
        │              │
        ▼              ▼
      GAP            GATT
        │              │
        └──────┬───────┘
               │
               ▼
         Connection State
               │
        ┌──────┴──────┐
        ▼             ▼
   device_store    cbor_codec
```

Trong project hiện tại, `app_main()` khởi tạo component theo thứ tự:

```text
device_store_init()
        ↓
command_dispatcher_init()
        ↓
ble_central_init()
        ↓
ble_central_start_reconnect_supervisor()
```

Notification từ BLE được chuyển về:

```c
static void on_device_notify(
    const char *device_id,
    const gw_message_t *msg)
{
    command_dispatcher_on_device_notify(device_id, msg);
}
```

Sau đó callback này được truyền vào:

```c
ble_central_init(on_device_notify);
```

Do đó `ble_central` đóng vai trò cầu nối giữa thiết bị BLE và tầng xử lý command của gateway.

---

# 3. Các file trong component

## `ble_central.c`

Public implementation chính.

Chịu trách nhiệm:

* init NimBLE;
* connect;
* disconnect;
* forget device;
* gửi command;
* kiểm tra connection;
* đếm số connection đang hoạt động.

---

## `ble_central_gap.c`

Xử lý các **GAP event** của NimBLE.

Ví dụ:

```text
CONNECT
DISCONNECT
NOTIFY_RX
MTU
ENC_CHANGE
REPEAT_PAIRING
```

---

## `ble_central_gatt.c`

Quản lý quá trình GATT discovery:

```text
Service
   ↓
Characteristic
   ↓
Descriptor
   ↓
Subscribe STATUS
```

Khi tất cả bước thành công, connection chuyển sang:

```text
BLE_CONN_SLOT_READY
```

---

## `ble_central_scan.c`

Thực hiện BLE scanning.

Component chỉ trả về advertisement chứa Gateway Service UUID:

```text
0xABF0
```

---

## `ble_central_state.c`

Quản lý:

* connection slots;
* mutex;
* global BLE state;
* lookup connection;
* allocate slot;
* tính connection interval.

---

## `ble_central_supervisor.c`

FreeRTOS task chuyên:

* tự động reconnect;
* kiểm tra discovery timeout;
* thử kết nối lại thiết bị offline.

---

# 4. BLE protocol

Component định nghĩa một custom BLE service:

```c
#define BLE_GATEWAY_SERVICE_UUID  0xABF0
#define BLE_GATEWAY_COMMAND_UUID  0xABF1
#define BLE_GATEWAY_STATUS_UUID   0xABF2
```

Cấu trúc logic:

```text
Service 0xABF0
│
├── COMMAND 0xABF1
│      Gateway → Peripheral
│
└── STATUS 0xABF2
       Peripheral → Gateway
       Notification
```

---

# 5. COMMAND characteristic

UUID:

```text
0xABF1
```

Gateway gửi `gw_message_t` qua characteristic này.

Luồng:

```text
gw_message_t
     ↓
cbor_codec_encode()
     ↓
CBOR binary
     ↓
ble_gattc_write_no_rsp_flat()
     ↓
Peripheral
```

Implementation:

```c
ble_central_send_command()
```

Chỉ gửi được khi slot đang ở:

```text
BLE_CONN_SLOT_READY
```

và đã discover được:

```text
command_val_handle
```

Component dùng **Write Without Response**, nên việc hàm trả về thành công chỉ xác nhận NimBLE đã chấp nhận thao tác ghi, không xác nhận peripheral đã thực thi command.

---

# 6. STATUS characteristic

UUID:

```text
0xABF2
```

Peripheral gửi trạng thái về gateway thông qua BLE Notification.

Luồng:

```text
Peripheral
    ↓
STATUS notify
    ↓
BLE_GAP_EVENT_NOTIFY_RX
    ↓
CBOR bytes
    ↓
cbor_codec_decode()
    ↓
gw_message_t
    ↓
ble_central_notify_cb_t
    ↓
command_dispatcher
```

Callback API:

```c
typedef void (*ble_central_notify_cb_t)(
    const char *device_id,
    const gw_message_t *msg);
```

Trong GAP handler, notification chỉ được xử lý nếu:

```c
event->notify_rx.attr_handle == slot->status_val_handle
```

Payload sau đó được copy từ `mbuf`, kiểm tra kích thước, decode CBOR và chuyển tới callback của application.

---

# 7. Connection slot

Mỗi thiết bị được quản lý bởi:

```c
typedef struct {
    ble_conn_slot_state_t state;

    bool forget_requested;

    char device_id[GW_MSG_DEVICE_ID_LEN];

    ble_addr_t peer_addr;

    uint16_t conn_handle;

    uint16_t service_start_handle;
    uint16_t service_end_handle;

    uint16_t command_val_handle;
    uint16_t status_val_handle;
    uint16_t status_cccd_handle;

    int64_t last_attempt_ms;
    int64_t discovery_started_ms;
} ble_conn_slot_t;
```

Các slot nằm trong:

```c
g_ble_connections[BLE_CENTRAL_MAX_CONN]
```

Trong đó:

```c
#define BLE_CENTRAL_MAX_CONN \
    CONFIG_BT_NIMBLE_MAX_CONNECTIONS
```

Do đó số thiết bị tối đa phụ thuộc cấu hình NimBLE của ESP-IDF.

---

# 8. Connection state machine

Một device trải qua state machine:

```text
FREE
 │
 │ allocate
 ▼
IDLE
 │
 │ ble_central_connect()
 ▼
CONNECTING
 │
 │ BLE_GAP_EVENT_CONNECT
 ▼
SECURING
 │
 │ encryption/security OK
 ▼
DISCOVERING
 │
 │ GATT discovery + CCCD
 ▼
READY
```

State được định nghĩa:

```c
typedef enum {
    BLE_CONN_SLOT_FREE = 0,
    BLE_CONN_SLOT_IDLE,
    BLE_CONN_SLOT_CONNECTING,
    BLE_CONN_SLOT_SECURING,
    BLE_CONN_SLOT_DISCOVERING,
    BLE_CONN_SLOT_READY,
} ble_conn_slot_state_t;
```

---

# 9. `FREE`

Slot chưa được sử dụng.

Khi cần connect một device mới:

```c
ble_central_allocate_slot_unlocked()
```

tìm slot:

```text
state == BLE_CONN_SLOT_FREE
```

---

# 10. `IDLE`

Device đã có slot nhưng hiện chưa kết nối.

Đây là state cho phép gọi:

```c
ble_central_connect()
```

Supervisor cũng chỉ reconnect slot khi:

```text
state == IDLE
```

---

# 11. `CONNECTING`

Sau:

```c
ble_gap_connect(...)
```

state chuyển thành:

```text
CONNECTING
```

Khi connect thành công:

```text
CONNECTING
     ↓
SECURING
```

---

# 12. `SECURING`

Ngay sau khi link BLE được thiết lập, component chạy:

```c
ble_gattc_exchange_mtu()
```

và:

```c
ble_gap_security_initiate()
```

Component bật:

```c
ble_hs_cfg.sm_bonding = 1;
ble_hs_cfg.sm_sc = 1;
ble_hs_cfg.sm_mitm = 0;
ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
```

Tức là:

* bonding bật;
* LE Secure Connections bật;
* MITM không bắt buộc;
* thiết bị hoạt động theo mô hình không có input/output UI.

Khi encryption thành công sẽ xuất hiện:

```text
BLE_GAP_EVENT_ENC_CHANGE
```

và component bắt đầu GATT discovery.

---

# 13. `DISCOVERING`

GATT discovery chạy theo chuỗi:

```text
Discover Service 0xABF0
        ↓
Discover characteristics
        ↓
find 0xABF1 COMMAND
find 0xABF2 STATUS
        ↓
Discover descriptor
        ↓
find STATUS CCCD
        ↓
write 0x0001 to CCCD
        ↓
READY
```

---

# 14. Subscribe STATUS

Sau khi tìm được STATUS characteristic, component tìm:

```text
Client Characteristic Configuration Descriptor
```

hay CCCD.

Sau đó ghi:

```c
const uint8_t notify_enable[] = {
    0x01,
    0x00
};
```

tương đương:

```text
0x0001
```

để bật Notification.

Khi việc ghi CCCD thành công:

```c
slot->state = BLE_CONN_SLOT_READY;
```

và:

```c
device_store_set_connected(
    slot->device_id,
    1);
```

---

# 15. `READY`

`READY` có nghĩa là:

* BLE link tồn tại;
* security đã hoàn thành;
* Gateway Service đã được tìm thấy;
* COMMAND characteristic đã được tìm thấy;
* STATUS characteristic đã được tìm thấy;
* notification đã được subscribe.

Chỉ ở state này:

```c
ble_central_is_connected()
```

mới trả về true.

Điểm này cần phân biệt:

```text
BLE physical connection
```

không đồng nghĩa với:

```text
ble_central_is_connected() == true
```

API của project coi device là connected khi toàn bộ GATT session đã `READY`.

---

# 16. Khởi tạo component

API:

```c
int ble_central_init(
    ble_central_notify_cb_t notify_cb);
```

Ví dụ trong project:

```c
static void on_device_notify(
    const char *device_id,
    const gw_message_t *msg)
{
    command_dispatcher_on_device_notify(
        device_id,
        msg);
}

void app_main(void)
{
    ...

    device_store_init();

    command_dispatcher_init();

    ble_central_init(
        on_device_notify);

    ble_central_start_reconnect_supervisor();

    ...
}
```

`ble_central_init()` thực hiện:

```text
state_init
    ↓
scan_reset
    ↓
nimble_port_init
    ↓
register reset_cb
    ↓
register sync_cb
    ↓
configure security
    ↓
configure bond store
    ↓
start NimBLE FreeRTOS task
```

---

# 17. NimBLE host task

NimBLE chạy trong FreeRTOS task:

```c
static void nimble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}
```

Khởi động bằng:

```c
nimble_port_freertos_init(
    nimble_host_task);
```

---

# 18. Host synchronization

Sau khi NimBLE stack sẵn sàng:

```c
on_ble_host_sync()
```

được gọi.

Component xác định local address type bằng:

```c
ble_hs_id_infer_auto(
    0,
    &g_ble_own_addr_type);
```

sau đó:

```c
g_ble_host_synced = true;
```

Trước thời điểm này `ble_central_connect()` chưa cho phép tạo connection.

---

# 19. Connect một device

API:

```c
int ble_central_connect(
    const char *device_id,
    const uint8_t *ble_addr,
    uint8_t addr_type);
```

Điều kiện:

* `device_id` hợp lệ;
* `ble_addr` hợp lệ;
* NimBLE host synced;
* device tồn tại trong `device_store`;
* có connection slot;
* slot đang `IDLE`.

Component kiểm tra device trước bằng:

```c
device_store_get(
    device_id,
    &registered_device);
```

Nếu device không tồn tại:

```text
return -3
```

---

# 20. Connection parameters

Connection interval được chọn tùy theo số connection đang hoạt động.

```text
active < 3
    → 15 ms

active < 6
    → 30 ms

active >= 6
    → 50 ms
```

Cụ thể:

```c
BLE_CONN_ITVL_FAST_UNITS    12
BLE_CONN_ITVL_MEDIUM_UNITS  24
BLE_CONN_ITVL_BUSY_UNITS    40
```

Một BLE interval unit bằng `1.25 ms`, tương ứng:

```text
12 × 1.25 = 15 ms
24 × 1.25 = 30 ms
40 × 1.25 = 50 ms
```

Logic lựa chọn nằm trong:

```c
ble_central_calculate_conn_interval()
```

Điều này giúp tránh sử dụng interval rất nhỏ khi số lượng peripheral tăng lên.

---

# 21. Connection timeout

Connect timeout:

```c
#define BLE_CONNECT_TIMEOUT_MS 10000
```

tức:

```text
10 giây
```

---

# 22. Disconnect

API:

```c
int ble_central_disconnect(
    const char *device_id);
```

Component lấy:

```text
conn_handle
```

sau đó gọi:

```c
ble_gap_terminate(
    handle,
    BLE_ERR_REM_USER_CONN_TERM);
```

Khi nhận:

```text
BLE_GAP_EVENT_DISCONNECT
```

slot được reset runtime handle và trở lại:

```text
IDLE
```

Do đó nếu reconnect supervisor đang chạy, thiết bị có thể được kết nối lại sau đó.

---

# 23. Forget peer

API:

```c
int ble_central_forget_peer(
    const char *device_id,
    const uint8_t ble_addr[6],
    uint8_t ble_addr_type,
    bool has_ble_addr);
```

Khác với `disconnect()`.

`forget_peer()` thực hiện:

```text
disconnect/cancel connection
        +
remove runtime slot
        +
delete BLE bond
```

Caller (command dispatcher) phải truyền peer identity lấy từ snapshot
`device_store` TRƯỚC khi entry bị xóa — BLE layer không quay lại lookup
store sau khi entry đã bị remove. Nếu runtime connection slot tồn tại,
addr của slot được ưu tiên.

Nếu thiết bị đang `IDLE`, slot được xoá ngay.

Nếu đang hoạt động:

```c
slot->forget_requested = true;
```

Sau khi disconnect, slot sẽ được giải phóng.

Component cũng gọi:

```c
ble_store_util_delete_peer(
    &peer_address);
```

để xóa bonding information khỏi NimBLE store. Return value propagate kết quả
thực tế: `0` = thành công (bao gồm case idempotent "bond không tồn tại" /
`BLE_HS_ENOENT`), `-1` = bond xóa thất bại hoặc BLE host chưa synced.

---

# 24. Scan BLE device

Public structure:

```c
typedef struct {
    uint8_t addr[6];
    uint8_t addr_type;
    int8_t rssi;
    char name[32];
} ble_scan_result_t;
```

Callback:

```c
typedef void (*ble_central_scan_result_cb_t)(
    const ble_scan_result_t *result);
```

---

# 25. Start scan

API:

```c
int ble_central_scan_start(
    ble_central_scan_result_cb_t scan_result_cb);
```

Ví dụ:

```c
static void on_scan(
    const ble_scan_result_t *result)
{
    printf(
        "Device: %s RSSI=%d\n",
        result->name,
        result->rssi);
}

ble_central_scan_start(on_scan);
```

Scan chạy liên tục:

```c
ble_gap_disc(
    g_ble_own_addr_type,
    BLE_HS_FOREVER,
    ...);
```

---

# 26. Scan filter

Không phải mọi BLE advertisement đều được trả về application.

Component parse advertisement:

```c
ble_hs_adv_parse_fields()
```

sau đó kiểm tra UUID list.

Chỉ device advertise:

```text
Service UUID 0xABF0
```

mới được callback.

```text
BLE advertisement
       ↓
parse advertisement
       ↓
contains 0xABF0 ?
   ┌───┴────┐
   │        │
  no       yes
   │        │
 ignore   callback
```

Điều này giúp scan API chỉ trả về các peripheral thuộc hệ thống Gateway.

---

# 27. Scan parameters

Component đang sử dụng:

```c
.itvl = 48;
.window = 48;
.filter_duplicates = 1;
```

Scan interval unit của BLE là `0.625 ms`:

```text
48 × 0.625 = 30 ms
```

Do:

```text
window == interval
```

nên scan gần như có duty cycle 100% trong thời gian scan.

Duplicate filtering được bật:

```text
filter_duplicates = 1
```

---

# 28. Stop scan

```c
ble_central_scan_stop();
```

Nội bộ gọi:

```c
ble_gap_disc_cancel();
```

Kiểm tra trạng thái bằng:

```c
ble_central_is_scanning();
```

---

# 29. Gửi command

API:

```c
int ble_central_send_command(
    const char *device_id,
    const gw_message_t *msg);
```

Ví dụ:

```c
gw_message_t message = {
    ...
};

int rc = ble_central_send_command(
    "device-01",
    &message);

if (rc != 0) {
    // device not ready
}
```

Luồng:

```text
find slot
    ↓
state == READY ?
    ↓
get command handle
    ↓
CBOR encode
    ↓
GATT write without response
```

---

# 30. Nhận notification

Application đăng ký callback:

```c
static void on_notify(
    const char *device_id,
    const gw_message_t *msg)
{
    ...
}
```

sau đó:

```c
ble_central_init(on_notify);
```

Khi peripheral notify:

```text
STATUS characteristic
        ↓
BLE_GAP_EVENT_NOTIFY_RX
        ↓
validate handle
        ↓
validate packet length
        ↓
copy mbuf
        ↓
CBOR decode
        ↓
notify_cb()
```

---

# 31. Kiểm tra device connected

```c
int ble_central_is_connected(
    const char *device_id);
```

Ví dụ:

```c
if (ble_central_is_connected("sensor-01")) {
    ...
}
```

Hàm trả true khi:

```text
slot exists
AND
slot.state == READY
```

---

# 32. Đếm active connection

```c
int ble_central_active_count(void);
```

Nội bộ đếm những slot có:

```c
conn_handle != BLE_HS_CONN_HANDLE_NONE
```

Do đó active count có thể bao gồm connection đang:

```text
SECURING
DISCOVERING
READY
```

chứ không chỉ `READY`.

---

# 33. Automatic reconnect supervisor

API:

```c
ble_central_start_reconnect_supervisor();
```

Tạo FreeRTOS task:

```text
ble_reconnect
```

stack:

```text
4096 bytes
```

priority:

```text
4
```

---

# 34. Supervisor loop

Task chạy mỗi:

```c
#define BLE_SUPERVISOR_TICK_MS 1000
```

tức:

```text
1 giây
```

Luồng:

```text
Supervisor
    │
    ▼
check discovery timeout
    │
    ▼
BLE host synced?
    │
    ▼
currently scanning?
    │
    ▼
device_store_snapshot()
    │
    ▼
find disconnected device
    │
    ▼
has BLE address?
    │
    ▼
retry interval passed?
    │
    ▼
ble_central_connect()
```

---

# 35. Reconnect interval

```c
#define BLE_RECONNECT_INTERVAL_MS 8000
```

tức:

```text
8 giây
```

Supervisor không thử kết nối lại cùng device liên tục.

---

# 36. Chỉ thực hiện một connection procedure mỗi vòng

Supervisor chứa:

```c
ble_central_connect(...);

/* NimBLE controllers commonly serialize
   connection procedures. */

break;
```

Điều đó có nghĩa một supervisor iteration chỉ khởi động kết nối tới tối đa một device.

Đây là chủ ý để phù hợp với cách controller NimBLE thường serialize quá trình connection establishment.

---

# 37. Scan và reconnect

Supervisor chỉ reconnect khi:

```c
!ble_gap_disc_active()
```

Do đó:

```text
BLE scanning
```

và:

```text
automatic reconnect
```

không chạy connection procedure đồng thời.

Nếu Web UI đang scan lâu, automatic reconnect sẽ tạm dừng cho đến khi scan kết thúc.

---

# 38. Discovery timeout

Security + GATT discovery có timeout:

```c
#define BLE_DISCOVERY_TIMEOUT_MS 10000
```

tức:

```text
10 giây
```

Supervisor kiểm tra slot ở:

```text
SECURING
```

hoặc:

```text
DISCOVERING
```

Nếu quá timeout:

```c
ble_gap_terminate(...)
```

để reset connection.

---

# 39. Host reset

Nếu NimBLE host reset:

```c
on_ble_host_reset()
```

component:

```text
g_ble_host_synced = false
        ↓
reset scan state
        ↓
mark every device disconnected
        ↓
reset GATT runtime handles
        ↓
slot → IDLE
```

Nếu slot có:

```text
forget_requested
```

thì slot được giải phóng hoàn toàn.

---

# 40. Repeat pairing

Nếu peripheral đã có bonding state không đồng bộ với gateway, NimBLE có thể phát:

```text
BLE_GAP_EVENT_REPEAT_PAIRING
```

Component xử lý bằng cách:

```c
ble_store_util_delete_peer(
    &repeat_description.peer_id_addr);
```

sau đó:

```c
return BLE_GAP_REPEAT_PAIRING_RETRY;
```

Có nghĩa gateway xoá bond cũ rồi thử pair lại.

---

# 41. Thread safety

Connection state được bảo vệ bởi:

```c
SemaphoreHandle_t s_connection_mutex;
```

Các thao tác dùng:

```c
ble_central_lock_connections();
...
ble_central_unlock_connections();
```

Mutex có timeout:

```c
pdMS_TO_TICKS(1000)
```

Điều này quan trọng vì connection state được truy cập từ nhiều context:

```text
Application tasks
Supervisor task
NimBLE callbacks
Scan callbacks
```

---

# 42. Quan hệ với `device_store`

`ble_central` không tự sở hữu database thiết bị.

`device_store` giữ các thông tin persistent/logical như:

```text
device_id
BLE address
BLE address type
connected state
```

Trong khi `ble_conn_slot_t` giữ runtime BLE state:

```text
conn_handle
GATT handles
connection state
timestamps
```

Có thể xem:

```text
device_store
    │
    ├── persistent/logical device state
    │
    ▼
ble_central
    │
    └── runtime BLE connection state
```

Khi connection trở thành `READY`:

```c
device_store_set_connected(
    slot->device_id,
    1);
```

Khi disconnect:

```c
device_store_set_connected(
    slot->device_id,
    0);
```

---

# 43. BLE address update

Sau khi connect, component đọc:

```c
description.peer_id_addr
```

và cập nhật:

```c
device_store_set_ble_addr(...)
```

Sau đó runtime slot cũng chuyển sang identity address này.

Điều này đặc biệt hữu ích với BLE privacy/random address vì địa chỉ sử dụng lúc scan không nhất thiết luôn là địa chỉ identity cuối cùng.

---

# 44. Public API

Header public hiện cung cấp:

```c
int ble_central_init(
    ble_central_notify_cb_t notify_cb);

int ble_central_connect(
    const char *device_id,
    const uint8_t *ble_addr,
    uint8_t addr_type);

int ble_central_disconnect(
    const char *device_id);

int ble_central_forget_peer(
    const char *device_id,
    const uint8_t ble_addr[6],
    uint8_t ble_addr_type,
    bool has_ble_addr);

int ble_central_send_command(
    const char *device_id,
    const gw_message_t *msg);

int ble_central_is_connected(
    const char *device_id);

int ble_central_active_count(void);

int ble_central_scan_start(
    ble_central_scan_result_cb_t scan_result_cb);

int ble_central_scan_stop(void);

int ble_central_is_scanning(void);

int ble_central_start_reconnect_supervisor(void);

void ble_central_stop_reconnect_supervisor(void);
```

---

# 45. Giá trị trả về quan trọng

Các API chủ yếu dùng convention:

```text
0  = success
<0 = error
```

Riêng `ble_central_connect()` có một số mã lỗi đáng chú ý:

```text
 0   thành công bắt đầu connection

-1   argument/state/slot/connect error

-2   NimBLE host chưa synchronized

-3   device_id chưa tồn tại trong device_store
```

---

# 46. Ví dụ workflow hoàn chỉnh

## Bước 1 — Init

```c
ble_central_init(on_device_notify);

ble_central_start_reconnect_supervisor();
```

## Bước 2 — Scan

```c
ble_central_scan_start(on_scan_result);
```

Peripheral phải advertise:

```text
UUID 0xABF0
```

## Bước 3 — Chọn device

Application nhận:

```text
BLE address
address type
RSSI
name
```

## Bước 4 — Register device

Device cần tồn tại trong `device_store` trước khi gọi `ble_central_connect()`.

## Bước 5 — Connect

```c
ble_central_connect(
    device_id,
    address,
    address_type);
```

## Bước 6 — Security

```text
BLE connected
      ↓
MTU exchange
      ↓
security / bonding
```

## Bước 7 — GATT discovery

```text
ABF0
 ↓
ABF1
 ↓
ABF2
 ↓
CCCD
```

## Bước 8 — READY

```c
ble_central_is_connected(
    device_id);
```

trả về true.

## Bước 9 — Send command

```c
ble_central_send_command(
    device_id,
    &message);
```

## Bước 10 — Receive status

Peripheral:

```text
STATUS notify
```

Gateway:

```text
on_device_notify()
```

---

# 47. Sequence diagram

```text
Gateway                Peripheral
   │                        │
   │------ Scan ----------->│
   │<--- Advertisement -----│
   │      UUID ABF0         │
   │                        │
   │------ Connect -------->│
   │<---- Connected --------│
   │                        │
   │---- Security --------->│
   │<---- Encrypted --------│
   │                        │
   │-- Discover ABF0 ------>│
   │<------ Service --------│
   │                        │
   │-- Discover chars ----->│
   │<--- ABF1 / ABF2 -------│
   │                        │
   │-- Discover CCCD ------>│
   │<------- CCCD ----------│
   │                        │
   │-- Enable notify ------>│
   │                        │
   │       READY            │
   │                        │
   │-- COMMAND / CBOR ----->│
   │       ABF1             │
   │                        │
   │<-- STATUS / CBOR ------│
   │       ABF2             │
   │                        │
```

---

# 48. Quan hệ với toàn Gateway

Kiến trúc tổng thể hiện tại có thể biểu diễn:

```text
              Web / MCP
                  │
                  ▼
        command_dispatcher
                  │
                  ▼
           gw_message_t
                  │
                  ▼
            ble_central
                  │
            CBOR encode
                  │
                  ▼
             COMMAND
               ABF1
                  │
                  ▼
            BLE Device
                  │
               STATUS
               ABF2
                  │
                  ▼
            ble_central
                  │
            CBOR decode
                  │
                  ▼
        command_dispatcher
```

Trong `app_main()`, BLE notification trực tiếp được chuyển tới `command_dispatcher_on_device_notify()`.

---

# 49. Dependency

Component khai báo:

```cmake
REQUIRES
    bt
    cbor_codec
    device_store
```

Ý nghĩa:

```text
bt
 └── ESP-IDF NimBLE

cbor_codec
 └── encode/decode gw_message_t

device_store
 └── registered devices
     BLE address
     connected state
```

---

# 50. Các đặc điểm thiết kế đáng chú ý

### Connection state tách biệt rõ

Không sử dụng đơn giản một biến `connected`.

Thay vào đó:

```text
CONNECTING
SECURING
DISCOVERING
READY
```

giúp tránh gửi command khi BLE link đã tồn tại nhưng GATT chưa sẵn sàng.

### GATT handle được cache

Sau discovery:

```text
command_val_handle
status_val_handle
status_cccd_handle
```

được lưu trong connection slot nên mỗi command không cần discovery lại.

### CBOR ở tầng transport

`ble_central` không cần biết nội dung command cụ thể.

Nó chỉ xử lý:

```text
gw_message_t
      ↕
CBOR
```

### Auto reconnect

Device đã đăng ký và có BLE address sẽ tự động được supervisor thử kết nối lại.

### Adaptive connection interval

Khi số connection tăng:

```text
15 ms → 30 ms → 50 ms
```

giúp scheduler BLE có thêm thời gian phục vụ nhiều link.

---

# 51. Tóm tắt

`components/ble_central` là tầng quản lý **BLE Central hoàn chỉnh** của ESP32 BLE Gateway.

Luồng quan trọng nhất là:

```text
SCAN
  ↓
REGISTER DEVICE
  ↓
CONNECT
  ↓
SECURE
  ↓
DISCOVER SERVICE
  ↓
DISCOVER CHARACTERISTICS
  ↓
SUBSCRIBE STATUS
  ↓
READY
  ↓
COMMAND ↔ STATUS
```

Gateway sử dụng custom GATT protocol:

```text
0xABF0  Gateway Service

0xABF1  COMMAND
        Gateway → Device

0xABF2  STATUS
        Device → Gateway
```

Message ở cả hai chiều được biểu diễn dưới dạng:

```text
gw_message_t
```

và truyền qua BLE dưới dạng CBOR.

Component còn cung cấp ba cơ chế quan trọng cho vận hành lâu dài:

```text
Connection state machine
Automatic reconnect
Discovery timeout recovery
```

Nhờ đó các tầng phía trên như `command_dispatcher`, Web UI hay MCP không cần trực tiếp làm việc với NimBLE/GATT.
