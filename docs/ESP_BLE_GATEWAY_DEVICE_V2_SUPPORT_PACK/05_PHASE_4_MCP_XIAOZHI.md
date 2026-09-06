# Phase 4 — MCP / Xiaozhi Support

## 1. Tổng quan

Gateway hiện dùng static MCP tools:

```text
get_status
list_devices
device_control
```

Giữ nguyên mô hình này.

Không tạo MCP tool riêng cho mỗi command.

---

## 2. Cần thêm / sửa gì

### 2.1. Template dependency

`mcp_sem_serialize_feature()` hiện skip feature nếu:

```text
device_template_resolve(...) == NULL
```

Vì vậy `GENERIC_VALUE` bắt buộc có template trước MCP phase.

---

### 2.2. Feature metadata serialization

Target `mcp_sem_serialize_feature()`:

```json
{
  "feature_id": "dryer_temperature",
  "title": "Nhiệt độ sấy",
  "semantic_name": "value",
  "type": "value",
  "property": "value",
  "value_type": "int",
  "unit": "°C",
  "decimals": 1,
  "writable": true,
  "minimum": 300,
  "maximum": 1000,
  "step": 5
}
```

Range raw.

---

### 2.3. Feature resolution

Keep priority:

```text
1 exact feature_id
2 semantic name if unique
```

Do not resolve by title.

Generic features all có thể semantic_name `"value"` nên fallback này thường ambiguous.

Expected agent path:

```text
list_devices -> feature_id
device_control -> exact feature_id
```

---

### 2.4. Read

Current read uses:

```text
device_template_property_value_type(property_id)
```

Sau khi:

```text
VALUE -> INT
```

generic read hoạt động.

Read response nên thêm:

```text
title
unit
decimals
raw value
```

Optional:

```text
display_value
```

---

### 2.5. Set

Current `device_control` typed contract:

```text
bool_value
int_value
```

MVP giữ raw INT.

Example:

```json
{
  "operation": "set",
  "device": "device-id",
  "feature": "dryer_temperature",
  "int_value": 655
}
```

Gateway schema validation kiểm tra tool range.

---

### 2.6. Human-scale input optional

Sau MVP có thể thêm:

```text
number_value
```

Example:

```text
65.5 -> raw 655
```

Rules:

- mutually exclusive với int_value;
- only numeric feature;
- Gateway convert bằng decimals;
- BLE vẫn INT.

---

### 2.7. Completion / authoritative state

Current completion chủ yếu trả:

```text
success/error
```

V2 khuyến nghị sau successful set:

1. lấy updated `device_state` cho feature;
2. trả actual raw value;
3. trả decimals/unit;
4. optionally formatted value.

Example:

```json
{
  "success": true,
  "feature_id": "dryer_temperature",
  "value_raw": 650,
  "decimals": 1,
  "unit": "°C",
  "value_display": 65.0
}
```

Điều này quan trọng khi device clamp requested value.

---

## 3. Sửa ở đâu

| File | Thay đổi |
|---|---|
| `components/mcp_endpoint/mcp_semantic_control.c` | serialize title/unit/decimals/range |
| `components/mcp_endpoint/mcp_device_control.c` | read/set result metadata |
| `components/mcp_endpoint/mcp_registry.c` | description/schema nếu thêm number_value |
| `components/mcp_endpoint/mcp_endpoint_internal.h` | struct nếu cần |
| MCP tests | generic describe/read/set |
| `components/device_template/device_template.c` | prerequisite generic template |

---

## 4. Checklist

### Describe/list

- [ ] generic feature visible.
- [ ] title present.
- [ ] unit present.
- [ ] decimals present.
- [ ] raw range present.
- [ ] writable present.

### Resolve

- [ ] exact feature_id works.
- [ ] semantic fallback unique only.
- [ ] generic `"value"` ambiguity returns ambiguous.
- [ ] title not used as identity.

### Read

- [ ] BOOL read.
- [ ] INT read.
- [ ] generic read.
- [ ] metadata returned.

### Set

- [ ] BOOL set.
- [ ] FAN INT set.
- [ ] generic INT set.
- [ ] range reject.
- [ ] step reject.
- [ ] authoritative actual state returned/recoverable.

---

## 5. Test plan

### T4.1 — describe generic

Expected metadata complete.

### T4.2 — read temp

raw 251, decimals1.

Expected enough metadata to interpret 25.1°C.

### T4.3 — set fan

60.

Expected success.

### T4.4 — set dryer raw655

Expected command sent.

### T4.5 — clamp

Requested655 -> ACK650.

Expected MCP result/state shows actual650.

### T4.6 — ambiguous generic semantic

Two GENERIC_VALUE features.

Query feature `"value"`.

Expected ambiguous error.

Exact feature ID works.

### T4.7 — Xiaozhi natural language

```text
"Bật relay chính"
"Đặt quạt 60%"
"Đặt nhiệt độ sấy 65.5 độ"
"Đặt thời gian sấy 30 phút"
"Nhiệt độ hiện tại bao nhiêu?"
```

---

## 6. Exit criteria

- [ ] generic visible in MCP.
- [ ] exact feature identity.
- [ ] read/set generic works.
- [ ] authoritative result meaningful.
- [ ] Xiaozhi E2E pass.
