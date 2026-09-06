# ESP BLE Gateway — Device v2 Support Plan

**Version:** 2.0  
**Date:** 2026-09-06  
**Gateway repo:** `hailp-vn38/esp-ble-gateway`  
**Target branch:** `dev-ws`  
**Baseline commit:** `4ea6f0e36efd727fcf5ed5c93ba373d008baa57a`  
**Compatible device contract:** `ESP_BLE_DEVICE_V2_IMPLEMENTATION_PACK`

---

## 1. Mục tiêu

Tài liệu này chỉ mô tả các thay đổi phía **BLE Gateway** để hỗ trợ device v2 mới:

- 7 public tools;
- 10 semantic features;
- BOOL + INT feature state;
- `GENERIC_VALUE`;
- `GW_PROP_VALUE`;
- feature `title`;
- feature `unit`;
- `decimals`;
- generic writable setpoint;
- authoritative INT state;
- Web UI schema-driven;
- MCP/Xiaozhi qua `device_control`.

---

## 2. Device contract Gateway phải hỗ trợ

### New semantic type

```c
GW_FEATURE_GENERIC_VALUE = 2
```

### New property

```c
GW_PROP_VALUE = 8
```

### New CBOR metadata

```c
CBOR_KEY_FEATURE_DECIMALS = 31
```

### Reused fields trong `feature_item`

```text
capability_label -> feature title
capability_unit  -> feature unit
value_type       -> feature state type
```

### Numeric scale

```text
display = raw / 10^decimals
```

Raw integer áp dụng thống nhất cho:

```text
command int_value
tool min/max/step
state
ACK
event
```

---

## 3. Các vấn đề hiện tại cần sửa

| ID | Vấn đề hiện tại | Tác động |
|---|---|---|
| G2-01 | chưa có `GENERIC_VALUE` / `VALUE` | generic setpoint unsupported |
| G2-02 | template DIMMER đang `ON_OFF` | semantic/UI sai |
| G2-03 | template FAN đang `ON_OFF` | semantic/UI sai |
| G2-04 | feature schema chưa có title/unit/value_type/decimals | UI/MCP thiếu metadata |
| G2-05 | schema feature còn giữ runtime bool/int | duplicate với `device_state` |
| G2-06 | NVS schema version vẫn 1/raw struct | layout migration nguy hiểm |
| G2-07 | initial state seed chỉ BOOL | INT feature ban đầu chưa có state |
| G2-08 | Web API không expose title/unit/decimals | UI không render v2 đúng |
| G2-09 | Web UI hiện hiển thị `feature_id` | không có tên thân thiện |
| G2-10 | Web UI numeric chủ yếu hard-code LEVEL | FAN/VALUE không render generic |
| G2-11 | MCP semantic serializer bỏ feature không có template | generic feature có thể biến mất |
| G2-12 | MCP read/set type dựa vào property template | `VALUE` phải map INT |
| G2-13 | MCP completion chưa ưu tiên actual post-command state | clamp/setpoint UX chưa đầy đủ |

---

## 4. Kiến trúc Gateway v2

```text
BLE notify
   |
   v
cbor_codec
   |
   v
device_schema
   |                 |               -> device_template
   |
   +--> persisted schema v2
   |
   +--> device_state seed
            |
            +--> ACK/event updates
            |
            +--> gateway_events / WebSocket
            |
            +--> Web API
            |
            +--> MCP device_control
```

Ownership:

```text
device_schema_tool_t
  = command/write constraints

device_schema_feature_t
  = semantic/display metadata

device_state_entry_t
  = runtime value
```

---

## 5. Target schema

### Tool

Giữ:

```c
typedef struct {
    device_command_t command;
    char label[GW_MSG_CAP_LABEL_LEN];
    char unit[GW_MSG_CAP_UNIT_LEN];
    uint8_t value_type;
    uint8_t flags;
    int32_t min_value;
    int32_t max_value;
    uint32_t step;
} device_schema_tool_t;
```

### Feature target

```c
typedef struct {
    device_feature_id_t feature_id;
    char title[GW_MSG_CAP_LABEL_LEN];
    char unit[GW_MSG_CAP_UNIT_LEN];

    uint8_t feature_type;
    uint16_t feature_schema_version;
    uint16_t feature_flags;

    uint8_t property_id;
    uint8_t value_type;
    uint8_t decimals;

    int8_t writable_tool_index;
} device_schema_feature_t;
```

Không giữ runtime value trong schema.

---

## 6. Target demo compatibility

Expected device schema:

```text
tools=7
features=10
```

Features:

```text
relay_main
plug_main
light_main
dimmer_main
fan_main
temperature_main
humidity_main
contact_main
dryer_temperature
drying_time
```

---

## 7. Phase map

| Phase | Nội dung |
|---|---|
| 0 | Protocol / CBOR compatibility |
| 1 | Schema / Template / Persistence |
| 2 | Runtime State / Command validation |
| 3 | Web API / Web UI / WebSocket |
| 4 | MCP / Xiaozhi |
| 5 | Integration / Regression / Rollout |

---

## 8. File index

- [Phase 0 — Protocol / CBOR](./01_PHASE_0_PROTOCOL_CBOR.md)
- [Phase 1 — Schema / Template / NVS](./02_PHASE_1_SCHEMA_TEMPLATE_PERSISTENCE.md)
- [Phase 2 — State / Command Pipeline](./03_PHASE_2_STATE_COMMAND_PIPELINE.md)
- [Phase 3 — Web API / UI / Realtime](./04_PHASE_3_WEB_API_UI_REALTIME.md)
- [Phase 4 — MCP / Xiaozhi](./05_PHASE_4_MCP_XIAOZHI.md)
- [Phase 5 — Integration / Release](./06_PHASE_5_INTEGRATION_RELEASE.md)
- [File change matrix](./07_FILE_CHANGE_MATRIX.md)

---

## 9. Rollout order

Khuyến nghị:

```text
1. deploy Gateway v2 support
2. verify old device regression
3. flash Device v2
4. refresh schema
5. verify UI/MCP
```

Không rollout Device v2 trước Gateway v2 nếu cần generic feature hoạt động đầy đủ.

---

## 10. Global Definition of Done

- [x] CBOR decode generic metadata.
- [ ] schema stores title/unit/value_type/decimals.
- [ ] no runtime state duplicated in schema.
- [ ] FAN/PERCENT_SETTING correct.
- [ ] DIMMER/LEVEL correct.
- [ ] GENERIC_VALUE/VALUE correct.
- [ ] NVS schema v2 migration safe.
- [ ] BOOL + INT initial state seed.
- [ ] API exposes generic metadata.
- [ ] UI displays scaled numeric values.
- [ ] WebSocket realtime works.
- [ ] MCP describe/read/set generic feature works.
- [ ] old reference device regression passes.
- [ ] demo 7 tools / 10 features passes.
