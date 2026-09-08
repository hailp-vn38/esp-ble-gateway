# Phase G4 — Settings Schema Discovery ✅ DONE (2026-09-07)

**Document set:** ESP32 BLE Gateway ↔ Device Settings v2
**Version:** v2.1 phased documents
**Date:** 2026-09-07
**Gateway repo:** `hailp-vn38/esp-ble-gateway`
**Gateway branch:** `dev-device-settings`
**Device repo:** `hailp-vn38/esp-ble-device`
**Device branch:** `main`
**Protocol:** ESP-GATT Protocol v4

---

## Protocol constants used by this phase

Canonical Settings keys:

```c
GW_KEY_SETTINGS_SUPPORTED          = 32,
GW_KEY_SETTINGS_SCHEMA_REVISION    = 33,
GW_KEY_SETTINGS_ID                 = 34,
GW_KEY_SETTINGS_TITLE              = 35,
GW_KEY_SETTINGS_GROUP              = 36,
GW_KEY_SETTINGS_UNIT               = 37,
GW_KEY_SETTINGS_TYPE               = 38,
GW_KEY_SETTINGS_FLAGS              = 39,
GW_KEY_SETTINGS_VALUE              = 40,
GW_KEY_SETTINGS_TRANSACTION_ID     = 41,
GW_KEY_SETTINGS_EXPECTED_REVISION  = 42,
GW_KEY_SETTINGS_NEW_REVISION       = 43,
GW_KEY_SETTINGS_OPTION_INDEX       = 44,
GW_KEY_SETTINGS_MAX_LENGTH         = 45,
GW_KEY_SETTINGS_OPTION_COUNT       = 46,
GW_KEY_SETTINGS_SEQUENCE           = 47,
```

Canonical Settings wire types:

```c
GW_SETTING_TYPE_NONE   = 0,
GW_SETTING_TYPE_BOOL   = 1,
GW_SETTING_TYPE_INT    = 2,
GW_SETTING_TYPE_FLOAT  = 3,
GW_SETTING_TYPE_STRING = 4,
GW_SETTING_TYPE_ENUM   = 5,
```

Canonical Settings flags:

```c
GW_SETTING_FLAG_READONLY = 1 << 0
GW_SETTING_FLAG_SECRET   = 1 << 1
GW_SETTING_FLAG_ADVANCED = 1 << 2
```

Do not create a second private key table in Gateway or Device.


# 1. Objective

Decode and atomically commit Device Settings descriptors, metadata, and enum options.

# 2. Files

```text
components/device_settings/device_settings_protocol.c
components/device_settings/device_settings_memory.c
components/device_settings/device_settings.c
components/device_settings/include/device_settings.h
```

# 3. Accepted schema stream

```text
settings_begin
settings_item*
settings_option_item*
settings_end
ACK(describe_settings)
```

# 4. `settings_begin`

Require:

```text
protocol_version=4
type=settings_begin
command=describe_settings
request_id
total
```

Do not require:

```text
device_id
snapshot_id
capability_revision
```

BLE callback `device_id` is routing authority.

# 5. `settings_item`

Require:

```text
request_id
total
settings_sequence
setting_id
setting_type
```

Optional metadata:

```text
title
group
unit
flags
max_length
min
max
step
```

Fallback:

```text
title absent/empty -> setting_id
```

# 6. Descriptor storage

Recommended descriptor stores string-pool offsets:

```c
typedef struct {
    uint16_t id_off;
    uint16_t title_off;
    uint16_t group_off;
    uint16_t unit_off;

    uint8_t type;
    uint16_t flags;

    int32_t min_value;
    int32_t max_value;
    uint32_t step;
    uint16_t max_length;

    uint16_t option_start;
    uint8_t option_count;
} ds_setting_descriptor_t;
```

# 7. Wire flag translation

Do not copy raw wire flags.

Translate:

```text
wire READONLY set   -> internal WRITABLE clear
wire READONLY clear -> internal WRITABLE set
SECRET              -> internal SECRET
ADVANCED            -> internal ADVANCED
```

# 8. Enum options

Current Device emits:

```text
settings_sequence = parent descriptor index
settings_option_index = option value/index
settings_title = option label
```

Store:

```c
typedef struct {
    uint8_t value;
    uint16_t label_off;
} ds_enum_option_t;
```

Validate:

```text
parent exists
parent type == ENUM
option index unique
option count bounded
```

# 9. Stream integrity

Validate:

```text
same BLE device
same active request_id
sequence contiguous
no duplicate setting ID
item count <= total
end total == begin total
received count == total
```

# 10. Atomic commit

Use builder → immutable committed schema swap.

Failure:

```text
discard builder
retain previous committed schema
```

Never expose partially received schema.

# 11. Memory

Prefer PSRAM for immutable committed schema/string pool when available.

Keep worker control objects in internal RAM.

Do not allocate one large builder per paired device.

# 12. Logging


## Logging requirements

Use the following levels:

```text
INFO    operation begin/end, ACK, state transition
DEBUG   per schema item/value/option
WARN    retry, reject, mismatch
ERROR   terminal operation failure
VERBOSE optional bounded CBOR hexdump
```

Never log:

```text
secret plaintext
Wi-Fi password
credential/token
full normal STRING value by default
```

Normal STRING values should log only:

```text
len=N
```

SECRET values:

```text
value=<redacted>
```

All Settings operations should be traceable using:

```text
device_id
request_id
command
sequence/total where applicable
transaction_id where applicable
revision where applicable
```


Required:

```text
[SCHEMA_BEGIN]
[SCHEMA_ITEM]
[SCHEMA_META]
[SCHEMA_OPTION]
[SCHEMA_END]
[SCHEMA_COMMIT]
[REJECT reason=...]
```

# 13. Tests

```text
DS-SCHEMA-001 BOOL
DS-SCHEMA-002 INT min/max/step
DS-SCHEMA-003 STRING max_length
DS-SCHEMA-004 ENUM + options
DS-SCHEMA-005 title
DS-SCHEMA-006 group/unit
DS-SCHEMA-007 duplicate ID reject
DS-SCHEMA-008 sequence gap reject
DS-SCHEMA-009 duplicate sequence reject
DS-SCHEMA-010 total mismatch reject
DS-SCHEMA-011 option before parent reject
DS-SCHEMA-012 option on non-enum reject
DS-SCHEMA-013 readonly translation
DS-SCHEMA-014 no device_id accepted
DS-SCHEMA-015 no snapshot_id accepted
DS-SCHEMA-016 old schema retained on failure
```

# 14. Exit gate

```text
[x] all descriptor metadata preserved
[x] enum options preserved
[x] wire flags translated
[x] no device_id requirement
[x] no snapshot_id requirement
[x] sequence/total validation
[x] atomic commit
[x] failed stream keeps previous schema
```
