# Phase 0 — Protocol / CBOR Compatibility ✅ DONE (2026-09-06)

## 1. Tổng quan

Gateway hiện dùng Protocol v4 và CBOR keys 0..30.

Device v2 vẫn dùng Protocol v4 nhưng thêm semantic extension:

```text
GENERIC_VALUE
VALUE
feature decimals
```

Phase này chỉ làm Gateway hiểu wire contract mới.

---

## 2. Cần thêm / sửa gì

### 2.1. Feature type

Trong shared protocol enum:

```c
GW_FEATURE_GENERIC_VALUE = 2
```

Giữ nguyên các giá trị cũ.

### 2.2. Property

```c
GW_PROP_VALUE = 8
```

### 2.3. CBOR key

```c
CBOR_KEY_FEATURE_DECIMALS = 31
```

Không renumber key 0..30.

### 2.4. Message field

```c
uint8_t feature_decimals;
int has_feature_decimals;
```

### 2.5. Reuse metadata

Không thêm `feature_title`.

Trong `feature_item`:

```text
capability_label = feature title
capability_unit  = feature unit
value_type       = feature value type
```

---

## 3. Encode / decode rules

### Decode

Key 31 optional:

```text
missing -> has_feature_decimals=0
consumer fallback -> decimals=0
```

Validation:

```text
0 <= decimals <= UINT8_MAX
```

Schema layer sẽ giới hạn practical range, ví dụ 0..3.

### Encode

Gateway không nhất thiết emit `feature_item`, nhưng codec roundtrip test vẫn nên hỗ trợ key 31.

---

## 4. Compatibility

### Old Device -> Gateway v2

Expected:

```text
missing decimals -> 0
missing title -> feature_id fallback later
```

### Device v2 -> Gateway v2

Full support.

### Device v2 -> Gateway cũ

Wire có thể decode unknown key, nhưng semantic support không đảm bảo.

---

## 5. Sửa ở đâu

| File | Thay đổi |
|---|---|
| `components/cbor_codec/include/cbor_codec.h` | enum type/property + message decimals |
| `components/cbor_codec/cbor_codec.c` | key 31 encode/decode/JSON |
| `components/cbor_codec/test/*` | codec tests |
| các protocol mirror/header liên quan | giữ enum đồng bộ |

---

## 6. Checklist

- [x] `GENERIC_VALUE == 2`.
- [x] `VALUE == 8`.
- [x] key 31 không conflict.
- [x] decoder optional.
- [x] encoder optional.
- [x] old v4 packet pass.
- [x] INT feature state vẫn decode.
- [x] BOOL feature state vẫn decode.
- [x] unknown future key vẫn tolerated.
- [x] no protocol version bump.

---

## 7. Test plan

### T0.1 — Old feature item

No key 31.

Expected:

```text
decode success
has_feature_decimals=0
```

### T0.2 — Generic feature item

```text
feature_type=2
property_id=8
value_type=INT
decimals=1
```

Expected exact decode.

### T0.3 — INT event

`feature_value_int`.

Expected unchanged.

### T0.4 — BOOL event

Expected unchanged.

### T0.5 — Roundtrip

encode -> decode -> compare.

### T0.6 — Unknown key

Extra key 32.

Expected decoder still accepts if existing targeted-lookup policy supports it.

---

## 8. Exit criteria

- [x] codec tests pass.
- [x] old packet regression pass.
- [x] generic metadata roundtrip pass.
