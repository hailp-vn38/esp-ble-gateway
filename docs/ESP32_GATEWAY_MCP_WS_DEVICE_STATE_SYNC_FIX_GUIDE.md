# ESP32 BLE Gateway — MCP → Device → WebSocket State Sync Fix Guide

**Project:** ESP32-GATEWAY  
**Gateway repo:** `hailp-vn38/esp-ble-gateway`  
**Gateway branch:** `dev-ws`  
**Device repo:** `hailp-vn38/esp-ble-device`  
**Target:** ESP32-S3  
**Purpose:** Fix trường hợp MCP gọi tool của device thành công nhưng Web UI không cập nhật runtime state của device.

---

## 1. Triệu chứng

Log điển hình:

```text
I (...) dispatcher: [CMD_SEND] device=AC:27:6E:CC:F2:26 request_id=7 command=set_led
I (...) device_state: [AC:27:6E:CC:F2:26] feature=led_main prop=1 state updated
I (...) dispatcher: [CMD_ACK] device=AC:27:6E:CC:F2:26 request_id=7 command=set_led result=ok

I (...) dispatcher: [CMD_SEND] device=AC:27:6E:CC:F2:26 request_id=8 command=set_led
I (...) device_state: [AC:27:6E:CC:F2:26] feature=led_main prop=1 state updated
I (...) dispatcher: [CMD_ACK] device=AC:27:6E:CC:F2:26 request_id=8 command=set_led result=ok

I (...) dispatcher: [CMD_SEND] device=AC:27:6E:CC:F2:26 request_id=9 command=set_led
I (...) dispatcher: [CMD_ACK] device=AC:27:6E:CC:F2:26 request_id=9 command=set_led result=ok
```

Request `7` và `8` có state update. Request `9` ACK thành công nhưng không có:

```text
device_state: ... state updated
```

Web UI vì vậy có thể giữ state cũ.

---

# 2. Kết luận root cause

Có **hai lỗi độc lập** cần sửa.

## 2.1. Root cause A — Device chỉ publish `feature_state` khi giá trị thay đổi

Trong reference device hiện tại, logic tương đương:

```c
bool changed = s_led_state != new_state;
s_led_state = new_state;

if (publish && changed) {
    device_feature_publish_bool(
        "led_main",
        GW_PROP_ON_OFF,
        s_led_state
    );
}
```

Do đó:

```text
set_led(true), LED đang false
    -> changed=true
    -> feature_state được publish

set_led(true), LED đã true
    -> changed=false
    -> ACK success
    -> không có feature_state
```

ACK command và runtime state event hiện không có quan hệ bắt buộc.

### Hệ quả

```text
CMD_ACK result=ok
```

không đồng nghĩa với:

```text
device_state cache updated
GW_EVENT_FEATURE_STATE emitted
WebSocket feature.state emitted
Web UI updated
```

---

## 2.2. Root cause B — Frontend dùng `/api/devices` cursor để replay cả feature state

Trong Web UI hiện tại, recovery có dạng:

```text
GET /api/devices
    -> X-Gateway-Event-Seq = N
    -> apply device list
    -> events.goLive(N)
    -> replay WS event seq > N
```

Nhưng `/api/devices` không chứa runtime feature state.

Ví dụ:

```text
seq=100  feature.state LED=ON
         event được buffer khi frontend đang sync

GET /api/devices
X-Gateway-Event-Seq: 101

/api/devices không chứa LED state

events.goLive(101)

buffered feature.state seq=100
    -> bị loại vì 100 <= 101
```

Kết quả:

```text
Physical device = ON
Gateway device_state = ON
Web UI = OFF
```

Đây là lỗi **snapshot domain / event cursor ownership**.

---

# 3. Kiến trúc đúng sau khi fix

Target flow:

```text
MCP Tool
   |
   v
Gateway command dispatcher
   |
   v
BLE command
   |
   v
Device applies physical state
   |
   +------------------------------+
   |                              |
   v                              v
ACK chứa authoritative state    Spontaneous feature_state event
   |                              |
   v                              v
Gateway device_state cache <------+
   |
   v
GW_EVENT_FEATURE_STATE
   |
   v
WebSocket /ws/events
   |
   v
Web UI feature cache
   |
   v
Device Detail controls
```

REST vẫn là authoritative recovery snapshot.

WebSocket vẫn là delta/invalidation transport.

Không thêm polling loop.

---

# 4. Fix P0 — Device trả authoritative feature state trong ACK

## 4.1. Mục tiêu

Mỗi writable semantic feature command thành công phải có khả năng trả runtime state mới trong ACK.

Protocol hiện đã có field phù hợp:

```c
bool has_feature_value_bool;
bool feature_value_bool;
uint8_t feature_property_id;
char feature_id[GW_FEATURE_ID_LEN];
```

Không cần tạo protocol mới.

## 4.2. Sửa `cmd_set_led_handler()`

File dự kiến:

```text
devices/reference_device/main/reference_product.c
```

Hiện tại:

```c
response->success = true;
response->int_value = new_state ? 1 : 0;
```

Sửa thành:

```c
response->success = true;
response->int_value = s_led_state ? 1 : 0;

response->has_feature_value_bool = true;
response->feature_value_bool = s_led_state;
response->feature_property_id = GW_PROP_ON_OFF;

strlcpy(
    response->feature_id,
    "led_main",
    sizeof(response->feature_id)
);
```

### Yêu cầu

ACK phải trả state thực tế sau khi apply hardware:

```c
response->feature_value_bool = s_led_state;
```

Không dùng trực tiếp `new_state` nếu hardware apply có thể normalize hoặc fail.

---

# 5. Không dùng ACK `bool_value` làm feature state

Trong protocol hiện tại:

```text
ACK.bool_value
```

mang ý nghĩa:

```text
command success / rejected
```

Không được diễn giải thành LED ON/OFF.

Phải dùng:

```text
has_feature_value_bool
feature_value_bool
feature_id
property_id
```

Target ACK:

```text
type=device_ack
request_id=123
command=set_led
bool_value=true                # success
feature_id=led_main
property_id=1
feature_value_bool=true        # actual LED state
has_feature_value_bool=true
```

---

# 6. Fix P0 — Gateway apply feature state từ ACK

## 6.1. Vấn đề hiện tại

Gateway notify routing:

```text
BLE notify
   |
   v
on_device_notify()
   |
   +--> device_schema_on_notify()
   +--> device_state_on_notify()
   +--> command_dispatcher_on_device_notify()
```

`device_state_on_notify()` hiện chỉ xử lý:

```text
type=device_event
command=feature_state
```

ACK dù có structured feature value vẫn không update cache.

## 6.2. Không được swallow ACK

ACK vẫn phải tới:

```c
command_dispatcher_on_device_notify()
```

để:

```text
device_request_complete()
```

unblock MCP / REST command caller.

Vì vậy không sửa theo kiểu:

```c
if (device_state_on_ack(...)) return;
```

Target phải là:

```c
static void on_device_notify(
    const char *device_id,
    const gw_message_t *msg
)
{
    if (device_schema_on_notify(device_id, msg)) {
        return;
    }

    if (device_state_on_notify(device_id, msg)) {
        return;
    }

    device_state_on_command_ack(device_id, msg);

    command_dispatcher_on_device_notify(device_id, msg);
}
```

`device_state_on_command_ack()` là observer, không consume ACK.

---

# 7. Thêm API `device_state_on_command_ack()`

File:

```text
components/device_state/device_state.c
components/device_state/include/device_state.h
```

Suggested contract:

```c
void device_state_on_command_ack(
    const char *device_id,
    const gw_message_t *msg
);
```

Pseudo implementation:

```c
void device_state_on_command_ack(
    const char *device_id,
    const gw_message_t *msg
)
{
    if (device_id == NULL || msg == NULL) {
        return;
    }

    if (strcmp(msg->type, "device_ack") != 0) {
        return;
    }

    if (!msg->bool_value) {
        return;
    }

    if (!msg->has_feature_id ||
        !msg->has_property_id) {
        return;
    }

    if (!msg->has_feature_value_bool &&
        !msg->has_feature_value_int) {
        return;
    }

    /* Reuse common state-apply helper. */
    device_state_apply_feature_value(
        device_id,
        msg->feature_id,
        msg->property_id,
        msg
    );
}
```

---

# 8. Refactor `device_state` để event và ACK dùng chung apply path

Không nên copy/paste logic.

Tách private helper:

```c
static bool apply_feature_state(
    const char *device_id,
    const char *feature_id,
    uint8_t property_id,
    bool has_bool,
    bool bool_value,
    bool has_int,
    int32_t int_value
);
```

Flow:

```text
device_event feature_state
       |
       v
apply_feature_state()
       |
       +--> update cache
       +--> updated_at_ms
       +--> publish GW_EVENT_FEATURE_STATE

device_ack structured state
       |
       v
apply_feature_state()
       |
       +--> update cache
       +--> updated_at_ms
       +--> publish GW_EVENT_FEATURE_STATE
```

## 8.1. Duplicate event policy

Nếu device gửi:

```text
ACK with feature state
```

và ngay sau đó cũng gửi:

```text
device_event feature_state
```

gateway có thể nhận hai update giống nhau.

Khuyến nghị:

- Cho phép duplicate semantic state ở backend.
- Mỗi apply tạo sequence mới.
- Frontend chỉ render state cuối cùng.
- Không thêm heap allocation hay complicated dedupe table.

Nếu muốn tối ưu sau này, có thể skip event khi cùng device/feature/property/value trong khoảng rất ngắn, nhưng không cần cho bản fix đầu tiên.

---

# 9. Device event policy sau fix

## Option A — Khuyến nghị

ACK luôn trả authoritative state.

Device spontaneous event chỉ publish khi state thực sự thay đổi.

```text
command path:
    authoritative ACK

local/button/physical path:
    feature_state event
```

Ưu điểm:

- Không spam BLE event.
- Idempotent command vẫn đồng bộ được.
- MCP / REST command caller đều có behavior giống nhau.

## Option B

Luôn publish `feature_state` sau mọi successful `set_led`.

Dễ sửa nhưng tạo event thừa.

Không ưu tiên nếu ACK structured state đã tồn tại.

---

# 10. Fix P0 — Frontend recovery phải phục hồi selected feature state

## 10.1. Sai hiện tại

Không được dùng:

```text
/api/devices snapshot
```

như complete baseline cho:

```text
device.connection
device.changed
device.schema
feature.state
```

vì `/api/devices` chỉ authoritative cho device list/connectivity metadata.

## 10.2. Recovery đúng

Khi Device Detail đang mở:

```text
WS CONNECTED / RESYNC REQUIRED
    |
    v
BUFFER EVENTS
    |
    +--> GET /api/devices
    |
    +--> GET /api/devices/schema?device_id=X
    |
    v
apply devices snapshot
apply schema + feature state snapshot
    |
    v
replay WS delta newer than relevant baseline
    |
    v
LIVE
```

---

# 11. Dùng `getDeviceSchemaSnapshot()`

API frontend hiện đã có method dạng:

```js
async getDeviceSchemaSnapshot(deviceId) {
    const { data, eventSeq } = await this.requestWithMeta(
        `/api/devices/schema?device_id=${encodeURIComponent(deviceId)}`
    );

    return {
        schema: data,
        eventSeq
    };
}
```

Không tiếp tục dùng:

```js
api.getDeviceSchema(deviceId)
```

ở code path cần realtime recovery.

---

# 12. Thêm seq vào feature cache

Target:

```js
featureMap.set(key, {
    seq: ev.seq,
    valueType: ev.valueType,
    value: ev.value,
    updatedAtMs: ev.updatedAtMs
});
```

Key:

```js
const key = `${ev.featureId}:${ev.propertyId}`;
```

---

# 13. Merge schema snapshot với buffered feature events

Ví dụ:

```text
schema snapshot cursor = 200

feature.state seq=199
    -> snapshot đã cover
    -> ignore

feature.state seq=201
    -> xảy ra sau snapshot
    -> apply

feature.state seq=202
    -> apply
```

Suggested helper:

```js
_applyFeatureEventToCurrentFeatures(ev) {
    for (const feature of this.currentFeatures) {
        if (
            feature.feature_id === ev.featureId &&
            feature.property_id === ev.propertyId
        ) {
            if (!feature.state) {
                feature.state = {};
            }

            feature.state.valid = true;
            feature.state.updated_at_ms = ev.updatedAtMs;

            if (ev.valueType === 'bool') {
                feature.state.value_bool = ev.value;
            } else if (ev.valueType === 'int') {
                feature.state.value_int = ev.value;
            }

            return true;
        }
    }

    return false;
}
```

---

# 14. Tránh một global cursor cho nhiều snapshot domain

Không nên coi:

```text
X-Gateway-Event-Seq của /api/devices
```

là baseline đầy đủ cho runtime feature state.

## Strategy A — Khuyến nghị cho code hiện tại

Giữ global WS sequence để detect gap.

Nhưng khi resync selected Device Detail phải được reload từ schema/state snapshot trước khi LIVE.

Sau snapshot:

```text
replay buffered events > snapshot cursor
```

theo domain đang recover.

## Strategy B — Phức tạp hơn

Tách per-domain sequence cursors:

```text
deviceListSeq
schemaSeqByDevice
featureStateSeqByDevice
```

Không cần thiết ở thời điểm này nếu Strategy A được triển khai đúng.

---

# 15. Suggested frontend sync flow

Pseudo-code:

```js
async _syncFromSnapshot(reason) {
    if (this._syncPromise) {
        this._syncRequested = true;
        return;
    }

    this._syncRequested = false;

    const doSync = async () => {
        const selectedId =
            state.selectedDeviceDetail?.id ?? null;

        const devicesSnapshot =
            await api.getDevicesSnapshot();

        let schemaSnapshot = null;

        if (selectedId) {
            try {
                schemaSnapshot =
                    await api.getDeviceSchemaSnapshot(selectedId);
            } catch (_) {
                schemaSnapshot = null;
            }
        }

        this._applyDeviceSnapshot(
            devicesSnapshot.devices
        );

        if (
            schemaSnapshot &&
            state.selectedDeviceDetail?.id === selectedId
        ) {
            this._applySchemaSnapshot(
                schemaSnapshot.schema
            );
        }

        events.goLive(devicesSnapshot.eventSeq);

        this._reconcileFeatureCacheAfterSnapshot(
            selectedId,
            schemaSnapshot?.eventSeq ?? 0
        );

        ui.setRealtimeBanner('hide');
    };

    this._syncPromise = doSync();

    try {
        await this._syncPromise;
    } finally {
        this._syncPromise = null;

        if (this._syncRequested) {
            void this._syncFromSnapshot('queued');
        }
    }
}
```

Implementation có thể khác tùy state machine cuối cùng, nhưng invariant bắt buộc là:

> Không chuyển UI sang authoritative LIVE trước khi selected device runtime feature state đã có baseline hợp lệ.

---

# 16. Fix P1 — `loadSchema()` sử dụng metadata cursor

Thay:

```js
const snapshot =
    await api.getDeviceSchema(device.id);
```

bằng:

```js
const result =
    await api.getDeviceSchemaSnapshot(device.id);

const snapshot = result.schema;
const snapshotSeq = result.eventSeq;
```

Sau đó:

```text
apply snapshot
overlay cached/buffered feature events có seq > snapshotSeq
render
```

---

# 17. Fix P1 — Queue work recovery

Trong WebSocket backend, nếu:

```c
httpd_queue_work(...)
```

fail, không được chỉ set:

```c
resync_required = true;
work_pending = false;
```

và chờ event khác.

Recovery phải được bảo đảm.

## Suggested bounded retry

Không retry busy loop.

Có thể dùng một pending recovery flag và retry ở một safe scheduled point.

Invariant:

```text
queue_work_failed
    -> resync_required=true
    -> một drain/recovery work mới phải được schedule
```

không phụ thuộc event domain tiếp theo.

---

# 18. Observability cần giữ

`/api/status` nên dùng để debug realtime:

```json
{
  "websocket": {
    "active_clients": 1,
    "max_clients": 2,
    "ring_used": 0,
    "ring_depth": 32,
    "resync_pending": false,
    "resync_total": 0,
    "send_error_total": 0,
    "connect_total": 1,
    "disconnect_total": 0
  }
}
```

Nếu gateway có:

```text
device_state ... state updated
```

nhưng browser không nhận frame, kiểm tra:

```text
active_clients
send_error_total
resync_pending
resync_total
```

---

# 19. Logging nên bổ sung

## Device

Sau handler:

```text
[STATE_ACK] feature=led_main prop=1 value=true
```

## Gateway

Khi apply structured ACK:

```text
device_state: [MAC] feature=led_main prop=1 source=ack state updated
```

Khi apply spontaneous event:

```text
device_state: [MAC] feature=led_main prop=1 source=event state updated
```

## WebSocket

Không log mỗi event ở INFO trong production.

Có thể dùng DEBUG:

```text
ws event seq=123 type=feature.state fd_count=1
```

---

# 20. Test plan bắt buộc

## T01 — MCP changes OFF → ON

Initial:

```text
LED=false
UI=false
```

Call:

```text
MCP set_led(true)
```

Expected:

```text
ACK success
gateway device_state=true
WS feature.state=true
UI=true
```

## T02 — MCP idempotent ON → ON

Initial:

```text
LED=true
UI=true
```

Call:

```text
MCP set_led(true)
```

Expected:

```text
ACK success
ACK contains feature_value_bool=true
gateway state remains true
UI remains true
no stale state
```

Không yêu cầu spontaneous device event.

## T03 — MCP OFF → ON nhưng browser reconnect đúng lúc command chạy

Expected sau recovery:

```text
UI=true
```

Không được phụ thuộc refresh page.

## T04 — Local button/device-side change

Không có MCP command.

Expected:

```text
device spontaneous feature_state
gateway device_state update
WS event
UI update
```

## T05 — REST command path

Call:

```text
POST /api/command set_led
```

Expected giống MCP.

Không được có behavior riêng cho MCP.

## T06 — MCP command rejected

Expected:

```text
ACK success=false
do not mutate device_state
do not emit fake feature.state
```

## T07 — ACK structured state + spontaneous event duplicate

Expected:

```text
gateway remains correct
frontend remains correct
no crash
no sequence corruption
```

## T08 — WebSocket unavailable

Run command while WS disconnected.

Expected:

```text
gateway device_state updated
browser reconnect
schema/state snapshot restores correct value
UI converges
```

## T09 — Browser refresh after MCP command

Expected schema API returns:

```json
{
  "feature_id": "led_main",
  "state": {
    "valid": true,
    "value_bool": true
  }
}
```

## T10 — Same-value command without spontaneous event

Explicitly verify device không cần phát spontaneous `feature_state`, nhưng gateway/UI vẫn hội tụ từ structured ACK.

---

# 21. Browser validation checklist

Open:

```text
DevTools
  -> Network
  -> /ws/events
  -> Messages / Frames
```

Expected event:

```json
{
  "seq": 123,
  "type": "feature.state",
  "deviceId": "AC:27:6E:CC:F2:26",
  "featureId": "led_main",
  "propertyId": 1,
  "valueType": "bool",
  "value": true,
  "updatedAtMs": 123456
}
```

Nếu gateway log có:

```text
device_state ... state updated
```

nhưng frame không xuất hiện:

```text
bug = gateway event -> WS transport
```

Nếu frame xuất hiện nhưng UI không đổi:

```text
bug = events.js / devices.js state application
```

Nếu không có `device_state updated`:

```text
bug = device notify / structured ACK / state apply
```

---

# 22. Files dự kiến cần sửa

## Device repo

```text
devices/reference_device/main/reference_product.c
components/device_command/device_command.c
components/device_command/include/device_command.h
test/host/test_device_feature.c
test/host/test_device_protocol.c
test/host/test_gateway_interop.c
```

`device_command.c/.h` có thể chỉ cần test nếu structured ACK support hiện đã đủ.

## Gateway repo

```text
main/main.c

components/device_state/device_state.c
components/device_state/include/device_state.h
components/device_state/test/test_device_state.c

components/web_server/web_event_ws.c
components/web_server/test/test_event_ws.c

components/web_server/www_src/dashboard/js/core/events.js
components/web_server/www_src/dashboard/js/core/state.js
components/web_server/www_src/dashboard/js/core/api.js
components/web_server/www_src/dashboard/js/features/devices.js
```

---

# 23. Suggested commit split

## Device

```text
fix(device): include authoritative feature state in command ack
```

```text
test(device): cover idempotent feature command ack state
```

## Gateway

```text
fix(state): apply structured command ack feature state
```

```text
fix(webui): recover selected feature state across websocket resync
```

```text
fix(ws): guarantee resync recovery after queue work failure
```

```text
test(realtime): cover MCP command to websocket state convergence
```

---

# 24. Không nên làm

Không sửa bằng:

```text
MCP success
    -> browser polling every 500 ms
```

Không sửa bằng:

```text
MCP caller trực tiếp gọi WebSocket publish
```

Không sửa bằng optimistic UI toggle không có authoritative state.

Không tạo một MCP-specific state path riêng.

Không dùng:

```text
ACK.bool_value
```

làm feature value.

Không dùng `/api/devices` như full baseline cho feature runtime state.

---

# 25. Memory / ESP32-S3 constraints

Giữ nguyên các nguyên tắc:

- Không malloc trong BLE notify callback.
- Không cJSON trong device-state realtime callback.
- Reuse fixed-size structs.
- WebSocket ring bounded.
- Không tạo task riêng per WS client.
- Không thêm polling task.
- Không tạo per-feature dynamic heap cache nếu fixed table hiện đủ.
- Event serialization bounded.
- ACK state application dùng cùng fixed `device_state` table.

---

# 26. Definition of Done

## Device

- [ ] `set_led` successful ACK chứa authoritative semantic feature state.
- [ ] Same-value idempotent command vẫn trả state.
- [ ] Spontaneous physical/local change vẫn emit feature event.
- [ ] Rejected command không emit fake state.

## Gateway state

- [ ] Structured ACK update `device_state`.
- [ ] Structured ACK publish `GW_EVENT_FEATURE_STATE`.
- [ ] ACK vẫn complete pending request.
- [ ] Không tạo MCP-specific state logic.
- [ ] REST và MCP command path có behavior giống nhau.

## WebSocket

- [ ] `feature.state` serialize đúng.
- [ ] Browser đang connected nhận event.
- [ ] Reconnect/resync không làm mất final feature state.
- [ ] Queue-work failure không để recovery pending vô hạn.

## Web UI

- [ ] Feature cache lưu event seq.
- [ ] Selected device detail có authoritative schema/state snapshot khi recovery.
- [ ] Buffered feature event mới hơn snapshot được overlay.
- [ ] UI không cần manual refresh.
- [ ] Idempotent MCP call không làm UI stale.

## Tests

- [ ] OFF → ON.
- [ ] ON → ON.
- [ ] ON → OFF.
- [ ] MCP path.
- [ ] REST path.
- [ ] Browser reconnect during command.
- [ ] WS disconnect then reconnect.
- [ ] Local device-side state change.
- [ ] Duplicate ACK + event.
- [ ] Rejected command.

---

# 27. Final expected behavior

Sau fix:

```text
User / AI Agent calls MCP tool
        |
        v
Gateway sends BLE set_led
        |
        v
Device applies actual state
        |
        v
Device ACK returns authoritative feature value
        |
        v
Gateway updates device_state
        |
        v
Gateway emits GW_EVENT_FEATURE_STATE
        |
        v
WebSocket sends feature.state
        |
        v
Web UI updates immediately
```

Nếu WebSocket bị mất:

```text
Gateway device_state vẫn đúng
        |
        v
Browser reconnect
        |
        v
REST schema/state snapshot
        |
        v
Replay newer WS events
        |
        v
Web UI converges to correct state
```

---

# 28. Recommended implementation order

1. Device `set_led` structured ACK state.
2. Gateway ACK → `device_state` shared apply helper.
3. Verify MCP idempotent command.
4. Add backend tests.
5. Change frontend schema loading to metadata snapshot.
6. Add seq to feature cache.
7. Fix selected-device resync baseline.
8. Add reconnect-during-command test.
9. Harden `queue_work_failed` recovery.
10. Run soak / repeated MCP commands.

---

## Final design rule

> Command success, runtime state và UI state phải hội tụ qua cùng một authoritative state model, bất kể command bắt nguồn từ Web UI, REST, MCP hay Xiaozhi MCP.

Web UI không được phụ thuộc vào origin của command. Nó chỉ consume authoritative runtime state từ gateway snapshot + realtime event stream.
