# Command Dispatcher

## 1. Tổng quan

`command_dispatcher` là **routing layer** trung tâm giữa các transport (Web UI, MCP endpoint) và hai domain xử lý:

```text
Web UI / MCP
      │
      ▼
 gw_message_t
      │
      ▼
command_dispatcher_handle()
      │
      ├── gateway_command ──► command registry ──► handler chạy trên ESP32
      │
      └── device_command  ──► device_request_manager ──► BLE Central
                                                       ──► peripheral
                                                             │
                                                        notification
                                                             │
                                              command_dispatcher_on_device_notify()
```

Nguyên tắc thiết kế (Phase 1):

* Mọi command đi qua một dispatcher chung; transport không chứa business logic.
* `status` của result là single source of truth; dispatcher không phụ thuộc HTTP/MCP.
* ACK correlation bằng `request_id` — không match ACK theo "command rỗng".
* Không giữ mutex khi chờ BLE I/O; không dynamic allocation trong hot path.

---

## 2. Các file chính

```text
components/command_dispatcher/
├── command_dispatcher.c          # Entry point, validation boundary, routing
├── command_registry.c            # Registry gateway command (freeze sau init)
├── gateway_commands.c            # add/delete/edit/list_devices, get_status
├── device_command.c              # Gửi BLE command, chờ ACK, sinh wire copy
├── device_request_manager.c/.h   # Pending request table + ACK correlation
├── command_dispatcher_internal.h # API nội bộ + test hooks
├── include/command_dispatcher.h  # Public API
└── test/                         # Unity tests (29 cases dispatcher + request manager)
```

---

## 3. Protocol message (`cbor_codec.h`)

Dispatcher dùng `gw_message_t` của component `cbor_codec`, **protocol version 3**
(vẫn nhận v1/v2):

```c
typedef struct {
    uint8_t protocol_version;
    char type[GW_MSG_TYPE_LEN];
    char device_id[GW_MSG_DEVICE_ID_LEN];
    char command[GW_MSG_COMMAND_LEN];
    uint32_t request_id;        // correlation ID Gateway ↔ peripheral
    int has_request_id;
    int int_value;
    int bool_value;
    int has_device_id;
    char name[GW_MSG_NAME_LEN];
    uint8_t ble_addr[6];
    uint8_t ble_addr_type;
    int has_ble_addr;
} gw_message_t;
```

Message type trên wire:

| Type | Ý nghĩa | Ghi chú |
|---|---|---|
| `gateway_command` | Điều khiển gateway | Xử lý qua registry |
| `device_command` | Lệnh xuống BLE device | Dispatcher tự gán `request_id` |
| `device_ack` | Peripheral trả lời | Phải echo đúng `request_id` |
| `device_event` | Telemetry/notification | Không bao giờ complete pending command |

> ⚠️ Peripheral firmware phải hỗ trợ protocol v4 (echo `request_id`). Peripheral v1/v2 sẽ bị log `[ACK_UNMATCHED]`.

Protocol v4 bổ sung device schema discovery. Khi snapshot đã biết, device command
được kiểm tra qua `device_schema` trước khi cấp pending request; command
không quảng bá hoặc argument sai type/range bị từ chối trước BLE write.

---

## 4. Public API

```c
int  command_dispatcher_init(void);                    // single-shot
int  command_dispatcher_register(const char *name, gateway_command_fn_t fn);
int  command_dispatcher_freeze_registry(void);
int  command_dispatcher_get_registered_names(char out[][GW_MSG_COMMAND_LEN], int max);
bool command_dispatcher_is_registered(const char *name);

void command_dispatcher_handle(const gw_message_t *msg, dispatch_result_t *result);
void command_dispatcher_on_device_notify(const char *device_id, const gw_message_t *msg);

void command_dispatcher_set_text_result(dispatch_result_t*, dispatch_status_t, const char *fmt, ...);
void command_dispatcher_set_json_result(dispatch_result_t*, dispatch_status_t, const char *json);
```

Hằng số: `DISPATCHER_MAX_RESULT_LEN 4096`, `DISPATCHER_MAX_COMMANDS 16`, `DISPATCHER_ACK_TIMEOUT_MS 2000`.

### Boundary validation (§15 refactor plan)

* `handle()`: msg NULL, type rỗng, sai `protocol_version`, thiếu `device_id`/`command` theo loại → `DISPATCH_STATUS_INVALID_ARGUMENT`.
* `on_device_notify()`: chỉ chấp nhận `device_ack`/`device_event`; ACK malformed bị drop kèm log.

---

## 5. Registry lifecycle

```text
command_dispatcher_init()          // đăng ký defaults; gọi lần 2 → ESP_ERR_INVALID_STATE
        │
register custom commands           // fail nếu đã freeze, trùng tên, quá 16
        │
command_dispatcher_freeze_registry()
        │
handle() hoạt động                 // trước freeze → INTERNAL_ERROR
```

Sau freeze: register trả lỗi, registry coi như immutable → pointer lifetime rõ, không race. `get_registered_names()` là **copy-out API** — caller sở hữu bộ nhớ.

---

## 6. Result contract

```c
typedef enum { DISPATCH_RESULT_TEXT = 0, DISPATCH_RESULT_JSON } dispatch_result_format_t;

typedef enum {
    DISPATCH_STATUS_OK = 0,
    DISPATCH_STATUS_INVALID_ARGUMENT,
    DISPATCH_STATUS_NOT_FOUND,
    DISPATCH_STATUS_BUSY,
    DISPATCH_STATUS_TIMEOUT,
    DISPATCH_STATUS_NOT_CONNECTED,
    DISPATCH_STATUS_TRANSPORT_ERROR,
    DISPATCH_STATUS_INTERNAL_ERROR,
    DISPATCH_STATUS_DEVICE_ERROR,   // peripheral nhận lệnh nhưng báo thất bại
} dispatch_status_t;

typedef struct {
    dispatch_status_t status;
    dispatch_result_format_t format;
    char payload[DISPATCHER_MAX_RESULT_LEN];
} dispatch_result_t;
```

Không có field `success` riêng. Boolean helper: `dispatch_result_is_ok(&result)`.

Map HTTP deterministic ở transport layer:

```text
OK → 200 | INVALID_ARGUMENT → 400 | NOT_FOUND → 404 | BUSY → 409
NOT_CONNECTED / TRANSPORT_ERROR / DEVICE_ERROR → 502 | TIMEOUT → 504 | INTERNAL_ERROR → 500
```

---

## 7. Device command & ACK correlation

Luồng gửi:

1. Check connected → không thì `NOT_CONNECTED`.
2. `device_request_allocate()` — nếu device đang có pending → `BUSY` (invariant Phase 1: **1 pending/device**, max = `DEVICE_STORE_MAX_DEVICES`).
3. Tạo **bản sao wire** của message, gán `request_id` mới (monotonic, ≠ 0, không reuse khi pending). Message của caller không bị sửa.
4. Send qua BLE Central. Fail → release slot, `TRANSPORT_ERROR`.
5. Chờ semaphore tối đa 2000 ms → hết giờ `[CMD_TIMEOUT]`, `TIMEOUT`, slot reusable.

ACK chỉ complete request khi **toàn bộ** khớp:

```text
type == "device_ack"
AND device_id matches       AND request_id matches   (primary key)
AND command matches         (validation bổ sung, lệch → [ACK_PROTOCOL_ERROR])
```

Đặc điểm đảm bảo correctness:

* `device_event` không bao giờ đánh thức waiter → telemetry không bị nhầm thành ACK.
* Stale ACK (của request đã timeout) không thể complete request mới.
* ACK lạ/malformed → `[ACK_UNMATCHED]`, bỏ qua an toàn.
* Giá trị ACK: `bool_value=true` → OK; `false` → `DEVICE_ERROR` ("Device rejected").

Kết quả text dạng: `Device <id> acknowledged/rejected '<command>'`.

---

## 8. Gateway commands mặc định

| Command | Status | Format | Ghi chú |
|---|---|---|---|
| `add_device` | OK nếu persist được | JSON `{device_id, persisted, connect_requested}` | BLE connect là **best-effort side effect** |
| `delete_device` | OK | Text | Thứ tự: snapshot store → `ble_central_forget_peer(addr...)` → `store_delete`. Forget fail → giữ entry để retry (`TRANSPORT_ERROR`) |
| `edit_device` | OK / NOT_FOUND | Text | Cần `name` |
| `list_devices` | OK | JSON array | Payload quá lớn → INTERNAL_ERROR |
| `get_status` | OK | JSON | device_count / connected_count / ble_link_count |

Lifecycle delete không để orphan bond: peer identity được snapshot **trước khi** xóa store entry, BLE layer không quay lại lookup store; lỗi xóa bond thực tế được propagate lên caller.

---

## 9. Concurrency model

* Dispatcher giữ **synchronous**: caller block tối đa ~2 s cho device command (Web `/api/command` đã có worker task riêng; benchmark worker queue là việc của Phase E).
* Mutex: `s_registry_mutex` (registry), `s_request_mutex` (request manager). Chỉ giữ trong thao tác ngắn — **không giữ khi chờ ACK** (waiter take semaphore trực tiếp).
* Nhiều FreeRTOS task gọi `handle()` đồng thời là an toàn.

---

## 10. Khởi tạo trong app_main

```c
device_store_init();
command_dispatcher_init();
command_dispatcher_freeze_registry();   // BẮT BUỘC trước khi nhận request
ble_central_init(on_device_notify);     // notify callback → command_dispatcher_on_device_notify
...
```

Provisioning mode không init dispatcher — mọi call vào `handle()` lúc đó trả `INTERNAL_ERROR` một cách an toàn.

---

## 11. Logging

```text
[CMD_SEND]            device= relay-1 request_id=1042 command=set_power
[CMD_ACK]             ... result=ok|rejected
[CMD_TIMEOUT]         ... timeout_ms=2000
[ACK_UNMATCHED]       ACK không khớp pending nào
[ACK_PROTOCOL_ERROR]  request_id khớp nhưng command lệch
[DEVICE_EVENT]        event từ peripheral (không complete command)
[DEVICE_DELETE_FAILED] lỗi lifecycle delete/bond
```

---

## 12. Tests

Unity test trên hardware thật (`cd test && idf.py flash monitor`):

* Routing + boundary validation (null msg, sai version, sai type, thiếu field).
* Registry: init-once, duplicate, over-capacity, freeze, copy-out names.
* ACK correlation case 1–10 của refactor plan: matching ACK, wrong id/device/command, device_event, stale ACK, BUSY, send-fail, timeout + slot reusable.
* BLE layer được mock qua `device_command_set_hooks()` — không cần radio để test correlation logic.

Hiện tại: **29/29 test của component PASS** (toàn bộ test app: 52/52) trên ESP32-S3.

---

## 13. Ngoài scope Phase 1

Nhiều command concurrent trên cùng peripheral, async dispatcher queue, dynamic unregister, multi-gateway — xem `command_dispatcher_refactor.md` §21.
