# Phase G1 — Gateway Codec Alignment

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

Make Gateway decode/encode the exact Device Protocol v4 Settings contract.

# 2. Files

```text
components/cbor_codec/include/cbor_codec.h
components/cbor_codec/cbor_codec.c
components/cbor_codec/include/gw_settings_view.h
components/cbor_codec/gw_settings_view.c
```

# 3. Remove duplicate key semantics

Current Gateway Settings key definitions must no longer interpret 32–47 differently from Device.

Allowed architecture:

```text
cbor_codec.h
  owns/shared imports canonical key constants

gw_settings_view.h
  owns only Settings-specific data structures
  does NOT renumber keys
```

# 4. Common message additions

At minimum decode:

```c
int has_settings_supported;
bool settings_supported;

int has_settings_schema_revision;
uint16_t settings_schema_revision;
```

# 5. Specialized Settings view

Recommended:

```c
typedef struct {
    bool has_sequence;
    uint16_t sequence;

    bool has_setting_id;
    char setting_id[GW_FEATURE_ID_LEN];

    bool has_title;
    char title[GW_MSG_CAP_LABEL_LEN];

    bool has_group;
    char group[32];

    bool has_unit;
    char unit[GW_MSG_CAP_UNIT_LEN];

    bool has_type;
    uint8_t type;

    bool has_flags;
    uint16_t flags;

    bool has_max_length;
    uint16_t max_length;

    bool has_option_index;
    uint8_t option_index;

    bool has_transaction_id;
    uint64_t transaction_id;

    bool has_expected_revision;
    uint32_t expected_revision;

    bool has_new_revision;
    uint32_t new_revision;

    bool has_value;
    /* typed value */
} gw_settings_view_t;
```

# 6. Typed key40 decode

Use wire type from key38.

Requirements:

```text
BOOL   -> CBOR bool
INT    -> signed integer
STRING -> bounded text
ENUM   -> unsigned integer
```

Do not fallback to generic `int_value`.

# 7. CBOR map order independence

The parser must accept:

```text
38 before 40
40 before 38
```

Recommended strategy:

```text
parse top-level map
retain slice/value descriptor for key40
finish map
decode key40 after key38 known
```

# 8. Key47 bug prevention

Key47 means:

```text
GW_KEY_SETTINGS_SEQUENCE
```

It must populate:

```c
has_settings_sequence
settings_sequence
```

Never `has_setting_id`.

# 9. Legacy compatibility

If legacy Settings frame names are still accepted:

```text
setting_item
setting_option_item
setting_value
```

keep that compatibility in message-type normalization, not by changing key meanings.

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


On decode error:

```c
ESP_LOGW("ds_protocol",
         "[CBOR_REJECT] key=%u reason=%s",
         key,
         reason);
```

Avoid dumping raw bytes unless VERBOSE trace is explicitly enabled.

# 11. Tests

```text
DS-CBOR-001 key32 integer 1
DS-CBOR-002 key32 integer 0
DS-CBOR-003 key33 revision
DS-CBOR-004 full INT schema item
DS-CBOR-005 readonly flags
DS-CBOR-006 enum option
DS-CBOR-007 BOOL key40
DS-CBOR-008 INT key40
DS-CBOR-009 STRING key40
DS-CBOR-010 ENUM key40
DS-CBOR-011 key40 before key38
DS-CBOR-012 oversized string reject
DS-CBOR-013 unknown key tolerated
DS-CBOR-014 trailing bytes reject
```

# 12. Exit gate

```text
[x] no incompatible private key table remains
[ ] Device golden vectors decode (blocked: G0 cross-repository fixtures are not present)
[x] key32/33 decode
[x] key34..47 decode correctly
[x] typed key40 works
[x] CBOR key order does not affect Settings value decode
[x] no unbounded string copy
```
