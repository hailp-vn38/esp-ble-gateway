# Phase G5 — Settings Values Discovery

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

Decode BOOL/INT/STRING/ENUM values from key40 and atomically commit a coherent values snapshot.

# 2. Files

```text
components/device_settings/device_settings_protocol.c
components/device_settings/device_settings_memory.c
components/device_settings/device_settings.c
```

# 3. Values request/response asymmetry

Outbound request:

```text
command=read_settings
```

Current Device values response stream:

```text
command=get_settings
```

Final ACK:

```text
command=read_settings
```

Gateway routing must use:

```text
message type
active operation
BLE device context
request_id
```

Do not require response `command == read_settings` on stream frames.

# 4. Values begin

Expected:

```text
type=settings_values_begin
request_id
total
key21 capability_revision used as config revision by current Device
```

Normalize only inside this message family:

```c
config_revision = msg->capability_revision;
```

# 5. Value frame

Require:

```text
request_id
settings_sequence
setting_id
setting_type
setting_value(key40)
```

Validate against committed schema:

```text
setting exists
wire type matches descriptor
enum in range
string within max length
```

# 6. String ownership

Never retain a pointer into temporary CBOR/BLE receive storage.

Copy bounded value into committed values storage/string pool.

Recommended transaction/message bound:

```c
#define GW_SETTINGS_VALUE_STR_LEN 64
```

Schema-specific `max_length` may impose a lower limit.

# 7. Secret values

Never expose plaintext.

Preferred representation:

```text
configured=true/false
```

If Device does not provide safe secret semantics yet:

```text
mark value unavailable
```

Do not invent plaintext-compatible behavior.

# 8. Values stream integrity

Validate:

```text
same device
same request
sequence contiguous
received count == total
begin/end config revision same
```

# 9. Atomic commit

Builder failure:

```text
discard new values
retain previous values snapshot
```

# 10. Logging


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
[VALUES_BEGIN]
[VALUE]
[VALUES_END]
[VALUES_COMMIT]
```

Value formatting:

```text
BOOL   value=0/1
INT    value=number
ENUM   value=index
STRING len=N
SECRET value=<redacted>
```

# 11. Tests

```text
DS-VAL-001 BOOL
DS-VAL-002 INT
DS-VAL-003 STRING
DS-VAL-004 ENUM
DS-VAL-005 unknown ID reject
DS-VAL-006 type mismatch reject
DS-VAL-007 enum out-of-range reject
DS-VAL-008 total mismatch reject
DS-VAL-009 revision mismatch reject
DS-VAL-010 missing key40 reject
DS-VAL-011 no fallback to generic int_value
DS-VAL-012 old values retained on failed refresh
```

# 12. Exit gate

```text
[ ] BOOL/INT/STRING/ENUM decode correctly
[ ] key40 is authoritative
[ ] stream response command asymmetry handled
[ ] config revision normalized locally from key21
[ ] schema type validation active
[ ] failed refresh keeps old values
[ ] strings copied safely
[ ] secrets not exposed
```
