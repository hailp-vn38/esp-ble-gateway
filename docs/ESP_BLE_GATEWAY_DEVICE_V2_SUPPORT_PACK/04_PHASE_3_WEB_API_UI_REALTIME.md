# Phase 3 — Web API, Web UI, Realtime

## 1. Tổng quan

Gateway hiện có:

```text
/api/devices/schema
/api/devices/detail
WebSocket feature.state
```

Nhưng feature JSON chưa đủ metadata v2 và UI numeric rendering còn hard-code.

---

## 2. Cần thêm / sửa gì

### 2.1. `/api/devices/schema`

Feature JSON target:

```json
{
  "feature_id": "dryer_temperature",
  "title": "Nhiệt độ sấy",
  "feature_type": 2,
  "property_id": 8,
  "value_type": 2,
  "unit": "°C",
  "decimals": 1,
  "writable_tool_index": 5,
  "write_command": "set_dryer_temperature"
}
```

State nên typed:

```json
"state": {
  "valid": true,
  "value_int": 655,
  "updated_at_ms": 123
}
```

Không bắt buộc trả cả bool và int cùng lúc.

---

### 2.2. `/api/devices/detail`

Đây là API UI đang dùng.

Add root feature fields:

```text
title
unit
decimals
value_type
```

Semantic:

```text
name
property
value_type
primary_property
```

Control derive từ tool:

```text
writable
write_command
minimum
maximum
step
```

Range giữ raw.

---

### 2.3. UI display title

Current UI đang render:

```text
feature.feature_id
```

Target:

```js
const title =
    feature.title ||
    feature.semantic?.name ||
    feature.feature_id;
```

Feature ID có thể hiển thị trong advanced/debug, không phải headline.

---

### 2.4. Numeric scaling

Helpers:

```js
function scaleOf(feature) {
    return 10 ** (feature.decimals ?? 0);
}

function rawToDisplay(feature, raw) {
    return raw / scaleOf(feature);
}

function displayToRaw(feature, value) {
    return Math.round(value * scaleOf(feature));
}
```

Examples:

```text
655 + decimals1 -> 65.5
251 + decimals1 -> 25.1
```

---

### 2.5. Generic numeric renderer

Không chỉ:

```text
primary_property == LEVEL
```

Target rule:

```text
BOOL writable
-> toggle

INT writable + valid tool range
-> slider + number input

INT read-only
-> value display

GENERIC_VALUE/VALUE
-> generic INT renderer
```

FAN/PERCENT_SETTING cũng dùng numeric renderer.

---

### 2.6. Control scaling

Tool raw:

```text
min=300
max=1000
step=5
```

UI:

```text
min=30.0
max=100.0
step=0.5
```

User input `65.5`:

```text
POST raw integer 655
```

---

### 2.7. Realtime

WebSocket event không cần title/unit/decimals.

Event:

```text
feature_id
property_id
valueType
value
updatedAt
```

UI lookup schema cache để format.

Optimization:

Hiện có thể rerender toàn list.

V2 target:

```text
update only affected feature card
```

Nếu chưa làm ngay, full list rerender vẫn functional nhưng cần test sensor rate.

---

### 2.8. Backward compatibility

Old device:

```text
title missing -> feature_id
decimals missing -> 0
unit missing -> ""
```

---

## 3. Sửa ở đâu

| File | Thay đổi |
|---|---|
| `components/web_server/web_device_schema_api.c` | add title/unit/decimals/value_type, typed state |
| `components/web_server/web_device_detail_api.c` | expose v2 metadata/control |
| `components/web_server/www_src/dashboard/js/features/devices.js` | title/scaling/generic renderer |
| API JS module nếu mapping khác | propagate fields |
| i18n resources | labels nếu thêm |
| `components/web_server/test/*` | API response tests |

---

## 4. Checklist

### Backend

- [ ] schema API metadata complete.
- [ ] detail API metadata complete.
- [ ] control derives from tool.
- [ ] state derives from `device_state`.
- [ ] typed state JSON.
- [ ] old device fallback.

### UI

- [ ] title display.
- [ ] unit display.
- [ ] decimals display.
- [ ] temperature read-only.
- [ ] dimmer slider.
- [ ] fan slider.
- [ ] generic temperature setpoint.
- [ ] generic drying time.
- [ ] display->raw conversion.
- [ ] authoritative state reflected.

### Realtime

- [ ] feature event updates visible state.
- [ ] no page refresh.
- [ ] schema event reloads metadata.
- [ ] state event does not reload schema.
- [ ] reconnect snapshot reconcile.

---

## 5. Test plan

### T3.1 — API generic feature

Expected fields exact.

### T3.2 — Vietnamese title

`Nhiệt độ sấy`.

Expected UTF-8 display correct.

### T3.3 — Temperature state

raw=251, decimals=1.

Expected UI 25.1°C.

### T3.4 — Setpoint control

raw range 300..1000 step5.

Expected UI 30.0..100.0 step0.5.

### T3.5 — Fan

PERCENT_SETTING.

Expected slider.

### T3.6 — Dimmer

LEVEL.

Expected slider.

### T3.7 — ACK clamp

Request display 65.5.

Device actual raw650.

Expected UI 65.0.

### T3.8 — Local event

Relay state changes without refresh.

### T3.9 — Sensor stream

Repeated INT events.

Expected no schema reload.

### T3.10 — Old reference device

Expected still renders.

---

## 6. Exit criteria

- [ ] API metadata complete.
- [ ] UI schema-driven.
- [ ] generic numeric control works.
- [ ] realtime works.
- [ ] old device regression works.
