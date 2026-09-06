# Phase 1 — Device Schema, Template, Persistence

## 1. Tổng quan

Đây là phase quan trọng nhất phía Gateway.

Gateway hiện:

- feature schema chưa có title/unit/value_type/decimals;
- feature schema vẫn chứa runtime bool/int;
- template FAN/DIMMER đang sai với target device;
- NVS store ghi raw struct layout version 1.

---

## 2. Cần thêm / sửa gì

### 2.1. Compact feature schema

Target:

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

Remove:

```text
feature_value_bool
feature_value_int
```

Runtime value chỉ ở `device_state`.

---

### 2.2. Feature item staging

Trong `handle_feature_item()`:

Required:

```text
snapshot_id
feature_id
feature_type
property_id
value_type
```

Copy:

```text
feature_id <- message.feature_id

title <-
    capability_label nếu non-empty
    else feature_id

unit <- capability_unit

decimals <-
    feature_decimals nếu present
    else 0
```

---

### 2.3. Writable tool mapping

Nếu `feature_tool` present:

1. resolve tool index;
2. reject nếu tool missing;
3. validate tool `value_type == feature.value_type`;
4. nếu cả 2 unit non-empty, require equal;
5. set `writable_tool_index`.

Không copy min/max/step vào feature.

---

### 2.4. Template corrections

Current target phải sửa:

```text
DIMMABLE_LIGHT
  primary_property = LEVEL

FAN
  primary_property = PERCENT_SETTING
```

Thêm:

```text
GENERIC_VALUE
  semantic_name = "value"
  primary_property = VALUE
```

Property registry:

```text
VALUE -> INT
```

---

### 2.5. Semantic validation

Known feature type phải dùng primary property đúng.

Examples:

```text
FAN + PERCENT_SETTING -> valid
FAN + ON_OFF          -> reject v2

DIMMABLE_LIGHT + LEVEL -> valid
GENERIC_VALUE + VALUE  -> valid
```

Temperature:

```text
TEMPERATURE_SENSOR + TEMPERATURE -> valid
```

---

### 2.6. Snapshot equality

Title/unit/value_type/decimals là schema content.

Nếu đổi một trong các field:

```text
snapshot changed
```

Product cũng phải bump capability revision.

---

### 2.7. Persistence migration

Current:

```c
SCHEMA_STORE_SCHEMA_VERSION 1
```

và NVS blob chứa raw struct arrays.

Target:

```c
SCHEMA_STORE_SCHEMA_VERSION 2
```

Load policy:

```text
version=2 + exact size -> load
version=1             -> invalidate/erase
invalid length         -> ignore/erase
corrupt metadata       -> reject
```

Không reinterpret v1 struct bytes bằng v2 layout.

---

## 3. Sửa ở đâu

| File | Thay đổi |
|---|---|
| `components/device_schema/include/device_schema.h` | feature struct v2 |
| `components/device_schema/device_schema_protocol.c` | stage title/unit/type/decimals |
| `components/device_schema/device_schema_validate.c` | feature/tool/template validation |
| `components/device_schema/device_schema_store.c` | store schema version 2 |
| `components/device_schema/device_schema_internal.h` | helper declarations nếu cần |
| `components/device_template/device_template.c` | generic + FAN/DIMMER fix |
| `components/device_template/include/device_template.h` | nếu helper/interface đổi |
| `components/device_schema/test/*` | commit/migration tests |
| `components/device_template/test/*` | semantic tests |

---

## 4. Checklist

### Schema

- [ ] feature title.
- [ ] feature unit.
- [ ] feature value_type.
- [ ] feature decimals.
- [ ] writable_tool_index.
- [ ] no runtime bool/int in schema.
- [ ] duplicate feature ID reject.
- [ ] max 12 features.

### Binding

- [ ] tool exists.
- [ ] type matches.
- [ ] unit matches.
- [ ] read-only has index -1.
- [ ] missing tool rejects staging.

### Template

- [ ] FAN -> PERCENT_SETTING.
- [ ] DIMMER -> LEVEL.
- [ ] GENERIC_VALUE -> VALUE.
- [ ] VALUE -> INT.
- [ ] old light/relay/contact unchanged.

### Persistence

- [ ] version 2.
- [ ] v1 invalidation safe.
- [ ] reboot load v2.
- [ ] corrupt blob safe.
- [ ] no stale v1 struct read.

---

## 5. Test plan

### T1.1 — Generic schema commit

7 tools / 10 features.

Expected READY.

### T1.2 — Title fallback

Feature item empty label.

Expected:

```text
title=feature_id
```

### T1.3 — Type mismatch

INT feature bound BOOL tool.

Expected reject.

### T1.4 — Unit mismatch

Feature °C bound tool rpm.

Expected reject.

### T1.5 — FAN semantic

PERCENT_SETTING valid.

### T1.6 — FAN old ON_OFF

Expected reject under v2 target.

### T1.7 — Persist/reboot

Discover v2 -> reboot -> same schema metadata.

### T1.8 — Old v1 NVS blob

Expected invalidated, no crash.

### T1.9 — Revision change

Change title only + bump revision.

Expected schema changed event.

---

## 6. Exit criteria

- [ ] generic schema persists.
- [ ] templates correct.
- [ ] migration safe.
- [ ] no runtime state duplication.
