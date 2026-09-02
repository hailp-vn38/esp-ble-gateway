# Plan: MCP → WebSocket Device State Sync Fix

**Date:** 2026-09-02
**Reference:** docs/ESP32_GATEWAY_MCP_WS_DEVICE_STATE_SYNC_FIX_GUIDE.md

---

## Problem

Khi MCP tool call thành công (ACK ok) nhưng Web UI không cập nhật state vì:

1. **Device chỉ publish `feature_state` khi giá trị thay đổi** — `set_led(true)` khi LED đã true → ACK ok nhưng không có `feature_state` event → gateway cache không update → WS không gửi → UI cũ.
2. **Frontend recovery dùng sai baseline** — `/api/devices` không chứa runtime feature state, nhưng được dùng làm cursor replay → bỏ lọt feature state events.

---

## Scope

Gateway repo fix only (device firmware changes are out of scope —假设 device ACK đã có structured feature state).

---

## Phase 1: Gateway — ACK → device_state update

### 1.1 Tách `apply_feature_state()` helper

**File:** `components/device_state/device_state.c`

Tách logic update cache + publish event từ `device_state_on_notify()` thành static helper `apply_feature_state()` để cả event và ACK dùng chung.

```
apply_feature_state(device_id, feature_id, property_id, has_bool, bool_val, has_int, int_val)
  → find/allocate entry
  → update value_bool / value_int / valid / updated_at_ms
  → gateway_events_publish(GW_EVENT_FEATURE_STATE)
  → log "[source=ack|event] state updated"
```

### 1.2 Refactor `device_state_on_notify()` dùng helper

**File:** `components/device_state/device_state.c`

- Lines 114-176: Đổi sang gọi `apply_feature_state()` thay vì inline logic.

### 1.3 Thêm `device_state_on_command_ack()` observer

**File:** `components/device_state/device_state.c`, `include/device_state.h`

```c
void device_state_on_command_ack(const char *device_id, const gw_message_t *msg);
```

Logic:
- Kiểm tra `msg->type == "device_ack"` && `msg->bool_value == true` (accepted)
- Kiểm tra `msg->has_feature_id` && `msg->has_property_id`
- Kiểm tra `msg->has_feature_value_bool || msg->has_feature_value_int`
- Gọi `apply_feature_state()` với source = "ack"

### 1.4 Sửa `on_device_notify()` trong main.c

**File:** `main/main.c` lines 24-29

```c
static void on_device_notify(const char *device_id, const gw_message_t *msg)
{
    if (device_schema_on_notify(device_id, msg)) return;
    if (device_state_on_notify(device_id, msg)) return;
    device_state_on_command_ack(device_id, msg);    // observer, không consume
    command_dispatcher_on_device_notify(device_id, msg);
}
```

`device_state_on_command_ack()` là observer — ACK vẫn tới `command_dispatcher_on_device_notify()` để `device_request_complete()` unblock caller.

### 1.5 Logging

- `apply_feature_state()` log: `device_state: [%s] feature=%s prop=%u source=%s state updated` với source = "ack" hoặc "event"

---

## Phase 2: Frontend — Schema snapshot với cursor

### 2.1 Sửa `_syncFromSnapshot()`

**File:** `components/web_server/www_src/dashboard/js/features/devices.js`

Khi Device Detail đang mở, lấy thêm schema snapshot trước khi goLive:

```js
async _syncFromSnapshot(reason) {
    const selectedId = state.selectedDeviceDetail?.id ?? null;
    const snapshot = await api.getDevicesSnapshot();
    let schemaSnapshot = null;
    if (selectedId) {
        try {
            schemaSnapshot = await api.getDeviceSchemaSnapshot(selectedId);
        } catch (_) {}
    }
    this._applyDeviceSnapshot(snapshot.devices);
    if (schemaSnapshot && state.selectedDeviceDetail?.id === selectedId) {
        this._applySchemaSnapshot(schemaSnapshot.schema);
    }
    events.goLive(snapshot.eventSeq);
    this._reconcileFeatureCacheAfterSnapshot(selectedId, schemaSnapshot?.eventSeq ?? 0);
    ui.setRealtimeBanner('hide');
}
```

### 2.2 Thêm `_applySchemaSnapshot()`

**File:** `devices.js`

Áp dụng schema features + state từ REST snapshot vào `this.currentFeatures`.

### 2.3 Thêm `_reconcileFeatureCacheAfterSnapshot()`

**File:** `devices.js`

Replay buffered feature events có seq > snapshotSeq lên feature cache.

### 2.4 Sửa `loadSchema()` dùng cursor

**File:** `devices.js`

Thay `api.getDeviceSchema()` bằng `api.getDeviceSchemaSnapshot()` để lấy eventSeq, sau đó overlay buffered events.

### 2.5 Thêm seq vào feature cache

**File:** `devices.js` — `_handleFeatureStateEvent()`

```js
featureMap.set(key, {
    seq: ev.seq,
    valueType: ev.valueType,
    value: ev.value,
    updatedAtMs: ev.updatedAtMs
});
```

---

## Phase 3: Hardening

### 3.1 queue_work_failed recovery

**File:** `components/web_server/web_event_ws.c`

Hiện tại khi `httpd_queue_work` fail, chỉ set `resync_required=true` và chờ event tiếp theo. Cần schedule retry độc lập.

---

## Files cần sửa

### Gateway
| File | Thay đổi |
|------|----------|
| `components/device_state/device_state.c` | Tách helper, thêm `device_state_on_command_ack()` |
| `components/device_state/include/device_state.h` | Thêm declaration |
| `main/main.c` | Gọi `device_state_on_command_ack()` trong dispatch chain |
| `components/web_server/web_event_ws.c` | queue_work recovery |

### Frontend
| File | Thay đổi |
|------|----------|
| `components/web_server/www_src/dashboard/js/features/devices.js` | Schema snapshot sync, feature cache seq, reconcile |

---

## Verification

- [ ] `idf.py build` thành công
- [ ] MCP `set_led(true)` khi LED đã true → gateway log có `source=ack state updated` → WS event `feature.state` → UI cập nhật
- [ ] MCP `set_led(true)` khi LED false → cả ACK và event đều update state
- [ ] Browser reconnect → schema snapshot restore feature state → UI đúng
- [ ] REST `POST /api/command` có behavior giống MCP
