# GCF-05 — Settings Single-Owner Actor + Logical Lease
> Bộ tài liệu này được tách từ `GATEWAY_CONTROL_FLOW_REFACTOR_PLAN_v1.1.md`.
> Mỗi file phase là tài liệu triển khai độc lập và là nguồn checklist của phase tương ứng.
> Khi phase hoàn thành, đánh `[x]` toàn bộ checklist của phase và thêm `✅ DONE (YYYY-MM-DD)` vào heading phase theo rule của `AGENTS.md`.

← Trước: `04_GCF-04_SEQUENTIAL_STATE_SEED.md` | Tiếp: `06_GCF-06_WEB_MCP_MIGRATION.md` →

## 0. Phase gate

- **Phase:** `GCF-05`
- **Phụ thuộc:** GCF-04 phải DONE
- **Trạng thái ban đầu:** `[ ] NOT STARTED`
- **Quy tắc hoàn thành:** chỉ chuyển phase tiếp theo khi toàn bộ checklist + test bắt buộc + acceptance của file này PASS.


## 1. Vấn đề cần giải quyết

#### 2.3 Settings mutable state bị truy cập từ nhiều execution context

`device_settings_worker.c` có active operation state nhưng completion callback từ DCS có thể thay đổi state này ngoài worker context.

Trên ESP32-S3 dual-core, đây là race condition tiềm ẩn.

#### 2.4 Listener order không phải scheduler guarantee

Đăng ký Settings listener trước State listener không đảm bảo Settings command đã lấy command slot trước State seed.

Ordering phải do scheduler enforce, không dựa vào task scheduling timing.

#### 2.8 ACK transport không đồng nghĩa logical Settings complete

`read_settings` có thể ACK trước khi `settings_values_end` tới. Nếu scheduler mark device idle ngay sau DCS completion, Web/MCP/State có thể chen vào giữa Settings stream. Cần logical device lease/reservation.

#### 2.9 Scheduler public API không được coupling business modules vào DCS

Nếu scheduler public header expose type chỉ nằm trong `device_command_service.h`, business modules vẫn phụ thuộc transport layer về compile-time. Shared request/result/status types phải được tách thành component riêng.

#### 2.10 Schema migration có nguy cơ dependency cycle

DCS hiện dùng `device_schema_validate_command()`. Nếu `device_schema` lại `REQUIRES device_control_scheduler`, graph trở thành:

```text
device_schema -> scheduler -> DCS -> device_schema
```

Schema phải giữ submitter abstraction và scheduler bridge nằm ở adapter component độc lập.

#### 2.11 Cancel/context lifetime/deadline chưa có contract tổng quát

Sau khi thêm queue trung gian, cần định nghĩa rõ:

- synchronous reject có gọi callback hay không;
- accepted job bị cancel/dedupe/delete phải terminal thế nào;
- ai sở hữu `void *context`;
- cancel một seed revision có được ảnh hưởng Web/Settings hay không;
- job chờ scheduler bao lâu trước khi trở thành stale.


### 1.1 Baseline hiện tại và tiêu chí không được hiểu sai

Ở baseline hiện tại, `device_settings_worker.c` vẫn dùng `EventGroup`, callback DCS vẫn mutate `s_active_op`, `device_settings_worker_on_disconnect()`/`device_settings_worker_on_values_complete()` vẫn sửa state trực tiếp, và `device_settings_transaction.c` vẫn có state machine + callback riêng gọi DCS trực tiếp. Vì vậy **chỉ thêm enum/event helper hoặc lease API không làm GCF-05 hoàn thành**.

GCF-05 chỉ được coi là đạt khi runtime thực tế có wiring sau:

```text
producer context
  (scheduler completion / lease callback / timer / BLE parser / lifecycle / API)
        │
        ├─ build bounded event payload
        ├─ ds_actor_post_event()
        ▼
Settings actor queue
        ▼
device_settings_actor_task()
        ▼
switch (event.type)
        ▼
corresponding ds_handle_*()
        ▼
mutate operation / transaction / retry / lease state
```

Các trường hợp sau **không đạt** dù code compile:

- có `device_settings_event_t` nhưng callback vẫn ghi `s_active_op`;
- có `ds_actor_post_event()` nhưng `worker_task()` vẫn chủ yếu chạy bằng EventGroup bit cũ;
- có API acquire/release lease nhưng command Settings không mang `lease_id` thật;
- transaction vẫn advance trong `device_settings_transaction.c::on_cmd_complete()` thay vì actor handler;
- disconnect/forget vẫn cleanup mutable runtime trực tiếp ngoài actor;
- stale lease grant chỉ bị ignore mà không release;
- actor queue chỉ dùng như notification phụ, còn state transition chính vẫn ở callback/timer context.

**Rule:** nếu một transition có thể thay đổi logical Settings state, transition đó phải xảy ra trong actor task, ngoại trừ copy/commit immutable staging data theo contract của `device_settings_memory.c`.

---


## 2. Thiết kế, file và code contract cần triển khai

##### INV-02: Logical device lease tách biệt transport inflight

Một ACK của DCS chỉ kết thúc **transport command**, không nhất thiết kết thúc logical operation.

Ví dụ `read_settings`:

```text
READ_SETTINGS
    ↓
ACK                 <- transport command complete
    ↓
settings values stream
    ↓
settings_values_end <- logical operation complete
```

Vì vậy scheduler phải hỗ trợ logical reservation/lease:

```text
lease(device_id) = owner
```

Khi lease đang active:

- chỉ job có `lease_id` tương ứng được dispatch trên device đó;
- job Web/MCP/State/Schema khác vẫn queue nhưng không chen vào;
- lease được release bởi actor owner khi logical operation hoàn tất/cancel.

Settings describe/read, transaction/reconcile là consumer chính của lease trong refactor này.

##### INV-03: Global bounded transport inflight

Mặc định:

```text
DEVICE_CTRL_MAX_INFLIGHT = 4
```

Giá trị này độc lập số BLE links nhưng phải compile-time assert:

```c
_Static_assert(DEVICE_CTRL_MAX_INFLIGHT <= DEVICE_COMMAND_SERVICE_MAX_PENDING,
               "scheduler inflight exceeds DCS pending capacity");
```

Không tăng `DEVICE_COMMAND_SERVICE_MAX_PENDING` chỉ để che orchestration bug.

##### INV-04: ACK path không dùng normal submit queue

ACK/lifecycle/cancel/shutdown phải có urgent path riêng, compact event storage và wake DCS worker ngay khi event đến.

##### INV-05: Mutable state machine có một owner

Ví dụ Settings:

```text
API / BLE / scheduler / timer
        │
        ▼
 settings event queue
        │
        ▼
 Settings Worker
        │
        └── sole owner của operation/transaction/reconcile runtime
```

Settings memory snapshot có thể có lock riêng; "single owner" áp dụng cho **state machine mutation**, không bắt buộc mọi immutable snapshot read phải chạy trong worker.

##### INV-06: Scheduler không sở hữu protocol logic

Scheduler không encode CBOR, không validate feature schema, không hiểu settings payload semantics.

Scheduler chỉ biết:

- device id;
- priority;
- shared command request type;
- source;
- job id;
- owner token;
- optional lease id;
- queue deadline;
- dedupe metadata;
- completion destination.

#### 5.5 Device Settings

ADD:

```text
components/device_settings/device_settings_internal.h
components/device_settings/device_settings_events.c
components/device_settings/test/test_device_settings_actor.c
```

EDIT:

```text
components/device_settings/device_settings.c
components/device_settings/device_settings_worker.c
components/device_settings/device_settings_operation.c
components/device_settings/device_settings_transaction.c
components/device_settings/device_settings_memory.c
components/device_settings/include/device_settings.h
components/device_settings/CMakeLists.txt
```

## 10. Settings actor refactor

### 10.1 Owner rule

Chỉ `device_settings_worker` được mutate:

- active operation;
- operation slot state/generation;
- retry counters;
- ACK/values-stream flags;
- transaction runtime;
- reconciliation progression;
- active scheduler lease id.

`device_settings_memory.c` sở hữu immutable committed/staging storage và lock riêng; protocol decoder chỉ parse/copy bounded data.

#### 10.2 Actor event transport — bắt buộc là payload queue

Không dùng `EventGroup` để thay thế event transport cho các sự kiện có payload. `EventGroup` chỉ biểu diễn bit trạng thái/wakeup và có thể coalesce nhiều completion thành một bit, trong khi Settings cần giữ chính xác `generation`, `owner_token`, `job_id`, kết quả command và loại transition.

Bắt buộc dùng một queue bounded riêng của Settings actor:

```c
#define DS_ACTOR_EVENT_QUEUE_LEN 24

typedef enum {
    DS_EVENT_START_OPERATION = 0,
    DS_EVENT_LEASE_RESULT,
    DS_EVENT_COMMAND_COMPLETE,
    DS_EVENT_VALUES_STREAM_COMPLETE,
    DS_EVENT_DISCONNECTED,
    DS_EVENT_RETRY_TIMER,
    DS_EVENT_CANCEL_DEVICE,
    DS_EVENT_TRANSACTION_START,
    DS_EVENT_TRANSACTION_ABORT,
    DS_EVENT_FORGET_DEVICE,
    DS_EVENT_SHUTDOWN,
} device_settings_event_type_t;
```

Không cần `DS_EVENT_TRANSACTION_COMMAND_COMPLETE` riêng nếu command transaction dùng cùng `DS_EVENT_COMMAND_COMPLETE`; phase/kind trong actor state quyết định command đó là DESCRIBE/READ/TX_BEGIN/TX_SET/TX_COMMIT/CONFIRM.

Event payload phải bounded, không giữ pointer tới stack/cJSON/HTTP object:

```c
typedef struct {
    device_settings_event_type_t type;
    device_id_t device_id;
    uint16_t slot;
    uint32_t generation;
    device_control_owner_token_t owner_token;

    union {
        struct {
            esp_err_t status;
            device_control_lease_t lease_id;
        } lease;

        struct {
            device_control_job_id_t job_id;
            device_control_result_t result;
        } command;

        struct {
            bool success;
        } values;
    } data;
} device_settings_event_t;
```

Nếu `device_control_result_t` làm event quá lớn, chỉ copy compact fields Settings thực sự dùng: terminal type, command status, accepted, scheduler job id. Không copy full wire message.

**Ownership:** producer chỉ tạo/copy event rồi enqueue; sau khi enqueue thành công, Settings worker là sole owner của event payload và mọi mutable Settings runtime.

#### 10.3 Producer API và rule không mutate state ngoài actor

ADD trong `device_settings_internal.h` / `device_settings_events.c`:

```c
esp_err_t ds_actor_post_event(const device_settings_event_t *event);
void ds_actor_wake(void);
```

Các entry point bên ngoài actor chỉ được làm việc tối thiểu:

```text
scheduler completion callback
    -> build DS_EVENT_COMMAND_COMPLETE
    -> enqueue
    -> return

lease completion callback
    -> build DS_EVENT_LEASE_RESULT
    -> enqueue
    -> return

settings_values_end path
    -> commit/update staging theo memory-module contract
    -> build DS_EVENT_VALUES_STREAM_COMPLETE
    -> enqueue
    -> return

retry timer callback
    -> build DS_EVENT_RETRY_TIMER
    -> enqueue
    -> return

disconnect path
    -> build DS_EVENT_DISCONNECTED
    -> enqueue
    -> return
```

Không producer nào được đọc/ghi trực tiếp:

- `s_active_op`;
- active transaction state;
- `retry_count`;
- `read_ack_received`;
- `values_stream_complete`;
- `active_lease`;
- operation generation/progression.

Current implementation dùng `EventGroup` bit và callback mutate `s_active_op`; toàn bộ phần đó phải được loại bỏ trong GCF-05.

#### 10.4 Actor worker event loop thực tế

Worker phải xử lý **một event tại một thời điểm**, mọi transition chạy trong cùng actor task:

```c
static void device_settings_actor_task(void *arg)
{
    device_settings_event_t ev;

    while (s_actor.running) {
        if (xQueueReceive(s_actor.queue, &ev, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        switch (ev.type) {
        case DS_EVENT_START_OPERATION:
            ds_handle_start_operation(&ev);
            break;
        case DS_EVENT_LEASE_RESULT:
            ds_handle_lease_result(&ev);
            break;
        case DS_EVENT_COMMAND_COMPLETE:
            ds_handle_command_complete(&ev);
            break;
        case DS_EVENT_VALUES_STREAM_COMPLETE:
            ds_handle_values_complete(&ev);
            break;
        case DS_EVENT_RETRY_TIMER:
            ds_handle_retry_timer(&ev);
            break;
        case DS_EVENT_DISCONNECTED:
            ds_handle_disconnect(&ev);
            break;
        case DS_EVENT_CANCEL_DEVICE:
            ds_handle_cancel_device(&ev);
            break;
        case DS_EVENT_TRANSACTION_START:
            ds_handle_transaction_start(&ev);
            break;
        case DS_EVENT_TRANSACTION_ABORT:
            ds_handle_transaction_abort(&ev);
            break;
        case DS_EVENT_FORGET_DEVICE:
            ds_handle_forget_device(&ev);
            break;
        case DS_EVENT_SHUTDOWN:
            ds_handle_shutdown(&ev);
            return;
        }
    }
}
```

Không gọi completion của caller trong critical section/lock. Nếu `s_ops_invoke_completion()` có thể gọi code ngoài module, actor phải hoàn tất internal transition trước rồi mới invoke callback.


#### 10.4.1 Wiring matrix bắt buộc

Mỗi nguồn event phải có một đường runtime duy nhất vào actor loop. Bảng này là acceptance contract, không phải gợi ý:

| Producer hiện tại | Event đích | Producer được phép làm | Handler duy nhất được mutate state |
|---|---|---|---|
| Scheduler command completion | `DS_EVENT_COMMAND_COMPLETE` | copy compact result + enqueue | `ds_handle_command_complete()` |
| Scheduler lease completion | `DS_EVENT_LEASE_RESULT` | copy lease status/id + enqueue | `ds_handle_lease_result()` |
| Settings values-end / stream parser | `DS_EVENT_VALUES_STREAM_COMPLETE` | finalize memory staging theo memory contract + enqueue | `ds_handle_values_complete()` |
| Retry timer callback | `DS_EVENT_RETRY_TIMER` | enqueue cookie `(slot,generation,owner_token)` | `ds_handle_retry_timer()` |
| BLE disconnect/lifecycle callback | `DS_EVENT_DISCONNECTED` | enqueue device id + generation context | `ds_handle_disconnect()` |
| Public cancel API | `DS_EVENT_CANCEL_DEVICE` | enqueue request | `ds_handle_cancel_device()` |
| Public forget/delete integration | `DS_EVENT_FORGET_DEVICE` | enqueue request | `ds_handle_forget_device()` |
| Settings operation submit | `DS_EVENT_START_OPERATION` | reserve/copy immutable request metadata + enqueue | `ds_handle_start_operation()` |
| Settings transaction submit | `DS_EVENT_TRANSACTION_START` | reserve/copy immutable transaction request + enqueue | `ds_handle_transaction_start()` |
| Transaction abort request | `DS_EVENT_TRANSACTION_ABORT` | enqueue request | `ds_handle_transaction_abort()` |

Không được tồn tại hai đường xử lý song song cho cùng transition. Ví dụ `COMMAND_COMPLETE` không được vừa enqueue event vừa gọi `submit_next_command(tx)` trực tiếp trong callback.

#### 10.4.2 Producer wrapper mẫu

Scheduler completion callback phải nhỏ và không đọc mutable actor state:

```c
static void ds_scheduler_command_done(const device_control_result_t *result,
                                      void *ctx)
{
    const ds_callback_cookie_t *cookie = ctx;
    device_settings_event_t ev = {
        .type = DS_EVENT_COMMAND_COMPLETE,
        .slot = cookie->slot,
        .generation = cookie->generation,
        .owner_token = cookie->owner_token,
    };
    ev.data.command.job_id = cookie->job_id;
    ev.data.command.result = ds_compact_result(result);

    if (ds_actor_post_event(&ev) != ESP_OK) {
        ds_actor_terminal_fallback_store(&ev);
        ds_actor_wake();
    }
}
```

Timer callback cũng tương tự:

```c
static void ds_retry_timer_cb(TimerHandle_t timer)
{
    const ds_callback_cookie_t *cookie = pvTimerGetTimerID(timer);
    device_settings_event_t ev = {
        .type = DS_EVENT_RETRY_TIMER,
        .slot = cookie->slot,
        .generation = cookie->generation,
        .owner_token = cookie->owner_token,
    };
    (void)ds_actor_post_event(&ev);
}
```

Không được gọi từ producer wrapper:

```text
s_ops_set_state()
ds_tx_complete()
submit_next_command()
ds_release_active_lease()
s_active_op.* = ...
transaction->state = ...
```

#### 10.4.3 Actor handler phải thực hiện transition thật

`switch(event.type)` chỉ là dispatcher; checklist chỉ đạt khi handler thực hiện transition thật. Tối thiểu:

```c
static void ds_handle_command_complete(const device_settings_event_t *ev)
{
    ds_actor_operation_t *op = ds_actor_find_matching(ev);
    if (op == NULL) {
        ds_handle_stale_command_event(ev);
        return;
    }

    op->command_job_id = 0;

    if (!device_control_result_is_ok(&ev->data.command.result)) {
        ds_fail_operation_and_cleanup(op, &ev->data.command.result);
        return;
    }

    switch (op->command_kind) {
    case DS_CMD_DESCRIBE:
        ds_actor_submit_read_same_lease(op);
        break;
    case DS_CMD_READ:
        op->read_ack_received = true;
        ds_try_finish_read(op);
        break;
    case DS_CMD_TX_BEGIN:
    case DS_CMD_TX_SET:
    case DS_CMD_TX_COMMIT:
    case DS_CMD_TX_CONFIRM:
    case DS_CMD_TX_ABORT:
        ds_actor_advance_transaction(op, &ev->data.command.result);
        break;
    default:
        ds_fail_operation_internal(op);
        break;
    }
}
```

`ds_handle_lease_result()` phải lưu lease và submit command đầu tiên; `ds_handle_values_complete()` phải set flag rồi gọi `ds_try_finish_read()`; `ds_handle_disconnect()/cancel/forget()` phải invalidate generation trước khi cancel/release/clear runtime.


#### 10.5 Actor operation state machine

ADD state rõ ràng thay cho các bool rời rạc:

```c
typedef enum {
    DS_ACTOR_IDLE = 0,
    DS_ACTOR_WAIT_LEASE,
    DS_ACTOR_WAIT_COMMAND,
    DS_ACTOR_WAIT_READ_STREAM,
    DS_ACTOR_WAIT_READ_ACK,
    DS_ACTOR_WAIT_RETRY,
    DS_ACTOR_TX_ACTIVE,
    DS_ACTOR_COMPLETING,
    DS_ACTOR_CANCELLING,
} ds_actor_phase_t;
```

Active operation tối thiểu:

```c
typedef struct {
    bool active;
    uint16_t slot;
    uint32_t generation;
    device_control_owner_token_t owner_token;
    ds_actor_phase_t phase;

    device_control_lease_t lease_id;
    device_control_job_id_t command_job_id;

    bool read_ack_received;
    bool values_stream_complete;
    bool values_stream_success;
    uint8_t retry_count;
} ds_actor_operation_t;
```

`generation` dùng để loại stale work của slot; `owner_token` định danh logical operation ở scheduler. Mỗi lần bắt đầu operation mới phải cấp owner token mới, không reuse token của operation trước.

#### 10.6 Lease lifecycle thực tế

Lease không chỉ là field `lease_id`; actor phải có lifecycle explicit:

```text
IDLE
  ↓ start operation
WAIT_LEASE
  ↓ acquire_lease(device, source=SETTINGS, owner_token)
  ↓
LEASE_RESULT
  ├─ failure/cancel -> complete operation error
  └─ granted lease_id
         ↓ validate generation + owner_token
         ↓ store lease_id
         ↓ submit first scheduler job carrying same lease_id
         ↓ WAIT_COMMAND / TX_ACTIVE
```

##### 10.6.1 Request lease

```c
static esp_err_t ds_request_lease(ds_actor_operation_t *op,
                                  const char *device_id,
                                  device_control_priority_t priority)
{
    device_control_lease_request_t req = {
        .device_id = device_id,
        .priority = priority,
        .source = DEVICE_CTRL_SOURCE_SETTINGS,
        .owner_token = op->owner_token,
    };

    op->phase = DS_ACTOR_WAIT_LEASE;
    return device_control_scheduler_acquire_lease(
        &req, ds_lease_completion_cb, ds_make_callback_cookie(op));
}
```

Callback cookie không được là pointer trực tiếp tới mutable slot nếu lifetime có thể hết trước callback; dùng `(slot, generation, owner_token)` copy-out hoặc fixed callback context pool.

##### 10.6.2 Lease callback

Callback chỉ post event:

```c
static void ds_lease_completion_cb(esp_err_t status,
                                   device_control_lease_t lease_id,
                                   void *ctx)
{
    const ds_callback_cookie_t *cookie = ctx;
    device_settings_event_t ev = {
        .type = DS_EVENT_LEASE_RESULT,
        .slot = cookie->slot,
        .generation = cookie->generation,
        .owner_token = cookie->owner_token,
    };
    ev.data.lease.status = status;
    ev.data.lease.lease_id = lease_id;
    (void)ds_actor_post_event(&ev);
}
```

##### 10.6.3 Stale lease grant — bắt buộc release

Đây là case dễ leak lease:

```text
operation generation 10 requests lease
    ↓
disconnect/cancel -> actor moves to generation 11
    ↓
scheduler later grants old request generation 10
```

Actor **không được chỉ ignore event**. Nếu event stale nhưng chứa `lease_id != 0`, phải release lease ngay:

```c
if (!ds_event_matches_active_op(ev)) {
    if (ev->data.lease.lease_id != 0) {
        (void)device_control_scheduler_release_lease(ev->data.lease.lease_id);
    }
    return;
}
```

Acceptance bắt buộc: `active_lease_count` trở về baseline sau stale grant/cancel race.

##### 10.6.4 Hold lease

Sau `LEASE_GRANTED`, actor lưu `lease_id`. Mọi scheduler job thuộc logical operation đó phải gửi:

```c
job.source = DEVICE_CTRL_SOURCE_SETTINGS;
job.owner_token = op->owner_token;
job.lease_id = op->lease_id;
```

Không tạo command Settings với `lease_id = 0` trong khi operation đang ở critical stream/transaction scope.

##### 10.6.5 Release lease

Lease chỉ release khi logical operation thật sự kết thúc:

```c
static void ds_release_active_lease(ds_actor_operation_t *op)
{
    if (op->lease_id == 0) {
        return;
    }

    device_control_lease_t lease = op->lease_id;
    esp_err_t err = device_control_scheduler_release_lease(lease);
    if (err == ESP_OK || err == ESP_ERR_NOT_FOUND ||
        err == ESP_ERR_INVALID_STATE) {
        /* NOT_FOUND/INVALID_STATE có thể xảy ra nếu scheduler đã revoke do
         * disconnect/quiesce; không được release một lease khác. */
        op->lease_id = 0;
    } else {
        /* Không silently clear local ownership khi release request chưa được
         * scheduler accept. Log + retry/cleanup path bounded. */
        DS_DIAG_INC(lease_release_fail);
    }
}
```

Nếu scheduler API dùng error code khác cho `already revoked`, tài liệu implementation phải map chính xác; yêu cầu logic là idempotent đối với lease đã revoke, nhưng stale/wrong lease không được làm ảnh hưởng lease owner khác.

#### 10.7 READ lifecycle — ACK và values stream là hai điều kiện độc lập

READ phải giữ lease qua cả transport ACK và values stream.

##### Case A — ACK tới trước `settings_values_end`

```text
WAIT_COMMAND
  ↓ COMMAND_COMPLETE(OK)
read_ack_received = true
  ↓ values_stream_complete == false
phase = WAIT_READ_STREAM
  ↓ VALUES_STREAM_COMPLETE(success)
logical success
  ↓ release lease
  ↓ complete op
```

##### Case B — `settings_values_end` tới trước ACK

```text
WAIT_COMMAND
  ↓ VALUES_STREAM_COMPLETE(success)
values_stream_complete = true
  ↓ read_ack_received == false
phase = WAIT_READ_ACK
  ↓ COMMAND_COMPLETE(OK)
logical success
  ↓ release lease
  ↓ complete op
```

Helper bắt buộc centralize condition:

```c
static void ds_try_finish_read(ds_actor_operation_t *op)
{
    if (!op->active || !op->read_ack_received ||
        !op->values_stream_complete) {
        return;
    }

    ds_op_result_t result = op->values_stream_success
                                ? DS_OP_RESULT_OK
                                : DS_OP_RESULT_PROTOCOL_ERROR;
    ds_release_active_lease(op);
    ds_finish_operation(op, result);
}
```

Nếu command ACK fail/timeout/cancel trước stream hoàn tất: invalidate current read generation, cancel matching owner jobs, release/revoke lease, discard/abort staging snapshot theo memory contract và complete error.

Nếu stream báo failure nhưng ACK success: release lease và complete `PROTOCOL_ERROR`; không giữ lease chờ thêm event.

#### 10.8 DESCRIBE → READ lifecycle

Nếu current Settings flow yêu cầu DESCRIBE rồi READ, **không release lease giữa hai command**:

```text
acquire lease
  ↓
DESCRIBE_SETTINGS
  ↓ ACK OK
READ_SETTINGS          <- same owner_token + same lease_id
  ↓ ACK + values_end
commit logical result
  ↓
release lease
```

DESCRIBE completion chỉ advance actor phase và submit READ. Không set operation về generic QUEUED rồi cho scheduler/device khác chen vào trên cùng device.

#### 10.9 Transaction lease lifecycle

Transaction/reconcile giữ một lease xuyên suốt critical section:

```text
TRANSACTION_START
  ↓ allocate generation + owner_token
WAIT_LEASE
  ↓ LEASE_GRANTED
TX_BEGIN
  ↓ ACK OK
TX_SET #1
  ↓ ACK OK
...
TX_SET #N
  ↓ ACK OK
TX_COMMIT
  ↓ ACK OK
reconcile/read/confirm theo protocol hiện tại
  ↓ logical transaction complete
release lease
  ↓ caller completion
```

Các command liên tiếp không acquire/release lease lại.

Actor state cần biết transaction step:

```c
typedef enum {
    DS_TX_NONE = 0,
    DS_TX_BEGIN,
    DS_TX_SET,
    DS_TX_COMMIT,
    DS_TX_RECONCILE_READ,
    DS_TX_CONFIRM,
    DS_TX_ABORT,
} ds_tx_phase_t;
```

Nếu bất kỳ step terminal fail:

1. stop scheduling next TX step;
2. nếu protocol cho phép/đòi hỏi `TX_ABORT`, submit abort bằng **cùng lease**;
3. sau abort completion hoặc bounded abort failure: release/revoke lease;
4. invalidate generation/owner token;
5. complete transaction exactly once.

Không release lease ngay sau `TX_COMMIT ACK` nếu flow còn reconcile/read/confirm thuộc cùng atomic Settings workflow.

#### 10.10 Cancellation / disconnect / forget ordering

Thứ tự actor bắt buộc để tránh late completion resurrect state:

```text
receive DISCONNECTED / CANCEL_DEVICE / FORGET_DEVICE
  ↓
capture old owner_token + lease_id
  ↓
invalidate generation / mark CANCELLING
  ↓
cancel_source(device, SETTINGS, old_owner_token)
  ↓
release lease nếu scheduler chưa revoke
  ↓
abort/discard transient staging as required
  ↓
clear active operation / transaction runtime
  ↓
complete caller exactly once (trừ destructive forget contract nếu caller đã gone)
```

`FORGET_DEVICE` thêm:

- clear committed Settings schema/value snapshot;
- clear pending operation slots của device;
- clear retry metadata;
- clear transaction/reconcile runtime;
- không cho stale event tạo lại runtime của device.

Nếu scheduler/device lifecycle đã revoke lease trước actor event, actor coi release là idempotent cleanup; không được coi đó là fatal error.

#### 10.11 Retry lifecycle

Retry timer callback chỉ enqueue `DS_EVENT_RETRY_TIMER` với `(slot, generation, owner_token)`.

Worker khi nhận:

```text
validate current generation/token
  ↓ stale -> ignore
  ↓ valid
lease still active?
  ├─ yes -> retry command with same lease
  └─ no  -> request/reacquire lease only if logical operation policy permits
```

Không retry `DEVICE_CMD_STATUS_BUSY` như behavior bình thường sau khi migration hoàn tất; Scheduler serialization phải làm `DCS BUSY ≈ 0`. BUSY được log/metric như invariant violation và chỉ retry bounded để tránh làm hỏng user flow.

#### 10.12 Queue-full / must-deliver rule cho actor events

Các event terminal (`LEASE_RESULT`, `COMMAND_COMPLETE`, `DISCONNECTED`, `FORGET_DEVICE`, `SHUTDOWN`) không được silently drop.

Yêu cầu implementation:

- queue capacity phải benchmark với burst 16 registered devices;
- `ds_actor_post_event()` increment `event_queue_full` metric khi fail;
- completion callback không được block dài trong Scheduler task;
- nếu queue full ở event terminal, dùng bounded fallback đã định nghĩa trước (ví dụ fixed per-slot completion mailbox + task notify), không chỉ `ESP_LOGW` rồi bỏ event;
- acceptance production: `event_queue_full == 0` trong soak; fallback count cũng phải 0 trong normal workload.

Không dùng heap allocation cho từng actor event.

#### 10.13 Settings priorities/deadlines

- active transaction/reconcile: `HIGH + lease`;
- interactive GET: `NORMAL + lease`;
- background capability/settings refresh: `LOW/NORMAL + lease`;
- retry bounded, không busy spin.

Queue deadline do actor policy; interactive request không được chờ vô hạn. Khi scheduler trả `DEADLINE_EXCEEDED`, actor đi terminal cleanup: invalidate current command, release lease và complete operation timeout/busy phù hợp API hiện tại.

#### 10.14 Transaction migration

`device_settings_transaction.c` không gọi DCS trực tiếp.

Mọi command đi:

```text
Settings Actor
   -> Scheduler(job.source=SETTINGS,
                owner_token=current owner,
                lease_id=current lease)
   -> DCS
```

Completion callback chỉ post `DS_EVENT_COMMAND_COMPLETE`; transaction advancement chỉ chạy trong actor event handler.


#### 10.14.1 Transaction ownership migration bắt buộc

`device_settings_transaction.c` sau GCF-05 chỉ nên giữ:

- transaction data model / validation helpers;
- immutable change list ownership;
- formatting/publish helper không tự advance state;
- lookup/copy-out helper nếu cần.

Các function sau phải được remove hoặc chuyển thành actor-side helper không callable từ callback context:

```text
on_cmd_complete()
submit_next_command()  // nếu function này tự submit từ transaction module
reconciliation_timeout_cb() // nếu callback này trực tiếp complete transaction
```

Thay thế flow:

```text
transaction module API
    -> DS_EVENT_TRANSACTION_START
    -> actor acquires lease
    -> actor submits TX_BEGIN
    -> COMMAND_COMPLETE
    -> actor advances TX_SET/TX_COMMIT/...
    -> timer only posts RETRY/RECONCILE_TIMEOUT event
    -> actor finalizes transaction
```

Nếu cần giữ `reconciliation_timeout_cb()`, callback chỉ được build/post event, không gọi `ds_tx_complete()` trực tiếp.


#### 10.15 `device_settings_forget()`

Public API:

```c
esp_err_t device_settings_forget(const char *device_id);
```

Semantics destructive:

- cancel source jobs/lease;
- invalidate generation;
- clear committed settings schema;
- clear values snapshot;
- clear transaction/reconcile/retry runtime.

Disconnect thông thường không gọi forget nếu product muốn reuse snapshot. Delete bắt buộc gọi forget.

#### 7.5 Lease/reservation state machine

Lease acquisition là scheduler operation, không phải mutex lấy trực tiếp từ caller context.

```text
LEASE_REQUEST queued
      ↓ arbitration theo priority/fairness
no transport inflight + no active lease
      ↓
LEASE_GRANTED
      ↓
actor submit jobs carrying lease_id
      ↓
logical operation complete/cancel
      ↓
RELEASE_LEASE
```

Khi lease active:

- jobs không có matching lease không dispatch trên device;
- matching lease jobs vẫn chịu `transport_inflight <= 1`;
- disconnect/delete auto-revoke lease và actor nhận cancel event/generation invalidation;
- lease callback không mutate Settings state trực tiếp, chỉ post event về actor.

#### 7.6 Scheduler worker responsibilities

Worker là sole owner sau slot accepted/enqueued:

- move `ENQUEUE_PENDING -> QUEUED`;
- priority/fairness arbitration;
- grant/release lease;
- expire queue deadlines;
- dispatch DCS;
- process per-slot completion pending;
- exact-once terminal transition;
- targeted cancel/supersede;
- unblock next runnable job;
- stats/high-water.

#### 7.7 Completion path không được drop

Preferred v1.1:

```text
DCS completion callback
      │
      ├─ validate slot index + generation
      ├─ copy compact device_command_result_t into inflight slot
      ├─ state = COMPLETION_PENDING
      └─ xTaskNotifyGive(scheduler_task)
             │
             ▼
       Scheduler Worker
             ├─ clear transport inflight
             ├─ terminal callback exactly once
             └─ dispatch next runnable job
```

Không dùng bounded completion queue làm nơi duy nhất giữ terminal result.

Nếu implementation vẫn dùng queue, queue overflow phải có fallback mailbox và tuyệt đối không drop completion.

#### 7.8 Targeted cancel

Ba cấp:

1. `cancel_job(job_id)` — đúng một job;
2. `cancel_source(device, source, owner_token)` — ví dụ seed revision cũ;
3. `cancel_device(device)` — lifecycle destructive operation.

State seed revision replacement **không** được gọi `cancel_device()`.

### 17.5 State / Settings / Web / MCP

- State add scheduler + shared command types;
- Settings add scheduler + shared command types;
- Web/MCP đổi DCS dependency sang scheduler/shared command types nếu không còn service API usage.

### 17.6 Main

Add:

```text
device_control_scheduler
device_schema_control_adapter
device_command_types
```

và `gateway_runtime.c` vào `SRCS`.

## 19. Memory budget

### 19.1 Nguyên tắc

ESP32-S3 có PSRAM nhưng transport-critical metadata vẫn ưu tiên internal RAM.

#### Internal RAM

Ưu tiên:

- queue control structures;
- scheduler per-device metadata;
- DCS pending table;
- urgent ACK queue storage;
- compact urgent ACK/lifecycle structs;
- scheduler slot state/generation/completion mailbox metadata.

#### PSRAM/external preferred

Có thể dùng cho:

- command request payload storage nếu slot struct lớn;
- schema snapshots tạm;
- settings snapshot/large strings;
- Web/MCP JSON allocations qua memory policy hiện có.

#### 19.2 Không tạo task tràn lan

Target new persistent tasks:

1. Control Scheduler worker: +1 task.
2. Settings: reuse worker hiện có.
3. State seed: ưu tiên reuse scheduler completion + compact actor mechanism; nếu cần task riêng thì stack nhỏ và benchmark.
4. Web/MCP: reuse HTTP/MCP worker, không tạo task mới nếu tránh được.

#### 19.3 Metrics cần log

At checkpoints:

- free internal heap;
- largest internal block;
- free PSRAM;
- largest PSRAM block;
- scheduler pool high-water mark;
- DCS urgent queue high-water mark;
- scheduler queue full count;
- scheduler queue deadline count;
- active lease count/max hold time;
- completion mailbox conflict count.

---


## 3. Implementation plan của phase

### GCF-05 — Settings single-owner actor + logical lease

#### Mục tiêu

Loại cross-context mutation và bảo vệ logical Settings stream/transaction khỏi interleaving.

#### ADD

```text
components/device_settings/device_settings_internal.h
components/device_settings/device_settings_events.c
components/device_settings/test/test_device_settings_actor.c
```

#### EDIT

```text
components/device_settings/device_settings.c
components/device_settings/device_settings_worker.c
components/device_settings/device_settings_operation.c
components/device_settings/device_settings_transaction.c
components/device_settings/device_settings_memory.c
components/device_settings/include/device_settings.h
components/device_settings/CMakeLists.txt
components/device_settings/test/*
```

#### Implementation

**Actor processing bắt buộc:**

- thay EventGroup-bit completion hiện tại bằng `QueueHandle_t` chứa `device_settings_event_t` có payload;
- implement `device_settings_actor_task()` với `switch(event.type)` và handler riêng;
- implement `ds_handle_start_operation()`;
- implement `ds_handle_lease_result()`;
- implement `ds_handle_command_complete()`;
- implement `ds_handle_values_complete()`;
- implement `ds_handle_retry_timer()`;
- implement `ds_handle_disconnect()`;
- implement `ds_handle_cancel_device()`;
- implement `ds_handle_transaction_start()/abort()`;
- implement `ds_handle_forget_device()`;
- callback/timer/BLE entry point chỉ copy compact payload + enqueue event, không mutate actor state;
- actor hoàn tất internal transition trước khi invoke callback ngoài module.

**Lease lifecycle bắt buộc:**

- mỗi logical operation cấp `generation + owner_token` mới;
- `START_OPERATION -> WAIT_LEASE -> LEASE_RESULT`;
- lease grant hợp lệ được lưu vào `active_op.lease_id`;
- mọi Settings command trong scope mang cùng `owner_token + lease_id`;
- DESCRIBE -> READ giữ nguyên lease;
- READ giữ lease cho tới đủ `ACK_OK && VALUES_END_OK`;
- Settings transaction giữ cùng lease xuyên `TX_BEGIN -> TX_SET* -> TX_COMMIT -> reconcile/read/confirm`;
- stale `LEASE_RESULT` có lease_id phải release ngay, không chỉ ignore;
- command fail/timeout/cancel phải cancel owner jobs, cleanup staging và release/revoke lease;
- disconnect/cancel/forget invalidate generation trước khi cleanup để late completion không resurrect state;
- release/revoke phải idempotent với lease đã bị scheduler revoke do lifecycle;
- add metric `event_queue_full`, `lease_release_fail`, stale-event count và lease hold metric.

**Migration:**

- Settings worker là sole state-machine owner;
- remove direct `device_command_service_submit()` khỏi worker/transaction;
- add `device_settings_forget()`;
- retry BUSY chỉ là bounded compatibility fallback; sau scheduler migration `DCS BUSY` được xem là invariant violation.

#### Test

**Phase test boundary:** unit/integration tests dưới đây là bắt buộc, đồng thời GCF-05 phải PASS **Functional HIL acceptance** ở §25. Full stress/soak/multi-device saturation không thuộc GCF-05.

1. actor queue xử lý burst event có payload, không coalesce/mất completion;
2. READ ACK before values_end -> lease remains, Web job cannot interleave;
3. values_end before ACK -> operation waits;
4. DESCRIBE -> READ dùng cùng lease, không có non-owner interleave;
5. transaction lease prevents Web/MCP interleave xuyên cả reconcile/confirm;
6. lease release dispatches queued HIGH job;
7. stale command generation ignored nhưng terminal cleanup vẫn exact-once;
8. stale lease grant sau cancel được release, active lease count không leak;
9. disconnect during WAIT_LEASE;
10. disconnect during active READ lease;
11. disconnect during transaction lease;
12. cancel/forget invalidates generation trước late ACK/values_end;
13. forget clears snapshots/runtime/jobs/lease;
14. retry timer stale generation bị ignore;
15. queue-full must-deliver fallback không làm mất terminal completion;
16. actor event burst không còn race `s_active_op`.

#### Checklist

**Không được đánh dấu GCF-05 DONE nếu thiếu một trong ba trụ cột:**

1. actor worker/event processing thực sự;
2. transaction migration vào actor-owned state machine;
3. functional HIL gate PASS.

Lease lifecycle là contract xuyên suốt actor worker và transaction migration, không phải hạng mục tùy chọn.

- [ ] Actor worker/event loop là runtime path chính của Settings state machine.
- [ ] Transaction migration hoàn chỉnh: không còn callback/timer ngoài actor tự advance TX state.
- [ ] GCF-05 Functional HIL acceptance (§25) PASS.
- [x] Payload actor queue được implement; không dùng EventGroup bit làm carrier cho completion payload.
- [ ] `device_settings_actor_task()` có dispatch handler cho mọi event type.
- [ ] Wiring matrix được implement đầy đủ: mỗi producer runtime chỉ enqueue event và mỗi event có đúng một actor handler mutate state.
- [ ] `switch(event.type)` không chỉ tồn tại hình thức; các handler thực sự thực hiện transition/submit-next/cleanup.
- [ ] `START_OPERATION` thực sự request lease và chuyển `WAIT_LEASE`.
- [x] Lease callback chỉ post `DS_EVENT_LEASE_RESULT`.
- [ ] Stale lease grant có `lease_id != 0` được release ngay.
- [x] `COMMAND_COMPLETE` chỉ mutate state trong actor task.
- [ ] `VALUES_STREAM_COMPLETE` chỉ mutate active operation trong actor task.
- [x] Retry timer callback chỉ post event.
- [ ] Disconnect/cancel/forget entry point chỉ post event.
- [x] Cross-context `s_active_op` mutation removed.
- [ ] `device_settings_transaction.c` không còn callback/timer tự advance transaction state ngoài actor.
- [ ] Không còn dual-path kiểu "enqueue event + mutate/advance trực tiếp" cho cùng completion.
- [x] Actor events bounded, không heap-per-event.
- [ ] Terminal actor event không silently drop.
- [ ] READ uses logical lease.
- [ ] DESCRIBE -> READ giữ cùng lease.
- [ ] ACK + values_end two-condition completion pass.
- [ ] Transaction/reconcile/confirm lease semantics implemented.
- [ ] Command fail/timeout/cancel cleanup release/revoke lease.
- [ ] Disconnect/cancel invalidate generation trước cleanup.
- [ ] Stale generation/owner-token guard pass.
- [ ] Lease release/revoke idempotent với lifecycle race.
- [ ] `device_settings_forget()` purge jobs/lease/runtime/snapshots.
- [x] No direct DCS submit in Settings.
- [ ] `event_queue_full == 0` trong unit/integration tests và GCF-05 functional HIL gate.
- [ ] `active_lease_count` trở về baseline sau cancel/disconnect/forget tests.
- [x] Plan doc updated.

#### Reboot reconciliation regression — 2026-09-10

- Expected reboot disconnect không còn abort/free transaction đang
  `WAITING_REBOOT`; transaction được giữ lại và lease transport được release để
  post-reboot `read_settings` có thể chạy.
- Reconciliation timeout đi tới terminal `outcome_unknown` thay vì bị ghi sai
  thành `cancelled`.
- HIL Save một setting trên thiết bị `AC:27:6E:CC:F2:26` PASS đầy đủ:
  revision `21 → 22`, WebSocket đi qua `waiting_reboot → reconnecting →
  verifying → succeeded`, REST snapshot sau reboot khớp giá trị đã lưu.
- Đây là một regression case đã PASS, chưa thay thế toàn bộ functional HIL và
  actor ownership checklist; GCF-05 không đánh DONE.

---


## 4. Test cases chi tiết

### TC-SET-001 — ACK before stream end

READ ACK arrives, no `values_end` yet.

Expected operation remains active and lease remains held.

### TC-SET-002 — Stream end before ACK

Expected operation waits for ACK before success/release.

### TC-SET-003 — Stale callback

Generation 10 cancelled/reused as 11, inject completion 10.

Expected actor ignores stale event without corrupting 11.

### TC-SET-004 — Web cannot interleave after READ ACK

READ ACK arrives while values stream delayed. Queue Web HIGH command same device.

Expected Web command does not dispatch until Settings releases lease.

### TC-SET-005 — Transaction lease

TX_BEGIN -> TX_SET -> TX_COMMIT/reconcile with Web/MCP queued same device.

Expected no non-owner interleave inside defined transaction lease scope.



### TC-SET-006 — Actor event dispatch thực tế

Inject theo thứ tự `START_OPERATION -> LEASE_RESULT -> COMMAND_COMPLETE -> VALUES_STREAM_COMPLETE`.

Expected:

- mỗi event đi qua đúng handler;
- state transition `IDLE -> WAIT_LEASE -> WAIT_COMMAND -> WAIT_READ_STREAM/ACK -> COMPLETING -> IDLE`;
- không callback producer nào mutate active operation trực tiếp.

### TC-SET-007 — Stale lease grant after cancel

1. operation generation 20 request lease;
2. inject CANCEL/DISCONNECT, actor chuyển generation 21;
3. inject lease callback cũ generation 20 với `lease_id != 0`.

Expected old lease được release/revoke ngay, event không activate operation mới, `active_lease_count` trở về baseline.

### TC-SET-008 — Disconnect while waiting lease

Disconnect trước khi scheduler grant lease.

Expected actor invalidate owner token; lease request cũ bị cancel. Nếu grant race vẫn xảy ra, stale grant cleanup release lease. Không có Settings command được dispatch sau disconnect.

### TC-SET-009 — Disconnect after READ ACK before values_end

READ ACK OK, lease đang held, sau đó disconnect trước `settings_values_end`.

Expected actor cancel matching owner jobs, invalidate generation, release/revoke lease, discard transient read state và complete exactly once. Late `values_end` bị ignore.

### TC-SET-010 — DESCRIBE/READ same lease

Queue Web HIGH command cùng device trong lúc DESCRIBE đang chạy.

Expected DESCRIBE ACK advance trực tiếp sang READ với cùng `lease_id`; Web chỉ dispatch sau READ logical completion và lease release.

### TC-SET-011 — Transaction failure cleanup

TX_BEGIN OK -> TX_SET fail/timeout.

Expected actor không schedule TX_SET tiếp; nếu protocol yêu cầu thì TX_ABORT dùng cùng lease; sau bounded abort path lease được release/revoke và transaction callback chỉ chạy một lần.

### TC-SET-012 — Must-deliver completion event

Làm actor queue gần đầy rồi inject command terminal completion.

Expected completion không silently drop. Primary queue hoặc fallback mailbox đánh thức actor và operation đạt terminal state. Không leak callback context/job/lease.

### TC-SET-013 — Stale retry timer

Schedule retry generation N, sau đó cancel operation và reuse slot generation N+1 trước khi timer fire.

Expected `DS_EVENT_RETRY_TIMER(N)` bị ignore; không submit command cho operation mới.

### TC-SET-014 — Forget with late ACK and stream

FORGET device khi command inflight; sau purge inject late ACK và `settings_values_end` của owner cũ.

Expected không recreate snapshot/runtime, không publish stale settings, không leak lease/job, device có thể add lại sạch.



### TC-SET-015 — Producer callback wiring

Instrument producer callbacks và actor handlers. Inject scheduler completion/lease completion/timer/disconnect.

Expected mỗi producer chỉ enqueue đúng một event; state chỉ đổi khi actor task consume event. Trước khi actor consume, snapshot mutable state không đổi.

### TC-SET-016 — No dual-path transaction advance

TX_BEGIN completion được inject trong khi actor task tạm blocked.

Expected `next_change_index`, transaction state và next TX submit **không đổi** cho tới khi actor consume `DS_EVENT_COMMAND_COMPLETE`. Điều này bắt trường hợp callback vừa enqueue vừa gọi `submit_next_command()` trực tiếp.

### TC-SET-017 — Lease propagated to every job

Acquire lease L cho operation owner O, chạy DESCRIBE→READ và TX_BEGIN→TX_SET→TX_COMMIT.

Expected mọi scheduler job của operation đều có `lease_id == L` và `owner_token == O`; không có Settings job critical scope nào dùng lease 0.

### TC-SET-018 — State unchanged before actor consume

Pause actor task, inject `COMMAND_COMPLETE`, `VALUES_STREAM_COMPLETE`, `DISCONNECTED`.

Expected producer callbacks trả về sau enqueue nhưng mutable operation/transaction state chưa đổi. Resume actor; transitions mới xảy ra theo event order.


## 5. Acceptance / static analysis / build-hardware gate

#### After GCF-05 — Settings actor ownership

```sh
git grep "s_active_op" -- components/device_settings
```

Review all writes: only actor worker execution path mutates state. Callback/timer/BLE entry points only post events.

Also:

```sh
git grep "device_command_service_submit" -- components/device_settings
```

Expected none.


### 5.1 Static checks bắt buộc để chứng minh actor wiring

Các grep sau là **phase gate**, không chỉ dùng để tham khảo.

#### A. Settings không còn direct DCS submit

```sh
git grep -n "device_command_service_submit(" -- components/device_settings
```

Expected production result:

```text
<no matches>
```

#### B. Legacy EventGroup không còn là carrier của Settings state machine

```sh
git grep -n "xEventGroup\|EventGroupHandle_t\|DS_WORK_BIT_" -- components/device_settings
```

Expected: không còn trong worker/transaction flow của GCF-05. Nếu còn dùng EventGroup cho mục đích hoàn toàn khác, phải document rõ và không được dùng để transport command/lease/stream completion.

#### C. Producer callbacks không mutate actor state

```sh
git grep -n "on_cmd_complete\|retry_timer_cb\|on_values_complete\|on_disconnect" -- components/device_settings
```

Review từng callback: body chỉ build/copy event + enqueue/wake. Không được thấy direct writes vào operation/transaction/lease state.

#### D. Transaction không tự advance ngoài actor

```sh
git grep -n "submit_next_command\|ds_tx_complete\|transition_tx" -- components/device_settings/device_settings_transaction.c
```

Expected: các function mutate transaction hoặc submit-next chỉ còn được gọi từ actor-owned execution path, hoặc đã được chuyển vào actor module. Timer/scheduler callback không được gọi trực tiếp các function này.

#### E. Actor loop và handler symbols phải tồn tại và có caller thực

```sh
git grep -n "device_settings_actor_task\|ds_handle_lease_result\|ds_handle_command_complete\|ds_handle_values_complete\|ds_handle_disconnect\|ds_handle_forget_device" -- components/device_settings
```

Expected: tất cả symbol tồn tại và được referenced từ actor dispatcher, không phải dead helper.

#### F. Lease phải được truyền vào Settings jobs thật

```sh
git grep -n "lease_id\|owner_token" -- components/device_settings
```

Review submit path: every DESCRIBE/READ/TX job trong active logical scope phải copy `active_op.lease_id` và `owner_token` vào scheduler job. Không chấp nhận chỉ lưu lease trong actor state nhưng submit job với `lease_id = 0`.

#### G. Mutable state write ownership

```sh
git grep -n "s_active_op\|retry_count\|read_ack_received\|values_stream_complete\|lease_id" -- components/device_settings
```

Review writes: chỉ actor task/actor handlers được mutate runtime fields. Producer context chỉ đọc immutable cookie/copy-out metadata cần để build event.

### 5.2 Runtime acceptance bắt buộc

Không đánh dấu GCF-05 DONE nếu chưa chứng minh runtime:

1. `START_OPERATION` vào queue và actor handler thực sự request lease.
2. Lease grant callback enqueue `LEASE_RESULT`; actor lưu lease và submit command đầu tiên.
3. DCS/Scheduler completion enqueue `COMMAND_COMPLETE`; actor mới advance DESCRIBE→READ hoặc TX step.
4. READ ACK không release lease khi chưa có `values_end`.
5. `values_end` không complete READ khi ACK chưa tới.
6. Disconnect/cancel/forget đi qua actor event trước khi state bị clear.
7. Late ACK/late lease grant/late values_end không revive operation cũ.
8. Sau mọi terminal path, `active_lease_count` và Settings active-operation count trở về baseline.
9. Queue pressure không làm mất terminal completion.
10. Web/MCP job cùng device không dispatch khi Settings lease đang held.


## 24. Build validation

Root firmware:

```sh
git submodule update --init --recursive
idf.py set-target esp32s3
idf.py reconfigure
idf.py build
```

Test app:

```sh
cd test
idf.py set-target esp32s3
idf.py reconfigure
idf.py build
idf.py -p <PORT> flash monitor
```

Do project bật `MINIMAL_BUILD ON`, nếu component mới compile riêng nhưng không có trong final app, kiểm tra `REQUIRES` chain trước.

---

## 25. GCF-05 Functional HIL acceptance

### 25.1 Mục tiêu

HIL của GCF-05 chỉ xác nhận **actor worker + transaction migration + lease lifecycle hoạt động trên thiết bị thật**. Đây là functional gate để đóng phase, không phải stress/soak gate.

GCF-05 **không yêu cầu**:

- 9-device saturation;
- 2h/8h/24h soak;
- BLE + Wi-Fi heavy traffic stress;
- long-run heap fragmentation;
- scheduler fairness saturation;
- reconnect storm nhiều thiết bị.

Các bài trên thuộc **GCF-08**.

### 25.2 Functional HIL bắt buộc

Chạy với tối thiểu 1 device thật; có thể dùng device thứ hai để kiểm tra thêm non-interleaving nếu thuận tiện, nhưng không biến thành multi-device stress.

1. flash gateway và boot STA mode;
2. connect BLE device;
3. schema discovery hoàn tất;
4. Settings DESCRIBE chạy qua actor + scheduler + lease;
5. Settings READ chạy qua actor + scheduler + cùng logical lease;
6. xác nhận ACK trước `values_end` không release lease;
7. xác nhận `values_end` trước ACK không complete operation;
8. chạy transaction thật `TX_BEGIN -> TX_SET* -> TX_COMMIT`;
9. xác nhận transaction advancement chỉ xảy ra sau actor consume `COMMAND_COMPLETE`;
10. disconnect giữa READ và xác nhận actor cleanup + release/revoke lease + exact-once completion;
11. disconnect/cancel giữa transaction và xác nhận không submit step mới sau invalidation;
12. reconnect và chạy reconciliation/confirm nếu protocol path yêu cầu;
13. queue một Web/MCP command cùng device trong lúc Settings giữ lease và xác nhận command đó không dispatch trước lease release;
14. chạy `device_settings_forget()`/delete-path integration và inject late ACK/`values_end` nếu test harness hỗ trợ;
15. xác nhận không crash, WDT, deadlock, lease leak hoặc active-operation leak;
16. xác nhận `event_queue_full == 0` và `active_lease_count` trở về baseline sau terminal paths.

### 25.3 GCF-05 HIL pass criteria

GCF-05 functional HIL PASS khi:

- actor worker là runtime path thực sự, không phải helper/dead code;
- callback/timer/BLE producer chỉ enqueue event;
- READ two-condition completion đúng trên device thật;
- transaction chạy hoàn toàn qua actor-owned advancement;
- cùng `lease_id + owner_token` được giữ xuyên logical critical scope;
- disconnect/cancel/forget không làm stale completion resurrect state;
- Web/MCP không interleave khi Settings lease đang held;
- không leak lease/job/context/runtime state;
- không có `event_queue_full`, WDT, crash hoặc deadlock trong functional run.

### 25.4 Phân ranh với GCF-08

Sau khi GCF-05 functional HIL PASS có thể đóng GCF-05 nếu toàn bộ checklist khác cũng PASS. Không cần đợi 9-device stress/soak.

GCF-08 chịu trách nhiệm:

```text
4-device saturation
9-device reconnect/state-seed + Wi-Fi traffic
2h dev soak
8-24h release soak
long-run memory trend
queue/fairness stress
```

---


## 6. Checklist đóng phase

### 6.1 Definition of DONE riêng cho GCF-05

GCF-05 chỉ được gắn `✅ DONE` khi đồng thời thỏa cả bốn nhóm:

```text
A. Actor worker thực thi thật
B. Transaction đã migrate vào actor
C. Lease lifecycle hoạt động đúng
D. Unit/integration + Functional HIL PASS
```

Không yêu cầu GCF-08 stress/soak để đóng GCF-05. Ngược lại, compile PASS hoặc có event/lease API nhưng chưa nối runtime **không đủ** để đóng phase.

- [x] Tất cả file **ADD** trong phase đã được thêm vào CMake dependency graph.
- [ ] Tất cả file **EDIT** trong phase đã được cập nhật đúng contract mới.
- [ ] Không còn caller/flow legacy bị phase này yêu cầu loại bỏ.
- [ ] Unit test của phase PASS trên test project.
- [x] Firmware root project build PASS cho `esp32s3`.
- [ ] Không xuất hiện warning mới liên quan ownership, queue, task stack hoặc lifetime.
- [ ] Static grep/acceptance của phase PASS.
- [ ] Hardware test bắt buộc của phase PASS nếu phase yêu cầu BLE/Wi-Fi thực.
- [ ] Memory checkpoint không regression ngoài budget đã định nghĩa.
- [ ] Checklist chi tiết trong phần Implementation plan đã được đánh `[x]` toàn bộ.
- [ ] Heading phase được cập nhật `✅ DONE (YYYY-MM-DD)` trước khi bắt đầu phase kế tiếp.
