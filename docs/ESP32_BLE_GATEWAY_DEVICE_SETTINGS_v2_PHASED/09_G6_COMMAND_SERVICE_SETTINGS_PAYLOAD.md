# Phase G6 — Command Service Settings Payload

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

Add a typed Settings command payload to Gateway command service so transactions no longer misuse generic feature fields.

# 2. Files

```text
components/device_command_service/include/device_command_service.h
components/device_command_service/device_command_request.c
components/device_command_service/device_command_worker.c
components/device_command_service/device_command_pending.c
```

# 3. Current problem

Generic request fields:

```text
bool
int
feature_id
property_id
```

are not enough for:

```text
transaction_id
expected_revision
new_revision
setting_id
typed setting value
```

# 4. Payload model

Recommended:

```c
#define GW_SETTINGS_VALUE_STR_LEN 64

typedef struct {
    bool has_transaction_id;
    uint64_t transaction_id;

    bool has_expected_revision;
    uint32_t expected_revision;

    bool has_new_revision;
    uint32_t new_revision;

    bool has_setting_id;
    char setting_id[GW_FEATURE_ID_LEN];

    bool has_setting_value;
    uint8_t setting_type;

    union {
        bool bool_value;
        int32_t int_value;
        uint8_t enum_value;
        char string_value[GW_SETTINGS_VALUE_STR_LEN];
    } value;
} device_command_settings_payload_t;
```

Add to request:

```c
device_command_settings_payload_t settings;
```

# 5. Validation by command

## `describe_settings`

No Settings payload required.

## `read_settings`

No Settings payload required.

## `settings_tx_begin`

Require:

```text
transaction_id
expected_revision
```

## `settings_tx_set`

Require:

```text
transaction_id
setting_id
setting_type
setting_value
```

## `settings_tx_commit`

Require:

```text
transaction_id
```

## `settings_tx_abort`

Require:

```text
transaction_id
```

## `settings_commit_confirm`

Require:

```text
transaction_id
new_revision
```

# 6. Wire builder

Do not map setting fields through:

```text
feature_id
property_id
generic int_value
```

Encode canonical Settings keys directly.

# 7. String safety

Validate:

```text
NUL termination
len < GW_SETTINGS_VALUE_STR_LEN
len <= descriptor.max_length where known
```

# 8. Pending-slot behavior

Keep one command pending per device unless architecture intentionally changes.

Pending slot must be cleared before invoking completion callback so next transaction step can submit safely.

# 9. Logging


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


SEND logs:

```text
[TX_BEGIN]
[TX_SET]
[TX_COMMIT]
[TX_CONFIRM]
```

For `TX_SET`:

```text
STRING -> str_len
SECRET -> redacted
```

# 10. Tests

```text
DS-CMD-001 describe validation
DS-CMD-002 read validation
DS-CMD-003 begin payload
DS-CMD-004 bool set payload
DS-CMD-005 int set payload
DS-CMD-006 string set payload
DS-CMD-007 enum set payload
DS-CMD-008 commit payload
DS-CMD-009 abort payload
DS-CMD-010 confirm payload
DS-CMD-011 oversized string reject
DS-CMD-012 missing tx_id reject
```

# 11. Exit gate

```text
[ ] typed Settings payload exists
[ ] no Settings transaction path relies on feature_id/property_id overload
[ ] validators use canonical command requirements
[ ] wire builder emits keys 34/38/40/41/42/43 correctly
[ ] string bounds enforced
[ ] logs redact secrets
```
