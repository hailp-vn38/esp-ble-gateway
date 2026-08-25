# Tài liệu hướng dẫn refactor `components/ble_central` — v3

> Phiên bản triển khai đã hiệu chỉnh từ v2 sau khi đối chiếu với code hiện tại của `esp-ble-gateway`.
>
> Mục tiêu của v3 là biến tài liệu từ một design proposal thành một implementation specification đủ rõ để refactor theo từng commit mà vẫn giữ build ổn định và giảm rủi ro race/lifecycle bug.

---

## 1. Mục tiêu

Giữ nguyên các lớp lớn đang có:

- `ble_central.c` — public facade;
- `ble_central_gap.c` — GAP event handling;
- `ble_central_gatt.c` — GATT discovery/subscription;
- `ble_central_scan.c` — scan;
- `ble_central_supervisor.c` — reconnect/timeout supervisor.

Refactor phần state/lifetime bên dưới để:

- hỗ trợ `DEVICE_STORE_MAX_DEVICES > CONFIG_BT_NIMBLE_MAX_CONNECTIONS`;
- thiết bị offline không giữ connection slot;
- reconnect công bằng, có backoff;
- chỉ một connect procedure được khởi tạo tại một thời điểm ở giai đoạn đầu;
- không block NimBLE host task;
- mapping `device ↔ slot ↔ conn_handle` được cập nhật atomically;
- giảm race giữa NimBLE callback, supervisor và public API;
- chống stale callback khi slot được tái sử dụng;
- xử lý host reset và disconnect lifecycle rõ ràng;
- xử lý MTU an toàn;
- giữ public API hiện tại càng nhiều càng tốt;
- cho phép mỗi commit refactor vẫn build được.

---

## 2. Nguyên tắc thiết kế bắt buộc

### 2.1. Tách registered device khỏi active connection

Không dùng `ble_conn_slot_t` để giữ cả thông tin persistent/runtime của device và thông tin link BLE.

Phân tách:

```text
Device Store
    ↓
Device Runtime
    ↓
Connection Pool
    ↓
NimBLE
```

### 2.2. Một mutex cho toàn bộ BLE runtime state

Dùng một mutex:

```c
static SemaphoreHandle_t s_state_mutex;
```

Mutex này bảo vệ đồng thời:

```text
s_devices[]
g_ble_connections[]
mapping device.connection_slot
connection state
generation
GATT handles
cached MTU
retry/backoff state
supervisor state nếu cần
```

Không dùng hai mutex riêng cho runtime và connection pool nếu operation phải cập nhật cả hai.

### 2.3. Không gọi NimBLE API khi giữ `s_state_mutex`

Pattern chuẩn:

```text
lock
  snapshot/update state
unlock

call NimBLE API
```

Không:

```text
lock
  ble_gap_connect()
  ble_gap_terminate()
  ble_gattc_...
unlock
```

### 2.4. Không gọi `device_store` khi giữ `s_state_mutex`

`device_store` có mutex riêng. Không lock chéo để tránh deadlock.

### 2.5. Không gọi application callback khi giữ `s_state_mutex`

Application callback phải chạy trong worker riêng.

### 2.6. Không trả raw slot pointer qua task/callback

Không expose:

```c
ble_conn_slot_t *ble_conn_get(...);
```

ra ngoài một scope đang giữ lock.

Ưu tiên:

```c
ble_conn_snapshot(...);
ble_state_update_...(...);
```

---

# 3. Kiến trúc đích

```text
                         ┌──────────────────────┐
                         │     Device Store     │
                         │ persistent identity  │
                         └──────────┬───────────┘
                                    │
                                    ▼
                      ┌────────────────────────┐
                      │   BLE Device Runtime   │
                      │ DEVICE_STORE_MAX_DEV   │
                      │                        │
                      │ reconnect/backoff      │
                      │ lifecycle state        │
                      │ connection_slot        │
                      └───────────┬────────────┘
                                  │ scheduler
                                  ▼
                      ┌────────────────────────┐
                      │  BLE Connection Pool   │
                      │ BLE_CENTRAL_MAX_CONN   │
                      │                        │
                      │ conn_handle            │
                      │ GATT handles           │
                      │ MTU                    │
                      │ generation             │
                      └───────────┬────────────┘
                                  │
                  ┌───────────────┼────────────────┐
                  ▼               ▼                ▼
                 GAP             GATT             Scan
                  └───────────────┬────────────────┘
                                  ▼
                             NimBLE Host
                                  │
                                  │ raw notify
                                  ▼
                         FreeRTOS Notify Queue
                                  │
                                  ▼
                          BLE Notify Worker
                                  │
                                  ▼
                          Command Dispatcher
```

---

# 4. Source of truth

Quy định rõ để tránh hai nguồn trạng thái mâu thuẫn.

## 4.1. BLE lifecycle source of truth

`ble_device_runtime_t` là source of truth cho:

```text
OFFLINE
CONNECTING
CONNECTED
BACKOFF
REMOVING
```

Scheduler chỉ đọc BLE runtime.

## 4.2. `device_store.connected`

`device_store.connected` chỉ là mirror/projection để phục vụ API/UI.

Không dùng field này để quyết định reconnect.

---

# 5. Data model

## 5.1. Device state

```c
typedef enum {
    BLE_DEVICE_OFFLINE = 0,
    BLE_DEVICE_CONNECTING,
    BLE_DEVICE_CONNECTED,
    BLE_DEVICE_BACKOFF,
    BLE_DEVICE_REMOVING,
} ble_device_state_t;
```

`REMOVING` là bắt buộc vì disconnect/cancel là asynchronous.

## 5.2. Device runtime

```c
typedef struct {
    bool in_use;

    char device_id[GW_MSG_DEVICE_ID_LEN];

    ble_addr_t peer_addr;
    bool has_peer_addr;

    int connection_slot;       // -1 nếu không sở hữu slot

    ble_device_state_t state;

    bool reconnect_enabled;

    uint8_t retry_count;

    int64_t last_attempt_ms;
    int64_t next_retry_ms;
} ble_device_runtime_t;
```

Pool:

```c
static ble_device_runtime_t s_devices[DEVICE_STORE_MAX_DEVICES];
```

### Rule

`ble_runtime_remove()`:

- chỉ clear đúng entry;
- không compact array;
- không dịch chuyển các index khác.

`device_index` phải ổn định trong suốt lifetime của connection.

---

# 6. Connection data model

## 6.1. Connection state

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

Không còn `IDLE`.

## 6.2. Connection slot

```c
typedef struct {
    ble_conn_state_t state;

    int device_index;

    uint32_t generation;

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

Pool:

```c
static ble_conn_slot_t g_ble_connections[BLE_CENTRAL_MAX_CONN];
```

---

# 7. `generation` contract

`generation` dùng để phân biệt các lifetime khác nhau của cùng một slot.

## 7.1. Không reset generation khi release

Sai:

```c
slot->generation++;
memset(slot, 0, sizeof(*slot));
```

vì `memset()` xóa generation.

## 7.2. Pattern đúng

### Release

```c
static void ble_conn_reset_to_free_unlocked(int slot_index)
{
    ble_conn_slot_t *slot = &g_ble_connections[slot_index];

    uint32_t generation = slot->generation;

    memset(slot, 0, sizeof(*slot));

    slot->generation = generation;
    slot->state = BLE_CONN_FREE;
    slot->device_index = -1;
    slot->conn_handle = BLE_HS_CONN_HANDLE_NONE;
    slot->mtu = 23;
}
```

### Allocate

```c
static void ble_conn_prepare_new_lifetime_unlocked(
    int slot_index,
    int device_index)
{
    ble_conn_slot_t *slot = &g_ble_connections[slot_index];

    uint32_t generation = slot->generation + 1;

    memset(slot, 0, sizeof(*slot));

    slot->generation = generation;
    slot->state = BLE_CONN_CONNECTING;
    slot->device_index = device_index;
    slot->conn_handle = BLE_HS_CONN_HANDLE_NONE;
    slot->mtu = 23;
}
```

Sequence:

```text
FREE(gen=2)
   ↓ allocate
CONNECTING(gen=3)
   ↓ disconnect
FREE(gen=3)
   ↓ allocate
CONNECTING(gen=4)
```

---

# 8. Stable connection reference

Không truyền raw `ble_conn_slot_t *` làm identity.

Dùng:

```c
typedef struct {
    uint8_t slot_index;
    uint32_t generation;
} ble_conn_ref_t;
```

Sau khi có connection handle:

```c
typedef struct {
    ble_conn_ref_t ref;
    uint16_t conn_handle;
} ble_conn_event_ref_t;
```

Validation:

```c
bool ble_conn_snapshot(
    ble_conn_ref_t ref,
    ble_conn_slot_t *out);
```

Pseudo:

```c
bool ble_conn_snapshot(
    ble_conn_ref_t ref,
    ble_conn_slot_t *out)
{
    bool ok = false;

    if (!out || ref.slot_index >= BLE_CENTRAL_MAX_CONN) {
        return false;
    }

    if (!ble_state_lock()) {
        return false;
    }

    ble_conn_slot_t *slot =
        &g_ble_connections[ref.slot_index];

    if (slot->state != BLE_CONN_FREE &&
        slot->generation == ref.generation) {
        *out = *slot;
        ok = true;
    }

    ble_state_unlock();
    return ok;
}
```

---

# 9. Callback context lifetime

Đây là phần bắt buộc phải định nghĩa trước khi triển khai stale-callback hardening.

Không truyền pointer tới biến stack làm `arg`.

Không dùng callback context nằm bên trong slot nếu slot có thể bị overwrite trước khi callback cũ kết thúc.

## 9.1. Giai đoạn đầu

Với policy chỉ có một connect procedure tại một thời điểm, dùng callback context pool tĩnh:

```c
typedef struct {
    bool in_use;

    ble_conn_ref_t ref;

    uint16_t conn_handle;
} ble_callback_ctx_t;
```

Số context tối thiểu:

```c
#define BLE_CALLBACK_CTX_MAX BLE_CENTRAL_MAX_CONN
```

Một context gắn với một active connection lifetime và chỉ release sau disconnect/final callback.

## 9.2. Callback validation

Callback phải kiểm tra:

```text
slot_index
generation
conn_handle
```

trước khi mutate state.

Nếu mismatch:

```text
stale callback
→ ignore
```

---

# 10. Atomic mapping transaction

Không làm:

```text
has_connecting()
allocate_slot()
set_device_connection()
```

thành ba operation độc lập.

Tạo một transaction duy nhất:

```c
int ble_state_reserve_connection(
    int device_index,
    ble_conn_ref_t *out_ref);
```

Operation dưới `s_state_mutex`:

```text
1. validate device_index
2. device.in_use == true
3. device.state cho phép connect
4. device.connection_slot == -1
5. không có slot CONNECTING
6. tìm slot FREE
7. generation++
8. init slot
9. slot.device_index = device_index
10. device.connection_slot = slot_index
11. device.state = CONNECTING
12. device.last_attempt_ms = now
13. unlock
```

Kết quả mapping luôn nhất quán.

---

# 11. Rollback transaction

Nếu `ble_gap_connect()` trả lỗi đồng bộ:

```c
void ble_state_rollback_connection_start(
    ble_conn_ref_t ref,
    int64_t now_ms);
```

Dưới lock:

```text
validate generation
validate device mapping
release slot
device.connection_slot = -1
device.state = BACKOFF
update retry_count
update next_retry_ms
```

Không để public API/supervisor tự release từng phần.

---

# 12. Connection start orchestration

Tạo:

```c
int ble_connection_start(int device_index);
```

Flow:

```text
ble_state_reserve_connection()
        │
        ▼
snapshot peer address
        │
        ▼
unlock hoàn toàn
        │
        ▼
ble_gap_connect()
        │
   ┌────┴────┐
   │         │
 rc != 0   rc == 0
   │         │
rollback   chờ GAP callback
```

Pseudo:

```c
int ble_connection_start(int device_index)
{
    ble_conn_ref_t ref;

    int rc = ble_state_reserve_connection(
        device_index,
        &ref);

    if (rc != BLE_CENTRAL_OK) {
        return rc;
    }

    ble_addr_t peer_addr;
    if (!ble_runtime_get_peer_addr(
            device_index,
            &peer_addr)) {
        ble_state_rollback_connection_start(
            ref,
            ble_now_ms());
        return BLE_CENTRAL_ERR_NOT_FOUND;
    }

    struct ble_gap_conn_params params;
    ble_build_conn_params(&params);

    ble_callback_ctx_t *ctx =
        ble_callback_ctx_acquire(ref);

    if (!ctx) {
        ble_state_rollback_connection_start(
            ref,
            ble_now_ms());
        return BLE_CENTRAL_ERR_NO_RESOURCE;
    }

    rc = ble_gap_connect(
        g_ble_own_addr_type,
        &peer_addr,
        BLE_CONNECT_TIMEOUT_MS,
        &params,
        ble_central_gap_event_handler,
        ctx);

    if (rc != 0) {
        ble_callback_ctx_release(ctx);
        ble_state_rollback_connection_start(
            ref,
            ble_now_ms());
        return BLE_CENTRAL_ERR_STACK;
    }

    return BLE_CENTRAL_OK;
}
```

---

# 13. Public `ble_central_connect()`

Public API chỉ làm validation + runtime lookup/register + orchestration.

```text
validate args
   ↓
host synced?
   ↓
device exists in device_store?
   ↓
runtime find/register
   ↓
ble_connection_start()
```

Không mutate connection pool trực tiếp trong facade.

---

# 14. Device runtime initialization

Khi `ble_central_init()`:

```text
device_store_snapshot()
    ↓
for each registered device:
    if has BLE address:
        create/update runtime entry
```

Runtime entry:

```text
state = OFFLINE
connection_slot = -1
reconnect_enabled = true
retry_count = 0
```

---

# 15. Runtime sync khi add/edit/delete device

## Add device

Sau khi `device_store_add()` và lưu BLE address:

```text
ble_central_connect()
```

có thể tự register runtime nếu chưa có.

## Edit device

Không cần thay runtime nếu chỉ đổi `name/type`.

## Delete device

Không xóa store trước khi BLE layer lấy address và xử lý async disconnect.

Xem phần `REMOVING`.

---

# 16. Device removal lifecycle

`forget` là operation asynchronous nếu device đang connected/connecting.

## 16.1. Public responsibility

`ble_central_forget_device(device_id)` chịu trách nhiệm:

```text
disable reconnect
cancel/terminate BLE procedure
delete bond khi an toàn
remove BLE runtime khi lifecycle kết thúc
```

Không tự xóa `device_store`.

Device Store vẫn do higher-level orchestration quản lý.

## 16.2. State transition

```text
OFFLINE
   │ forget
   ▼
REMOVING
   │ finalize ngay
   ▼
runtime removed
```

Connected:

```text
CONNECTED
   │ forget
   ▼
REMOVING
   │ ble_gap_terminate()
   ▼
DISCONNECT callback
   │
   ├─ release slot
   ├─ delete bond
   └─ finalize runtime removal
```

Connecting:

```text
CONNECTING
   │ forget
   ▼
REMOVING
   │ ble_gap_conn_cancel()
   ▼
CONNECT failure/cancel callback
   │
   ├─ release slot
   ├─ delete bond
   └─ finalize runtime removal
```

## 16.3. Runtime entry không được reuse khi `REMOVING`

Chỉ set:

```c
in_use = false;
```

sau khi connection/callback lifecycle kết thúc.

---

# 17. Higher-level delete orchestration

`command_dispatcher` hiện phải được điều chỉnh.

Recommended flow:

```text
device_store_get(device_id)
     ↓
ble_central_forget_device(device_id)
     ↓
BLE layer bắt đầu remove
```

Có hai lựa chọn:

### Lựa chọn A — synchronous store deletion sau khi BLE layer snapshot address

`ble_central_forget_device()` trước khi return phải snapshot address cần cho bond deletion vào runtime/remove context.

Sau đó caller có thể:

```c
device_store_delete(device_id);
```

### Lựa chọn B — deferred delete callback

BLE layer báo removal complete rồi higher-level code xóa store.

V3 khuyến nghị **A** để giữ public API đơn giản.

Điều kiện bắt buộc:

> BLE layer không được phụ thuộc vào `device_store_get()` sau khi `ble_central_forget_device()` đã return.

---

# 18. GAP connect event

## Failure

```text
CONNECT event status != 0
     ↓
validate callback ref
     ↓
release connection mapping
     ↓
if device == REMOVING:
    finalize removal
else:
    BACKOFF
```

## Success

```text
CONNECT event success
     ↓
validate ref
     ↓
set conn_handle
     ↓
state = SECURING
     ↓
update callback ctx conn_handle
     ↓
exchange MTU
     ↓
security initiate
```

---

# 19. GAP disconnect event

Flow:

```text
DISCONNECT
    ↓
validate ref + conn_handle
    ↓
snapshot device_id/device_index
    ↓
under state lock:
    release slot
    device.connection_slot = -1

    if REMOVING:
        keep REMOVING
    else:
        state = BACKOFF
        update next_retry
    ↓
unlock
    ↓
device_store_set_connected(..., 0)
    ↓
if REMOVING:
    delete bond + finalize runtime
```

Connection slot phải trở về `FREE`.

---

# 20. Host reset recovery

Host reset phải được coi như mất toàn bộ active BLE runtime link.

Tạo:

```c
void ble_state_handle_host_reset(void);
```

Flow:

```text
NimBLE reset
   ↓
host_synced = false
   ↓
scan reset
   ↓
lock state
   ↓
for every device:
    if CONNECTED / CONNECTING:
        connection_slot = -1

    if REMOVING:
        giữ REMOVING
    else if in_use:
        state = BACKOFF hoặc OFFLINE
        schedule retry
   ↓
for every connection slot:
    invalidate generation/lifetime
    reset to FREE
   ↓
unlock
   ↓
mirror device_store.connected = 0
```

Khi host sync lại:

```text
host_synced = true
   ↓
supervisor reconnect theo scheduler
```

Acceptance criterion:

> Sau host reset không được còn device runtime nào trỏ tới connection slot cũ.

---

# 21. Host sync representation

Không bắt buộc phải giữ `volatile bool`.

Ưu tiên:

```c
EventGroupHandle_t s_ble_events;
#define BLE_EVENT_HOST_SYNCED BIT0
```

Hoặc atomic nếu chỉ cần flag.

Nếu dùng EventGroup:

```c
bool ble_host_is_ready(void);
```

là API duy nhất mà phần còn lại dùng.

---

# 22. Reconnect scheduler

## 22.1. Retry data thuộc device runtime

```c
retry_count
last_attempt_ms
next_retry_ms
```

không nằm trong connection slot.

## 22.2. Backoff

```c
#define BLE_RETRY_INITIAL_MS 2000
#define BLE_RETRY_MAX_MS    30000
```

Backoff:

```text
2s
4s
8s
16s
30s
30s
...
```

Có thể thêm jitter sau khi core behavior ổn định.

## 22.3. Round-robin

```c
static int s_scheduler_cursor;
```

Mỗi vòng bắt đầu từ cursor hiện tại.

Chọn một device đủ điều kiện:

```text
in_use
reconnect_enabled
state == OFFLINE hoặc BACKOFF
connection_slot == -1
now >= next_retry_ms
has_peer_addr
```

Sau khi chọn:

```text
cursor = selected + 1
```

---

# 23. Chỉ một connect procedure

Policy giai đoạn đầu:

```text
max pending CONNECTING = 1
```

Check phải nằm trong `ble_state_reserve_connection()` cùng transaction với allocation.

Không dùng pattern:

```c
if (!ble_conn_has_connecting()) {
    ble_conn_allocate(...);
}
```

vì có TOCTOU race.

Sau khi ổn định có thể benchmark lại.

---

# 24. Supervisor loop

Pseudo:

```c
static void reconnect_supervisor_task(void *arg)
{
    while (ble_supervisor_is_running()) {
        int64_t now_ms = ble_now_ms();

        ble_supervisor_check_timeouts(now_ms);

        if (!ble_host_is_ready()) {
            goto sleep;
        }

        if (ble_gap_disc_active()) {
            goto sleep;
        }

        int device_index =
            ble_scheduler_next_device(now_ms);

        if (device_index >= 0) {
            int rc = ble_connection_start(device_index);

            if (rc != BLE_CENTRAL_OK &&
                rc != BLE_CENTRAL_ERR_CONNECT_IN_PROGRESS &&
                rc != BLE_CENTRAL_ERR_NO_SLOT) {
                ble_scheduler_note_immediate_failure(
                    device_index,
                    now_ms);
            }
        }

sleep:
        vTaskDelay(pdMS_TO_TICKS(BLE_SUPERVISOR_TICK_MS));
    }

    ble_supervisor_mark_stopped();
    vTaskDelete(NULL);
}
```

---

# 25. Supervisor lifecycle

```c
typedef enum {
    BLE_SUPERVISOR_STOPPED = 0,
    BLE_SUPERVISOR_RUNNING,
    BLE_SUPERVISOR_STOPPING,
} ble_supervisor_state_t;
```

State transition:

```text
STOPPED
   │ start
   ▼
RUNNING
   │ stop
   ▼
STOPPING
   │ task exit
   ▼
STOPPED
```

`start()` chỉ create task khi `STOPPED`.

Không set `RUNNING` lại khi task cũ còn `STOPPING`.

---

# 26. Notification pipeline

NimBLE callback không decode CBOR và không gọi dispatcher.

## 26.1. Event

```c
typedef struct {
    char device_id[GW_MSG_DEVICE_ID_LEN];

    uint8_t cbor_buf[GW_MSG_MAX_LEN];
    uint16_t cbor_len;
} ble_notify_event_t;
```

## 26.2. NimBLE callback

Chỉ:

```text
validate connection
validate attr handle
validate packet length
copy mbuf
enqueue with timeout = 0
return
```

Pseudo:

```c
if (xQueueSend(
        s_notify_queue,
        &notify_event,
        0) != pdTRUE) {
    ble_metrics_notify_dropped();
}
```

Không block NimBLE host.

## 26.3. Worker

```text
queue receive
   ↓
CBOR decode
   ↓
application notify callback
```

---

# 27. Queue overload policy

Nếu queue full:

```text
drop newest event
increment notify_dropped
log rate-limited warning
```

Không block.

Metrics nên có:

```c
uint32_t notify_received;
uint32_t notify_enqueued;
uint32_t notify_dropped;
uint32_t notify_decode_errors;
uint32_t notify_queue_high_watermark;
```

ACK có thể bị drop khi overload; dispatcher sẽ timeout. Đây là failure mode chấp nhận được hơn block BLE host.

---

# 28. MTU-safe send

## 28.1. Default

```c
slot->mtu = 23;
```

## 28.2. MTU event

Trong `BLE_GAP_EVENT_MTU`:

```text
validate ref/conn_handle
update slot.mtu
```

## 28.3. Send

Lấy snapshot:

```c
typedef struct {
    uint16_t conn_handle;
    uint16_t command_val_handle;
    uint16_t mtu;
    ble_conn_state_t state;
} ble_send_snapshot_t;
```

Sau unlock:

```c
int length =
    cbor_codec_encode(msg, buffer, sizeof(buffer));
```

Validate:

```c
uint16_t max_payload =
    snapshot.mtu > 3 ? snapshot.mtu - 3 : 0;

if (length > max_payload) {
    return BLE_CENTRAL_ERR_MESSAGE_TOO_LARGE;
}
```

## 28.4. Lưu ý

`READY` không đảm bảo MTU exchange đã hoàn tất.

Nếu NimBLE version hiện tại có API query negotiated MTU đáng tin cậy, có thể query tại thời điểm send và dùng cache làm fallback.

---

# 29. GATT discovery hardening

COMMAND characteristic phải có:

```text
WRITE_NO_RSP
```

STATUS characteristic phải có:

```text
NOTIFY
```

Nếu UUID đúng nhưng properties sai:

```text
abort discovery
terminate connection
backoff
```

Không đợi write/subscribe fail muộn.

---

# 30. Timeout tách riêng

```c
#define BLE_SECURITY_TIMEOUT_MS        10000
#define BLE_GATT_DISCOVERY_TIMEOUT_MS  10000
```

Connection slot nên có timestamp phù hợp:

```c
started_ms;
discovery_started_ms;
```

Supervisor log rõ:

```text
security timeout
GATT discovery timeout
```

---

# 31. Error code contract

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
    BLE_CENTRAL_ERR_NO_RESOURCE = -11,
    BLE_CENTRAL_ERR_STATE = -12,
} ble_central_err_t;
```

Public API có thể tiếp tục trả `int`, nhưng dùng enum này internally/public header.

---

# 32. Concurrency context matrix

| Context | Trách nhiệm | Có thể block? |
|---|---|---|
| NimBLE Host | GAP/GATT callback, state update ngắn, enqueue notify | Không |
| Supervisor Task | timeout + scheduler + request connect | Ngắn |
| HTTP/App Task | public BLE API | Có giới hạn |
| Notify Worker | CBOR decode + dispatcher callback | Có |
| Device Store | persistent/runtime mirror | Có mutex riêng |

Rule:

```text
NimBLE Host không gọi application callback
NimBLE Host không wait ACK
State mutex không bao quanh NimBLE API
State mutex không bao quanh device_store API
State mutex không bao quanh app callback
```

---

# 33. Internal API đề xuất

## State

```c
bool ble_state_lock(void);
void ble_state_unlock(void);

int ble_state_reserve_connection(
    int device_index,
    ble_conn_ref_t *out_ref);

void ble_state_rollback_connection_start(
    ble_conn_ref_t ref,
    int64_t now_ms);

bool ble_conn_snapshot(
    ble_conn_ref_t ref,
    ble_conn_slot_t *out);

int ble_conn_find_by_handle(
    uint16_t conn_handle,
    ble_conn_ref_t *out_ref);

bool ble_state_on_connect_success(
    ble_conn_ref_t ref,
    uint16_t conn_handle);

bool ble_state_on_disconnect(
    ble_conn_event_ref_t event_ref,
    int64_t now_ms,
    int *out_device_index,
    bool *out_removing);
```

## Runtime

```c
int ble_runtime_init(void);

int ble_runtime_find(const char *device_id);

int ble_runtime_find_or_register(
    const char *device_id,
    const ble_addr_t *addr);

bool ble_runtime_snapshot(
    int device_index,
    ble_device_runtime_t *out);

bool ble_runtime_get_peer_addr(
    int device_index,
    ble_addr_t *out);

void ble_runtime_finalize_remove(
    int device_index);
```

---

# 34. Testability abstraction

Để scheduler/unit test deterministic, không gọi platform trực tiếp ở logic core.

## Time

```c
int64_t ble_now_ms(void);
```

Production:

```c
return esp_timer_get_time() / 1000;
```

Test:

```text
fake_now_ms
```

## Stack wrapper

Có thể thêm wrapper tối thiểu:

```c
int ble_stack_connect(...);
int ble_stack_cancel_connect(void);
int ble_stack_terminate(...);
```

Production gọi NimBLE.

Test dùng fake/mock.

Không cần wrapper toàn bộ NimBLE ngay từ đầu.

---

# 35. Logging chuẩn

Format:

```text
[device-id][slot=2][gen=5][handle=7] CONNECTING
[device-id][slot=2][gen=5][handle=7] SECURING
[device-id][slot=2][gen=5][handle=7] DISCOVERING
[device-id][slot=2][gen=5][handle=7] READY
[device-id][slot=2][gen=5][handle=7] DISCONNECTED reason=...
```

Generation trong log rất hữu ích khi debug stale callback.

---

# 36. Metrics

```c
typedef struct {
    uint32_t connect_attempts;
    uint32_t connect_success;
    uint32_t connect_failures;

    uint32_t disconnects;

    uint32_t security_failures;
    uint32_t discovery_failures;

    uint32_t reconnect_attempts;

    uint32_t host_resets;

    uint32_t stale_callbacks;

    uint32_t notify_received;
    uint32_t notify_enqueued;
    uint32_t notify_dropped;
    uint32_t notify_decode_errors;

    uint32_t mtu_rejects;
} ble_central_metrics_t;
```

---

# 37. Cấu trúc thư mục sau refactor

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
├── ble_central_notify.c
│
├── ble_central_supervisor.c
│
├── CMakeLists.txt
│
└── test/
    ├── test_ble_state.c
    ├── test_ble_runtime.c
    ├── test_ble_scheduler.c
    ├── test_ble_removal.c
    └── test_ble_host_reset.c
```

---

# 38. Migration plan — mỗi commit phải build

Không xóa field legacy quá sớm.

## Commit 1 — Add runtime registry

Thêm:

```text
ble_central_runtime.c
ble_device_runtime_t
```

Không thay behavior hiện tại.

Acceptance:

```text
build OK
runtime init từ device_store OK
```

## Commit 2 — Add state mutex + new connection reference helpers

Thêm:

```text
s_state_mutex
generation
ble_conn_ref_t
snapshot helpers
```

Tạm thời vẫn giữ legacy fields trong `ble_conn_slot_t`.

Không đổi GAP/GATT callback contract.

## Commit 3 — Add transactional mapping

Thêm:

```text
ble_state_reserve_connection()
ble_state_rollback_connection_start()
```

Migrate `ble_central_connect()` sang orchestration mới.

Legacy slot fields vẫn còn để GAP/GATT compile.

## Commit 4 — Migrate GAP connect/disconnect

GAP dùng:

```text
device_index
conn_ref
runtime mapping
FREE-on-disconnect
```

Thêm `REMOVING`.

## Commit 5 — Migrate GATT callbacks

Không đọc raw slot pointer ngoài lock.

Dùng snapshot/ref validation.

Bắt đầu dùng generation chống stale callback.

## Commit 6 — Remove legacy connection fields + remove `IDLE`

Sau khi không còn code dùng:

```text
device_id
peer_addr
forget_requested
last_attempt_ms
BLE_CONN_SLOT_IDLE
```

mới xóa chúng.

## Commit 7 — Reconnect scheduler

Thêm:

```text
round-robin
backoff
connect transaction guard
```

Scheduler chỉ dùng runtime source of truth.

## Commit 8 — Host reset recovery

Migrate `on_ble_host_reset()` sang state/runtime model mới.

## Commit 9 — MTU-safe send + error codes

Lưu/query MTU.

Reject payload quá lớn trước GATT write.

## Commit 10 — Notification queue

Thêm worker.

Không CBOR decode/application callback trong NimBLE host.

## Commit 11 — Forget/bond lifecycle

Hoàn thiện:

```text
REMOVING
cancel/terminate
bond delete
runtime finalize
```

Sửa command dispatcher delete ordering.

## Commit 12 — Supervisor lifecycle

Thêm:

```text
STOPPED/RUNNING/STOPPING
```

Loại duplicate task race.

## Commit 13 — GATT hardening + logging + metrics

Validate properties.

Tách timeout.

Thêm metrics.

## Commit 14 — Tests

Hoàn thiện unit/integration test suite.

---

# 39. Unit tests bắt buộc

## 39.1. Connection pool

- allocate tất cả slots;
- slot thứ N+1 trả `NO_SLOT`;
- release rồi reuse được;
- generation tăng qua mỗi lifetime;
- release không reset generation;
- stale `ble_conn_ref_t` bị reject.

## 39.2. Atomic mapping

- reserve tạo mapping hai chiều nhất quán;
- rollback xóa mapping hai chiều;
- hai connect request đồng thời không tạo hai `CONNECTING`;
- device không thể giữ hai slot.

## 39.3. Runtime pool

- `DEVICE_STORE_MAX_DEVICES` entries;
- remove không compact array;
- device_index của device khác không đổi;
- entry `REMOVING` không được reuse.

## 39.4. 16 devices / 9 connections

- 16 runtime entries;
- tối đa 9 connection slots;
- 7 offline devices không chiếm connection slot.

## 39.5. Scheduler

- round-robin fairness;
- backoff 2/4/8/16/30;
- success reset retry count;
- device `REMOVING` không reconnect;
- device chưa tới `next_retry_ms` không được chọn.

## 39.6. Disconnect

- READY → FREE;
- runtime `connection_slot = -1`;
- non-removing device → BACKOFF;
- removing device → finalize path.

## 39.7. Forget

- forget offline device;
- forget CONNECTING device;
- forget CONNECTED device;
- BLE address vẫn có sẵn sau khi caller xóa device_store;
- bond delete chạy đúng một lần.

## 39.8. Stale callback

- callback generation cũ không mutate slot mới;
- callback handle cũ không mutate connection mới;
- stale callback metric tăng.

## 39.9. Host reset

- tất cả connection slots về FREE;
- tất cả mappings bị clear;
- connected mirror về 0;
- runtime có thể reconnect sau host sync.

## 39.10. Notification

- raw payload được enqueue;
- CBOR decode chỉ chạy ở worker;
- queue overflow không block;
- drop metric tăng;
- worker callback chạy đúng device_id.

## 39.11. MTU

```text
MTU 23:
20 bytes → OK
21 bytes → MESSAGE_TOO_LARGE

MTU 256:
253 bytes → OK
254 bytes → MESSAGE_TOO_LARGE
```

## 39.12. Supervisor

- start khi STOPPED;
- start lần hai khi RUNNING không tạo task mới;
- stop → STOPPING;
- không start task mới trước khi task cũ STOPPED.

---

# 40. Integration tests trên ESP32

| Scenario | Expected |
|---|---|
| 1 peripheral | Connect → Secure → Discover → Ready |
| Peripheral power off | Disconnect → FREE slot → BACKOFF |
| Power on lại | Auto reconnect |
| 3 peripherals mất điện | Reconnect round-robin |
| Device A fail liên tục | B/C vẫn reconnect được |
| Scan đang active | Supervisor không connect |
| Delete online device | REMOVING → terminate → bond delete |
| Delete connecting device | cancel → callback → finalize |
| Delete offline device | bond delete + runtime remove |
| Host reset | clear mapping + recover after sync |
| Notification burst | không watchdog NimBLE host |
| Queue overload | drop có metric, không block |
| Payload > MTU | reject trước GATT write |
| 16 registered / 9 max conn | không fake slot exhaustion |
| Stop/start supervisor nhanh | không duplicate task |

---

# 41. Acceptance criteria cuối cùng

Refactor chỉ được xem là hoàn thành khi:

1. Offline device không giữ BLE connection slot.
2. `DEVICE_STORE_MAX_DEVICES` có thể lớn hơn `CONFIG_BT_NIMBLE_MAX_CONNECTIONS`.
3. Device runtime và connection pool có mapping atomically.
4. Không tồn tại TOCTOU giữa `has_connecting` và allocation.
5. Chỉ một connect procedure được khởi tạo tại một thời điểm ở policy hiện tại.
6. Disconnect luôn trả connection slot về `FREE`.
7. Runtime array không compact và `device_index` ổn định.
8. `REMOVING` ngăn runtime entry bị reuse trước khi async disconnect/cancel hoàn tất.
9. Stale callback không mutate connection lifetime mới.
10. Host reset clear toàn bộ stale mapping.
11. Scheduler chỉ đọc BLE runtime, không dùng `device_store.connected` làm source of truth.
12. NimBLE notify callback không decode CBOR và không gọi dispatcher.
13. NimBLE host không block trên application mutex/ACK.
14. MTU được kiểm tra trước GATT write.
15. Stop/start supervisor không tạo duplicate task.
16. Delete offline/online/connecting device vẫn xóa được BLE bond đúng lifecycle.
17. State mutex không bao quanh NimBLE API.
18. State mutex không bao quanh `device_store`.
19. State mutex không bao quanh application callback.
20. Public facade vẫn tương thích với command dispatcher sau migration.
21. Mỗi migration commit build được.
22. Có unit tests cho state, runtime, scheduler, removal, stale callback và host reset.

---

# 42. Thứ tự ưu tiên triển khai

Ưu tiên P0:

```text
1. state mutex + atomic mapping
2. runtime registry
3. real connection pool
4. GAP disconnect lifecycle
5. REMOVING lifecycle
6. host reset recovery
```

Ưu tiên P1:

```text
7. reconnect scheduler
8. stale callback hardening
9. MTU-safe send
10. notification queue
11. supervisor lifecycle
```

Ưu tiên P2:

```text
12. GATT property validation
13. metrics/logging
14. benchmark multi-connect policy
```

---

# 43. Kết luận

Kiến trúc refactor cuối cùng phải giữ ranh giới sau:

```text
Persistent identity/config
        ↓
Device Store

BLE lifecycle/retry
        ↓
Device Runtime

Active link resources
        ↓
Connection Pool

BLE protocol callbacks
        ↓
GAP/GATT

Application delivery
        ↓
Notify Queue/Worker
```

Hai invariant quan trọng nhất:

```text
Device runtime có thể tồn tại mà không có connection slot.

Connection slot không được tồn tại sau khi BLE connection lifetime kết thúc.
```

Và hai rule concurrency quan trọng nhất:

```text
Mapping device ↔ slot phải được cập nhật trong một state transaction.

Callback chỉ được mutate state sau khi xác thực đúng generation/lifetime.
```

Nếu triển khai đúng các invariant này, component sẽ tránh được các lỗi chính của thiết kế hiện tại: slot exhaustion giả, reconnect starvation, stale callback, delete race, duplicate connect procedure và block NimBLE host task.
