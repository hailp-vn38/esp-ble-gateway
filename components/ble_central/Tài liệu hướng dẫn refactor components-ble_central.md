# Tài liệu hướng dẫn refactor `components/ble_central`

## 1. Mục tiêu

Tài liệu này đề xuất kế hoạch refactor `components/ble_central` của repository `hailp-vn38/esp-ble-gateway` dựa trên implementation hiện tại trên nhánh `main`.

Component hiện được chia thành các phần:

```text
ble_central.c
ble_central_gap.c
ble_central_gatt.c
ble_central_scan.c
ble_central_state.c
ble_central_supervisor.c
```

Cấu trúc này nhìn chung hợp lý và không cần viết lại toàn bộ. CMake hiện đăng ký đúng sáu source file trên cùng và phụ thuộc vào `bt`, `cbor_codec`, `device_store`. 

Mục tiêu của refactor:

- hỗ trợ `DEVICE_STORE_MAX_DEVICES` lớn hơn số connection BLE tối đa;
- không giữ connection slot cho thiết bị offline;
- reconnect công bằng giữa nhiều thiết bị;
- không block NimBLE host task;
- loại bỏ phần lớn race condition;
- quản lý lifecycle connection rõ ràng;
- xử lý MTU an toàn;
- tách rõ **registered device** và **active BLE connection**;
- giữ public API hiện tại càng nhiều càng tốt;
- chuẩn bị kiến trúc cho 10–16 thiết bị đã đăng ký nhưng chỉ 9 link BLE đồng thời.

---

# 2. Vấn đề của kiến trúc hiện tại

## 2.1. `ble_conn_slot_t` đang đảm nhiệm hai vai trò

Hiện tại:

```c
typedef struct {
    ble_conn_slot_state_t state;
    bool forget_requested;

    char device_id[GW_MSG_DEVICE_ID_LEN];
    ble_addr_t peer_addr;

    uint16_t conn_handle;

    ...
    int64_t last_attempt_ms;
    int64_t discovery_started_ms;
} ble_conn_slot_t;
```

và array:

```c
ble_conn_slot_t g_ble_connections[BLE_CENTRAL_MAX_CONN];
```

Trong khi:

```c
#define BLE_CENTRAL_MAX_CONN CONFIG_BT_NIMBLE_MAX_CONNECTIONS
```



Cấu hình hiện tại:

```text
CONFIG_BT_NIMBLE_MAX_CONNECTIONS=9
```

nhưng `device_store` cho phép 16 thiết bị.  

Hiện tại slot vừa đại diện cho:

```text
registered device
```

vừa đại diện cho:

```text
active BLE connection
```

Đây là nguyên nhân gốc của nhiều vấn đề.

---

# 3. Kiến trúc đích

Nên chuyển sang kiến trúc ba lớp:

```text
                     Device Store
                         │
                         │ persistent data
                         ▼
                BLE Device Runtime
             DEVICE_STORE_MAX_DEVICES
                         │
                         │ scheduler
                         ▼
                BLE Connection Pool
             BLE_CENTRAL_MAX_CONN
                         │
                         ▼
                    NimBLE Host
```

Trong đó:

```text
Device Store
    |
    | device_id
    | peer address
    |
    v
Device Runtime
    |
    | reconnect policy
    | retry/backoff
    | connection_slot
    |
    v
Connection Slot
    |
    | conn_handle
    | GATT handles
    | connection state
    |
    v
NimBLE
```

Điểm quan trọng:

> **Device runtime tồn tại lâu dài. Connection slot chỉ tồn tại trong thời gian connection BLE tồn tại hoặc đang được thiết lập.**

---

# 4. Data model mới

## 4.1. Device runtime

Thêm type:

```c
typedef struct {
    bool in_use;

    char device_id[GW_MSG_DEVICE_ID_LEN];

    ble_addr_t peer_addr;
    bool has_peer_addr;

    int connection_slot;

    bool reconnect_enabled;

    uint8_t retry_count;

    int64_t last_attempt_ms;
    int64_t next_retry_ms;
} ble_device_runtime_t;
```

Array:

```c
static ble_device_runtime_t
    s_devices[DEVICE_STORE_MAX_DEVICES];
```

Trong đó:

```text
connection_slot = -1
```

nghĩa là thiết bị hiện không sở hữu BLE connection.

---

## 4.2. Connection slot

`ble_conn_slot_t` nên trở thành cấu trúc thuần connection:

```c
typedef struct {
    ble_conn_slot_state_t state;

    int device_index;

    uint16_t conn_handle;

    uint16_t service_start_handle;
    uint16_t service_end_handle;

    uint16_t command_val_handle;
    uint16_t status_val_handle;
    uint16_t status_cccd_handle;

    uint16_t mtu;

    int64_t started_ms;
    int64_t discovery_started_ms;
} ble_conn_slot_t;
```

Không cần giữ:

```c
char device_id[];
ble_addr_t peer_addr;
int64_t last_attempt_ms;
```

trong connection slot nữa.

Những dữ liệu đó thuộc `ble_device_runtime_t`.

---

# 5. State machine mới

## 5.1. Connection slot

Connection slot chỉ có:

```text
FREE
 │
 ▼
CONNECTING
 │
 ▼
SECURING
 │
 ▼
DISCOVERING
 │
 ▼
READY
 │
 │ disconnect
 ▼
FREE
```

Không cần `IDLE`.

Có thể đổi enum thành:

```c
typedef enum {
    BLE_CONN_FREE = 0,
    BLE_CONN_CONNECTING,
    BLE_CONN_SECURING,
    BLE_CONN_DISCOVERING,
    BLE_CONN_READY,
    BLE_CONN_DISCONNECTING,
} ble_conn_state_t;
```

### Tại sao bỏ `IDLE`?

`IDLE` là trạng thái của **device**, không phải của connection.

Device offline:

```text
device.connection_slot = -1
```

là đủ.

---

# 6. Runtime state của device

Device runtime có state riêng:

```c
typedef enum {
    BLE_DEVICE_OFFLINE = 0,
    BLE_DEVICE_CONNECTING,
    BLE_DEVICE_CONNECTED,
    BLE_DEVICE_BACKOFF,
} ble_device_state_t;
```

Có thể thêm vào:

```c
typedef struct {
    ...
    ble_device_state_t state;
    ...
} ble_device_runtime_t;
```

Luồng:

```text
           ┌─────────────┐
           │   OFFLINE   │
           └──────┬──────┘
                  │ scheduler
                  ▼
           ┌─────────────┐
           │ CONNECTING  │
           └──────┬──────┘
                  │ success
                  ▼
           ┌─────────────┐
           │  CONNECTED  │
           └──────┬──────┘
                  │ disconnect
                  ▼
           ┌─────────────┐
           │   BACKOFF   │
           └──────┬──────┘
                  │ timeout
                  └──────────► OFFLINE
```

Như vậy:

```text
Device state
```

và:

```text
BLE connection state
```

không còn bị trộn lẫn.

---

# 7. Cấu trúc thư mục sau refactor

Tôi đề xuất:

```text
components/ble_central/
│
├── include/
│   └── ble_central.h
│
├── ble_central.c
├── ble_central_internal.h
│
├── ble_central_state.c
├── ble_central_runtime.c
│
├── ble_central_gap.c
├── ble_central_gatt.c
│
├── ble_central_scan.c
│
├── ble_central_notify.c
│
├── ble_central_supervisor.c
│
├── CMakeLists.txt
│
└── test/
    ├── test_ble_state.c
    ├── test_ble_scheduler.c
    └── test_ble_runtime.c
```

Vai trò:

```text
ble_central.c
    public API / facade

ble_central_state.c
    connection pool
    mutex
    connection allocation/free

ble_central_runtime.c
    device runtime
    retry state
    device ↔ connection mapping

ble_central_gap.c
    NimBLE GAP callbacks

ble_central_gatt.c
    GATT discovery
    CCCD
    MTU

ble_central_notify.c
    queue + notification worker

ble_central_supervisor.c
    reconnect scheduler
    timeout checking

ble_central_scan.c
    BLE scan
```

---

# 8. Refactor giai đoạn 1 — Connection pool

Đây là phần nên làm đầu tiên.

## 8.1. API nội bộ

Trong `ble_central_internal.h` thêm:

```c
int ble_conn_allocate(int device_index);

void ble_conn_release(int slot_index);

ble_conn_slot_t *ble_conn_get(int slot_index);

int ble_conn_find_by_handle(uint16_t conn_handle);

int ble_conn_active_count(void);

bool ble_conn_has_connecting(void);
```

Không nên trả pointer ra ngoài lâu dài nếu có thể tránh.

---

## 8.2. Allocate

Pseudo implementation:

```c
int ble_conn_allocate(int device_index)
{
    if (!ble_central_lock_connections()) {
        return -1;
    }

    for (int i = 0; i < BLE_CENTRAL_MAX_CONN; i++) {
        ble_conn_slot_t *slot = &g_ble_connections[i];

        if (slot->state != BLE_CONN_FREE) {
            continue;
        }

        memset(slot, 0, sizeof(*slot));

        slot->state = BLE_CONN_CONNECTING;
        slot->device_index = device_index;
        slot->conn_handle = BLE_HS_CONN_HANDLE_NONE;

        ble_central_unlock_connections();

        return i;
    }

    ble_central_unlock_connections();

    return -1;
}
```

---

## 8.3. Release

```c
void ble_conn_release(int slot_index)
{
    if (slot_index < 0 ||
        slot_index >= BLE_CENTRAL_MAX_CONN) {
        return;
    }

    if (!ble_central_lock_connections()) {
        return;
    }

    memset(
        &g_ble_connections[slot_index],
        0,
        sizeof(g_ble_connections[slot_index])
    );

    g_ble_connections[slot_index].state =
        BLE_CONN_FREE;

    g_ble_connections[slot_index].device_index = -1;

    g_ble_connections[slot_index].conn_handle =
        BLE_HS_CONN_HANDLE_NONE;

    ble_central_unlock_connections();
}
```

### Nguyên tắc

Sau:

```text
BLE_GAP_EVENT_DISCONNECT
```

slot phải:

```text
READY
   ↓
FREE
```

chứ không:

```text
READY
   ↓
IDLE
```

như hiện tại.

---

# 9. Refactor giai đoạn 2 — Device runtime registry

Tạo:

```text
ble_central_runtime.c
```

## API

```c
int ble_runtime_init(void);

int ble_runtime_find(const char *device_id);

int ble_runtime_register(
    const char *device_id,
    const ble_addr_t *address
);

void ble_runtime_remove(int device_index);

void ble_runtime_set_connection(
    int device_index,
    int slot_index
);

void ble_runtime_clear_connection(
    int device_index
);
```

---

## Runtime initialization

Có thể đồng bộ từ `device_store`:

```c
device_entry_t entries[DEVICE_STORE_MAX_DEVICES];

int count = device_store_snapshot(
    entries,
    DEVICE_STORE_MAX_DEVICES
);
```

sau đó:

```c
for (int i = 0; i < count; i++) {
    if (!entries[i].has_ble_addr) {
        continue;
    }

    ble_runtime_register(...);
}
```

---

# 10. Refactor `ble_central_connect()`

Implementation hiện tại tự tìm/allocate `ble_conn_slot_t`, rồi giữ slot ở `IDLE` khi disconnected. 

Sau refactor, flow nên là:

```text
ble_central_connect(device)
          │
          ▼
find runtime device
          │
          ▼
already connected?
      ┌───┴────┐
      │        │
     YES       NO
      │        │
   return     ▼
        free connection slot?
             │
         ┌───┴────┐
         │        │
        NO       YES
         │        │
      BUSY        ▼
                allocate
                  │
                  ▼
            ble_gap_connect()
```

Pseudo code:

```c
int ble_central_connect(
    const char *device_id,
    const uint8_t *ble_addr,
    uint8_t addr_type)
{
    if (!device_id || !ble_addr) {
        return BLE_CENTRAL_ERR_INVALID_ARG;
    }

    if (!ble_host_is_ready()) {
        return BLE_CENTRAL_ERR_NOT_READY;
    }

    int device_index =
        ble_runtime_find_or_register(
            device_id,
            ble_addr,
            addr_type
        );

    if (device_index < 0) {
        return BLE_CENTRAL_ERR_DEVICE;
    }

    ble_device_runtime_t device;

    if (!ble_runtime_snapshot(
            device_index,
            &device)) {
        return BLE_CENTRAL_ERR_DEVICE;
    }

    if (device.connection_slot >= 0) {
        return BLE_CENTRAL_ERR_BUSY;
    }

    if (ble_conn_has_connecting()) {
        return BLE_CENTRAL_ERR_CONNECT_IN_PROGRESS;
    }

    int slot_index =
        ble_conn_allocate(device_index);

    if (slot_index < 0) {
        return BLE_CENTRAL_ERR_NO_SLOT;
    }

    ble_runtime_set_connection(
        device_index,
        slot_index
    );

    ...
}
```

---

# 11. Không cho nhiều connection procedure chạy cùng lúc

Hiện supervisor chỉ:

```c
ble_central_connect(...);
break;
```

nhưng vòng kế tiếp vẫn có thể thử thiết bị khác trong khi connection trước còn `CONNECTING`. 

Thêm:

```c
bool ble_conn_has_connecting(void)
{
    ...
}
```

Supervisor:

```c
if (ble_conn_has_connecting()) {
    continue;
}
```

Kết quả:

```text
A connecting
    │
    ├─ 1 s supervisor tick
    ├─ 2 s supervisor tick
    ├─ ...
    │
    └─ callback received
          │
          ▼
       scheduler tiếp tục
```

Không gọi:

```text
connect B
connect C
```

trong lúc A đang thiết lập link.

---

# 12. Refactor reconnect scheduler

## 12.1. Không lưu retry trong connection slot

Hiện:

```c
slot->last_attempt_ms
```

nằm trong `ble_conn_slot_t`. 

Sau refactor chuyển sang:

```c
device->last_attempt_ms;
device->next_retry_ms;
device->retry_count;
```

---

## 12.2. Exponential backoff

Dùng:

```c
#define BLE_RETRY_INITIAL_MS 2000
#define BLE_RETRY_MAX_MS     30000
```

Function:

```c
static uint32_t calculate_backoff(uint8_t retry)
{
    uint32_t delay =
        BLE_RETRY_INITIAL_MS << retry;

    if (delay > BLE_RETRY_MAX_MS) {
        delay = BLE_RETRY_MAX_MS;
    }

    return delay;
}
```

Ví dụ:

```text
failure 1    2 s
failure 2    4 s
failure 3    8 s
failure 4   16 s
failure 5   30 s
failure 6   30 s
```

---

# 13. Scheduler công bằng

Không nên luôn bắt đầu từ:

```c
for (int i = 0; i < count; i++)
```

vì thiết bị đầu danh sách có lợi thế.

Thêm:

```c
static int s_scheduler_cursor;
```

Mỗi vòng:

```c
for (int offset = 0; offset < count; offset++) {

    int i =
        (s_scheduler_cursor + offset) % count;

    ...
}
```

Sau khi chọn device:

```c
s_scheduler_cursor =
    (selected_index + 1) % count;
```

Đây là:

```text
round-robin reconnect scheduler
```

Ví dụ:

```text
tick 1 → Device A
tick 2 → Device B
tick 3 → Device C
tick 4 → Device D
tick 5 → Device A
```

thay vì:

```text
A
A
A
A
...
```

---

# 14. Supervisor mới

Pseudo implementation:

```c
static void reconnect_supervisor_task(void *arg)
{
    while (supervisor_should_run()) {

        int64_t now =
            esp_timer_get_time() / 1000;

        ble_supervisor_check_timeouts(now);

        if (!ble_host_is_ready()) {
            goto sleep;
        }

        if (ble_gap_disc_active()) {
            goto sleep;
        }

        if (ble_conn_has_connecting()) {
            goto sleep;
        }

        int device_index =
            ble_scheduler_next_device(now);

        if (device_index >= 0) {
            ble_scheduler_connect(device_index);
        }

sleep:

        vTaskDelay(
            pdMS_TO_TICKS(
                BLE_SUPERVISOR_TICK_MS
            )
        );
    }

    supervisor_mark_stopped();

    vTaskDelete(NULL);
}
```

---

# 15. Refactor notification handling

Đây là thay đổi quan trọng thứ hai sau connection pool.

Hiện tại:

```text
NimBLE
   │
   ▼
BLE_GAP_EVENT_NOTIFY_RX
   │
   ▼
CBOR decode
   │
   ▼
notify_cb()
   │
   ▼
command_dispatcher
```

Callback `command_dispatcher` cuối cùng có thể đợi mutex tới 1000 ms.  

Không nên làm việc này trong NimBLE host.

---

# 16. Notification queue

Tạo:

```text
ble_central_notify.c
```

Data:

```c
typedef struct {
    char device_id[GW_MSG_DEVICE_ID_LEN];
    gw_message_t message;
} ble_notify_event_t;
```

Queue:

```c
#define BLE_NOTIFY_QUEUE_SIZE 16

static QueueHandle_t s_notify_queue;
```

---

## Worker

```c
static void ble_notify_task(void *arg)
{
    ble_notify_event_t event;

    while (1) {

        if (xQueueReceive(
                s_notify_queue,
                &event,
                portMAX_DELAY) != pdTRUE) {
            continue;
        }

        ble_central_notify_cb_t cb =
            ble_central_notify_callback();

        if (cb) {
            cb(
                event.device_id,
                &event.message
            );
        }
    }
}
```

---

# 17. GAP callback sau refactor

NimBLE callback chỉ:

```text
validate
decode
queue
return
```

Ví dụ:

```c
case BLE_GAP_EVENT_NOTIFY_RX:
{
    ...

    gw_message_t message;

    if (cbor_codec_decode(
            buffer,
            copied,
            &message) != 0) {
        return 0;
    }

    ble_notify_push(
        device_id,
        &message
    );

    return 0;
}
```

Tuyệt đối tránh trong NimBLE callback:

```text
xSemaphoreTake(..., 1000 ms)
HTTP request
NVS write dài
waiting ACK
complex dispatcher
```

---

# 18. Queue full phải làm gì?

Không block NimBLE.

Không dùng:

```c
xQueueSend(
    queue,
    &event,
    portMAX_DELAY
);
```

Dùng:

```c
if (xQueueSend(
        queue,
        &event,
        0) != pdTRUE) {

    ESP_LOGW(
        TAG,
        "BLE notify queue full"
    );
}
```

Tức là ưu tiên:

```text
drop event
```

hơn:

```text
block BLE host
```

Nếu notification mang ACK rất quan trọng, tăng queue lên:

```text
16 hoặc 32
```

và thêm metric:

```c
uint32_t notify_dropped;
```

---

# 19. MTU-safe `send_command()`

Hiện codec cho phép:

```c
#define GW_MSG_MAX_LEN 256
```



Trong khi cấu hình preferred ATT MTU là 256. 

ATT Write Command cần 3 byte header:

```text
maximum payload = negotiated MTU - 3
```

Không nên giả định MTU luôn 256.

---

## Sau MTU exchange

Trong:

```text
BLE_GAP_EVENT_MTU
```

lưu:

```c
slot->mtu = event->mtu.value;
```

Default khi connect:

```c
slot->mtu = 23;
```

---

## Trước khi gửi

```c
int length =
    cbor_codec_encode(
        msg,
        buffer,
        sizeof(buffer)
    );

if (length <= 0) {
    return BLE_CENTRAL_ERR_ENCODE;
}

uint16_t max_payload =
    slot.mtu > 3
        ? slot.mtu - 3
        : 0;

if (length > max_payload) {

    ESP_LOGE(
        TAG,
        "[%s] payload=%d exceeds MTU payload=%u",
        device_id,
        length,
        max_payload
    );

    return BLE_CENTRAL_ERR_MESSAGE_TOO_LARGE;
}
```

---

# 20. Error code rõ ràng

Hiện các API phần lớn trả:

```text
0
-1
-2
-3
```

Nên định nghĩa:

```c
typedef enum {
    BLE_CENTRAL_OK = 0,

    BLE_CENTRAL_ERR_INVALID_ARG = -1,
    BLE_CENTRAL_ERR_NOT_READY = -2,
    BLE_CENTRAL_ERR_NOT_FOUND = -3,
    BLE_CENTRAL_ERR_NO_SLOT = -4,
    BLE_CENTRAL_ERR_BUSY = -5,
    BLE_CENTRAL_ERR_CONNECT_IN_PROGRESS = -6,
    BLE_CENTRAL_ERR_NOT_CONNECTED = -7,
    BLE_CENTRAL_ERR_ENCODE = -8,
    BLE_CENTRAL_ERR_MESSAGE_TOO_LARGE = -9,
    BLE_CENTRAL_ERR_STACK = -10,
} ble_central_error_t;
```

Nhờ đó caller có thể phân biệt:

```text
NO_SLOT
```

với:

```text
device not registered
```

hay:

```text
BLE host not synced
```

---

# 21. Refactor `BLE_GAP_EVENT_CONNECT`

Flow mới:

```text
CONNECT event
     │
 ┌───┴────┐
 │        │
fail    success
 │        │
 ▼        ▼
release   slot.conn_handle
slot      SECURING
 │        │
 ▼        ▼
schedule security
retry
```

Pseudo:

```c
case BLE_GAP_EVENT_CONNECT:
{
    ble_conn_slot_t slot;

    if (!ble_conn_snapshot(
            slot_index,
            &slot)) {
        return 0;
    }

    if (event->connect.status != 0) {

        ble_runtime_on_connect_failed(
            slot.device_index
        );

        ble_conn_release(slot_index);

        return 0;
    }

    ble_conn_set_handle(
        slot_index,
        event->connect.conn_handle
    );

    ble_conn_set_state(
        slot_index,
        BLE_CONN_SECURING
    );

    ...

    return 0;
}
```

---

# 22. Refactor disconnect

Hiện disconnect chuyển slot về `IDLE`. 

Sau refactor:

```text
disconnect
    │
    ├─ device_store.connected = false
    │
    ├─ runtime.connection_slot = -1
    │
    ├─ runtime.state = BACKOFF
    │
    ├─ runtime.next_retry = ...
    │
    └─ connection slot = FREE
```

Pseudo:

```c
case BLE_GAP_EVENT_DISCONNECT:
{
    int device_index =
        ble_conn_get_device(slot_index);

    device_store_set_connected(
        device_id,
        0
    );

    ble_runtime_on_disconnect(
        device_index
    );

    ble_conn_release(
        slot_index
    );

    return 0;
}
```

---

# 23. Manual disconnect và auto reconnect

Cần quyết định semantic.

Tôi đề xuất:

```c
ble_central_disconnect(device_id)
```

chỉ disconnect hiện tại nhưng vẫn cho reconnect supervisor hoạt động.

Nếu muốn người dùng chủ động disable reconnect thì nên có API khác:

```c
ble_central_set_auto_reconnect(
    device_id,
    false
);
```

hoặc:

```c
ble_central_pause_device(device_id);
```

Tránh dùng `disconnect()` cho hai ý nghĩa khác nhau.

---

# 24. Forget device

Nên thay flow hiện tại:

```text
device_store_delete
     ↓
ble_central_forget_device
```

bằng:

```text
ble_central_forget_device
     ↓
disconnect
     ↓
delete BLE bond
     ↓
remove runtime
     ↓
device_store_delete
```

Hiện `command_dispatcher` delete `device_store` trước rồi mới gọi `ble_central_forget_device()`, khiến trường hợp device không còn slot có thể không lấy được BLE address để xóa bond. 

---

## API tốt hơn

Có thể tạo orchestration ở command layer:

```c
device_entry_t device;

if (device_store_get(
        device_id,
        &device) != 0) {
    ...
}

ble_central_forget_device(
    device_id
);

device_store_delete(
    device_id
);
```

Hoặc truyền address:

```c
ble_central_forget_peer(
    device_id,
    device.ble_addr,
    device.ble_addr_type
);
```

---

# 25. Supervisor lifecycle

Không nên dùng:

```c
volatile bool s_reconnect_running;
```

làm lifecycle control duy nhất.

Thay bằng:

```c
typedef enum {
    BLE_SUPERVISOR_STOPPED,
    BLE_SUPERVISOR_RUNNING,
    BLE_SUPERVISOR_STOPPING
} ble_supervisor_state_t;
```

State cần được bảo vệ bằng mutex hoặc atomic.

Flow:

```text
STOPPED
   │ start
   ▼
RUNNING
   │ stop
   ▼
STOPPING
   │ task exits
   ▼
STOPPED
```

`start()` chỉ cho tạo task khi:

```text
state == STOPPED
```

---

# 26. Host synced state

Thay:

```c
volatile bool g_ble_host_synced;
```

bằng EventGroup là rõ ràng hơn.

Ví dụ:

```c
#define BLE_EVENT_HOST_SYNCED BIT0

static EventGroupHandle_t s_ble_events;
```

Khi sync:

```c
xEventGroupSetBits(
    s_ble_events,
    BLE_EVENT_HOST_SYNCED
);
```

Khi reset:

```c
xEventGroupClearBits(
    s_ble_events,
    BLE_EVENT_HOST_SYNCED
);
```

Kiểm tra:

```c
bool ble_host_is_ready(void)
{
    EventBits_t bits =
        xEventGroupGetBits(s_ble_events);

    return
        (bits & BLE_EVENT_HOST_SYNCED) != 0;
}
```

---

# 27. Không giữ mutex trong lúc gọi NimBLE

Một rule nên áp dụng toàn component:

> **Không giữ `connection_mutex` khi gọi API NimBLE có khả năng callback hoặc thực hiện operation phức tạp.**

Pattern tốt:

```c
lock();

copy state;

update state;

unlock();

ble_gap_connect(...);
```

Không:

```c
lock();

ble_gap_connect(...);

unlock();
```

Code hiện tại đã thực hiện pattern này ở khá nhiều nơi; nên duy trì nhất quán. 

---

# 28. Không trả raw slot pointer qua nhiều task

Hiện nhiều API dùng:

```c
ble_conn_slot_t *slot
```

làm callback argument.

Cách an toàn hơn về lâu dài:

```text
slot_index
+
conn_handle
```

Callback xác thực:

```c
if (!ble_conn_snapshot(
        slot_index,
        &slot)) {
    return;
}

if (slot.conn_handle != conn_handle) {
    return;
}
```

Như vậy callback cũ không dễ tác động lên state mới.

---

# 29. Harden stale callback

Có thể bổ sung:

```c
uint32_t generation;
```

cho connection slot.

Mỗi allocation:

```c
slot->generation++;
```

Logical connection identity trở thành:

```text
slot_index
+
generation
+
conn_handle
```

Callback chỉ hợp lệ khi cả ba trùng.

Đây không phải bước đầu tiên cần triển khai, nhưng đáng thêm sau khi connection pool hoạt động ổn định.

---

# 30. GATT discovery validation

Hiện code tìm characteristic chủ yếu bằng UUID. 

Nên validate properties.

COMMAND phải có:

```c
BLE_GATT_CHR_PROP_WRITE_NO_RSP
```

STATUS phải có:

```c
BLE_GATT_CHR_PROP_NOTIFY
```

Pseudo:

```c
if (uuid == BLE_GATEWAY_COMMAND_UUID) {

    if (!(characteristic->properties &
          BLE_GATT_CHR_PROP_WRITE_NO_RSP)) {

        ble_central_abort_discovery(...);

        return 0;
    }

    ...
}
```

Tương tự STATUS.

Điều này giúp lỗi protocol được phát hiện ngay trong discovery.

---

# 31. Discovery timeout

Timeout hiện có:

```c
#define BLE_DISCOVERY_TIMEOUT_MS 10000
```



Nên giữ.

Nhưng nên tách:

```c
#define BLE_SECURITY_TIMEOUT_MS   10000
#define BLE_GATT_DISCOVERY_TIMEOUT_MS 10000
```

để log chính xác hơn:

```text
security timeout
```

và:

```text
GATT discovery timeout
```

---

# 32. Logging chuẩn hóa

Nên thống nhất log:

```text
[device-id][slot=2][handle=7] CONNECTING
[device-id][slot=2][handle=7] SECURING
[device-id][slot=2][handle=7] DISCOVERING
[device-id][slot=2][handle=7] READY

[device-id][slot=2][handle=7] DISCONNECTED reason=...
```

Ví dụ macro:

```c
#define BLE_LOGI(device, slot, fmt, ...) \
    ESP_LOGI(TAG, "[%s][slot=%d] " fmt, \
             device, slot, ##__VA_ARGS__)
```

Điều này rất hữu ích khi có nhiều connection đồng thời.

---

# 33. Metrics nên bổ sung

Tạo:

```c
typedef struct {
    uint32_t connect_attempts;
    uint32_t connect_success;
    uint32_t connect_failures;

    uint32_t disconnects;

    uint32_t security_failures;
    uint32_t discovery_failures;

    uint32_t notify_received;
    uint32_t notify_dropped;

    uint32_t reconnect_attempts;
} ble_central_metrics_t;
```

Không nhất thiết expose ngay qua public API.

Sau này có thể đưa vào:

```text
gateway get_status
```

để debug thiết bị thực tế.

---

# 34. Public API nên giữ tương thích

Header hiện expose các API như:

```c
ble_central_init()
ble_central_connect()
ble_central_disconnect()
ble_central_forget_device()
ble_central_send_command()
ble_central_is_connected()
ble_central_active_count()
ble_central_scan_start()
...
```



Tôi khuyến nghị **không thay public API trong giai đoạn đầu**.

Refactor phần implementation trước:

```text
command_dispatcher
web_server
main
```

không phải thay đổi lớn.

Sau khi ổn định mới bổ sung API như:

```c
ble_central_get_device_state();

ble_central_get_connection_info();

ble_central_set_auto_reconnect();

ble_central_get_metrics();
```

---

# 35. CMake sau refactor

Hiện có:

```cmake
idf_component_register(
    SRCS
        "ble_central.c"
        "ble_central_gap.c"
        "ble_central_gatt.c"
        "ble_central_scan.c"
        "ble_central_state.c"
        "ble_central_supervisor.c"
    INCLUDE_DIRS "include"
    REQUIRES bt cbor_codec device_store
)
```



Sau refactor:

```cmake
idf_component_register(
    SRCS
        "ble_central.c"
        "ble_central_state.c"
        "ble_central_runtime.c"
        "ble_central_gap.c"
        "ble_central_gatt.c"
        "ble_central_scan.c"
        "ble_central_notify.c"
        "ble_central_supervisor.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        bt
        cbor_codec
        device_store
)
```

---

# 36. Thứ tự triển khai thực tế

Không nên refactor toàn bộ một commit.

Tôi đề xuất các commit độc lập:

## Commit 1 — Introduce device runtime

Thêm:

```text
ble_central_runtime.c
```

và:

```c
ble_device_runtime_t
```

Chưa thay behavior.

Mục tiêu:

```text
build OK
behavior unchanged
```

---

## Commit 2 — Convert connection slots to real pool

Bỏ:

```text
IDLE
```

Tạo:

```text
FREE → CONNECTING → ... → READY → FREE
```

Chuyển retry metadata ra khỏi slot.

Đây là commit quan trọng nhất.

---

## Commit 3 — Refactor reconnect scheduler

Thêm:

```text
round robin
next_retry_ms
retry_count
exponential backoff
connect-in-progress guard
```

---

## Commit 4 — Async notification delivery

Thêm:

```text
ble_central_notify.c
Queue
Worker task
```

NimBLE callback không gọi dispatcher trực tiếp nữa.

---

## Commit 5 — MTU-safe transport

Lưu negotiated MTU.

Validate:

```text
CBOR size <= MTU - 3
```

---

## Commit 6 — Fix device/bond deletion lifecycle

Sửa:

```text
forget
bond delete
runtime delete
device_store delete
```

---

## Commit 7 — Supervisor lifecycle

Bỏ dependency vào:

```c
volatile bool
```

Tạo state machine supervisor.

---

## Commit 8 — GATT hardening

Thêm:

```text
characteristic property validation
stale callback validation
better errors
```

---

## Commit 9 — Tests

Hoàn thiện test cho state/scheduler/runtime.

---

# 37. Test bắt buộc

Component hiện chưa có test suite BLE tương đương `device_store`; test application hiện chỉ require `device_store` và `log_buffer`. 

Sau refactor cần ít nhất các test sau.

## Test 1 — Slot allocation

```text
allocate slot 0
allocate slot 1
...
allocate slot N

N+1 → NO_SLOT
```

---

## Test 2 — Slot reuse

```text
allocate device A
release A

allocate device B

B phải nhận lại slot vừa giải phóng
```

Đây là regression test quan trọng cho lỗi hiện tại.

---

## Test 3 — 16 devices / 9 connections

```text
registered devices = 16
max connections    = 9
```

Phải đảm bảo:

```text
runtime entries = 16
connection slots = 9
```

và 7 device offline không làm mất slot.

---

## Test 4 — Reconnect fairness

Cho:

```text
A
B
C
D
```

đều offline.

Scheduler phải lần lượt xét:

```text
A → B → C → D
```

thay vì:

```text
A → A → A → A
```

---

## Test 5 — Backoff

Kỳ vọng:

```text
retry 0 → 2 s
retry 1 → 4 s
retry 2 → 8 s
retry 3 → 16 s
retry 4 → 30 s
retry 5 → 30 s
```

---

## Test 6 — Successful reconnect resets backoff

```text
retry_count = 5
       │
connect success
       ▼
retry_count = 0
```

---

## Test 7 — Disconnect releases slot

```text
READY
 │
 │ disconnect event
 ▼
FREE
```

và:

```text
device.connection_slot == -1
```

---

## Test 8 — Queue overflow

Push nhiều hơn:

```text
BLE_NOTIFY_QUEUE_SIZE
```

Không được:

```text
deadlock
assert
block NimBLE
```

Metric:

```text
notify_dropped
```

phải tăng.

---

## Test 9 — MTU

```text
MTU = 23
```

payload:

```text
20 bytes → OK
21 bytes → ERR_MESSAGE_TOO_LARGE
```

Với:

```text
MTU = 256
```

payload:

```text
253 bytes → OK
254 bytes → reject
```

---

## Test 10 — Forget offline device

Device:

```text
registered
bonded
offline
connection_slot = -1
```

Sau forget:

```text
runtime removed
bond removed
device store removed
```

---

# 38. Integration test trên ESP32

Sau unit tests, nên test thực tế theo matrix:

| Scenario | Kết quả mong đợi |
|---|---|
| 1 peripheral | Connect → Secure → Discover → Ready |
| peripheral power off | Disconnect → backoff |
| power on lại | Auto reconnect |
| 3 peripheral cùng mất điện | Reconnect round-robin |
| device liên tục fail | Không chặn device khác |
| scan khi reconnect | Không gây duplicate connect |
| delete online device | Disconnect + bond delete |
| delete offline device | Bond vẫn được xóa |
| notification burst | BLE host không watchdog |
| payload > MTU | Reject trước GATT write |
| NimBLE host reset | Runtime chuyển offline và recover |
| 16 registered / 9 max conn | Không xảy ra slot exhaustion giả |

---

# 39. Acceptance criteria

Refactor chỉ nên được xem là hoàn tất khi thỏa các điều kiện:

```text
1. Offline device không giữ BLE connection slot.

2. DEVICE_STORE_MAX_DEVICES có thể lớn hơn
   CONFIG_BT_NIMBLE_MAX_CONNECTIONS.

3. Một device lỗi không ngăn reconnect device khác.

4. Chỉ có một connection procedure được khởi tạo
   tại một thời điểm.

5. Notification callback NimBLE không block
   command dispatcher.

6. Disconnect luôn trả connection slot về FREE.

7. MTU được kiểm tra trước khi GATT write.

8. Stop/start supervisor không tạo duplicate task.

9. Delete offline device vẫn xóa được BLE bond.

10. Connection/runtime shared state có synchronization rõ ràng.

11. Có unit tests cho pool, scheduler và runtime.

12. Public API cũ vẫn hoạt động với
    command_dispatcher hiện tại.
```

---

# 40. Kiến trúc cuối cùng

Sau refactor toàn bộ flow sẽ là:

```text
                         ┌────────────────────┐
                         │    Device Store    │
                         │ persistent config  │
                         └─────────┬──────────┘
                                   │
                                   ▼
                    ┌──────────────────────────┐
                    │    BLE Device Runtime    │
                    │                          │
                    │ device 1                 │
                    │ device 2                 │
                    │ ...                      │
                    │ device 16                │
                    │                          │
                    │ retry / backoff / state  │
                    └────────────┬─────────────┘
                                 │
                                 │ scheduler
                                 ▼
                    ┌──────────────────────────┐
                    │   BLE Connection Pool    │
                    │                          │
                    │ slot 0                   │
                    │ slot 1                   │
                    │ ...                      │
                    │ slot 8                   │
                    │                          │
                    │ max = NimBLE max conn    │
                    └────────────┬─────────────┘
                                 │
             ┌───────────────────┼─────────────────┐
             │                   │                 │
             ▼                   ▼                 ▼
          GAP layer          GATT layer       Scan layer
             │                   │
             └──────────┬────────┘
                        │
                        ▼
                   NimBLE Host
                        │
                        │ notification
                        ▼
                ┌──────────────────┐
                │ FreeRTOS Queue   │
                └────────┬─────────┘
                         │
                         ▼
                ┌──────────────────┐
                │ BLE Notify Task  │
                └────────┬─────────┘
                         │
                         ▼
                Command Dispatcher
```

Đây là kiến trúc tôi khuyến nghị cho gateway hiện tại.

Điểm quan trọng nhất là **không viết lại BLE Central từ đầu**. Component đang có phân lớp khá tốt; nên giữ `GAP`, `GATT`, `scan` và public facade hiện tại, rồi thay đổi phần quản lý state/lifetime bên dưới.

Nếu triển khai đúng theo thứ tự:

```text
runtime registry
       ↓
connection pool
       ↓
scheduler
       ↓
async notification
       ↓
MTU/lifecycle hardening
       ↓
tests
```

thì mỗi bước đều có thể build và kiểm chứng độc lập, đồng thời giảm đáng kể nguy cơ tạo regression lớn trong gateway.