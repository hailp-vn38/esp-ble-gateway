# Phase D0 — Device Baseline Stabilization

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

Fix only the Device runtime defects that violate the Device-declared Protocol v4 Settings contract.

This phase does **not** redesign the protocol.

Repository:

```text
hailp-vn38/esp-ble-device
```

# 2. Why D0 is mandatory

Current Device source declares the correct Settings keys and wire types, but runtime paths contain implementation drift:

```text
capability struct fields set but not necessarily serialized
internal setting enum passed directly as wire type
Settings transaction key40 decode incomplete
commit_confirm handler exists but outer dispatch can skip it
transaction helpers may use legacy command strings
```

Gateway must not add heuristics to compensate for these Device bugs.

# 3. Files to inspect/change

```text
components/gateway_protocol/include/gateway_protocol.h
components/gateway_protocol/gateway_protocol.c
components/gateway_protocol/include/gateway_settings.h
components/gateway_protocol/gateway_settings.c

components/device_command/device_command.c

components/device_settings/include/device_settings.h
components/device_settings/device_settings_transaction.c

test/host/test_gateway_settings.c
test/host/test_settings_stream.c
test/host/test_settings_tx.c
test/host/test_settings_confirm.c
test/host/test_fault_injection.c
test/host/test_soak.c
```

# 4. D0.1 — Serialize Settings support and schema revision

Capability builder currently sets:

```c
message->settings_supported = 1;
message->has_settings_supported = 1;
message->settings_schema_revision = 1;
message->has_settings_schema_revision = 1;
```

The generic encoder must include both fields in `pair_count` and encode keys 32/33.

Required logic:

```c
if (msg->has_settings_supported) {
    pair_count++;
}

if (msg->has_settings_schema_revision) {
    pair_count++;
}
```

Encoding:

```c
if (rc == GW_OK && msg->has_settings_supported) {
    rc = gw_put_uint(&w, GW_KEY_SETTINGS_SUPPORTED);
    if (rc == GW_OK) {
        rc = gw_put_uint(&w, msg->settings_supported ? 1u : 0u);
    }
}

if (rc == GW_OK && msg->has_settings_schema_revision) {
    rc = gw_put_uint(&w, GW_KEY_SETTINGS_SCHEMA_REVISION);
    if (rc == GW_OK) {
        rc = gw_put_uint(&w, msg->settings_schema_revision);
    }
}
```

Canonical output:

```text
key32 = integer 0/1
key33 = uint16 revision
```

Do not make canonical Device output depend on CBOR simple bool.

Tests:

```text
DEV-WIRE-001 supported capability emits key32=1
DEV-WIRE-002 supported capability emits key33
DEV-WIRE-003 unsupported capability policy deterministic
```

Recommended unsupported policy:

```text
key32=0
key33 absent
```

# 5. D0.2 — Correct schema internal→wire type mapping

Device internal enum:

```c
DEVICE_SETTING_BOOL   = 0,
DEVICE_SETTING_INT    = 1,
DEVICE_SETTING_STRING = 2,
DEVICE_SETTING_ENUM   = 3,
```

Wire enum:

```text
BOOL=1
INT=2
STRING=4
ENUM=5
```

Therefore this is incorrect:

```c
(uint8_t)desc->type
```

When calling:

```c
gw_settings_encode_item(...)
```

use:

```c
uint8_t wire_type = settings_type_to_wire(desc->type);
```

Then:

```c
gw_settings_encode_item(..., wire_type, ...);
```

Add defensive failure:

```c
if (wire_type == GW_SETTING_TYPE_NONE) {
    ESP_LOGE(TAG, "invalid settings type id=%s internal_type=%u",
             desc->id, (unsigned)desc->type);
    goto fail;
}
```

Tests must verify actual Device command output:

```text
DEV-WIRE-010 BOOL -> key38=1
DEV-WIRE-011 INT -> key38=2
DEV-WIRE-012 STRING -> key38=4
DEV-WIRE-013 ENUM -> key38=5
```

# 6. D0.3 — Correct values internal→wire type mapping

Apply the same mapping before:

```c
gw_settings_encode_value(...)
```

Example:

```c
uint8_t wire_type = settings_type_to_wire(desc->type);
if (wire_type == GW_SETTING_TYPE_NONE) {
    goto fail;
}

enc = gw_settings_encode_value(
    storage,
    sizeof(storage),
    i,
    desc->id,
    wire_type,
    value_ptr,
    request_id);
```

Tests:

```text
DEV-WIRE-020 BOOL value
DEV-WIRE-021 INT value
DEV-WIRE-022 STRING value
DEV-WIRE-023 ENUM value
```

Each test must verify both:

```text
key38
key40 CBOR major type/value
```

# 7. D0.4 — Decode typed key40

Transaction SET requires:

```text
key34 setting_id
key38 setting_type
key40 setting_value
key41 transaction_id
```

Recommended bounded representation:

```c
#define GW_SETTINGS_VALUE_STR_LEN 64

typedef struct {
    uint8_t type;
    union {
        bool bool_val;
        int32_t int_val;
        uint8_t enum_val;
        char str_val[GW_SETTINGS_VALUE_STR_LEN];
    } value;
} gw_settings_wire_value_t;
```

Add:

```c
gw_settings_wire_value_t setting_value;
int has_setting_value;
```

Critical requirement:

> CBOR key order must not be assumed.

Test both:

```text
key38 before key40
key40 before key38
```

Recommended implementation options:

1. specialized Settings parser that scans key/value pairs and defers typed decode;
2. first pass finds key38, second pass extracts key40;
3. store raw slice for key40 and decode after map scan.

Avoid:

```c
case GW_KEY_SETTINGS_VALUE:
    if (!out_msg->has_setting_type) return error;
```

because it makes wire key order significant.

Tests:

```text
DEV-TX-001 BOOL
DEV-TX-002 INT
DEV-TX-003 STRING
DEV-TX-004 ENUM
DEV-TX-005 reversed key order
DEV-TX-006 oversized string rejected
```

# 8. D0.5 — Route `settings_commit_confirm`

The outer Device command worker must route:

```text
settings_commit_confirm
```

into the Settings handler.

Add it to the same dispatch condition as:

```text
describe_settings
read_settings
settings_tx_begin
settings_tx_set
settings_tx_commit
settings_tx_abort
```

Test:

```text
DEV-TX-010 request settings_commit_confirm
-> handle_settings_tx_command()
-> ACK
-> restart path scheduled according to Device transaction policy
```

# 9. D0.6 — Canonical transaction commands

Production helpers must emit exactly:

```text
settings_tx_begin
settings_tx_set
settings_tx_commit
settings_tx_abort
settings_commit_confirm
```

Legacy command names:

```text
set_settings
commit_settings
```

must not be used by the canonical v2 transaction flow.

If kept for compatibility, isolate them behind explicit legacy APIs and tests.

# 10. D0.7 — Secret setting behavior

Do not transmit secret plaintext.

For M1, recommended behavior is:

```text
schema advertises SECRET flag
values API exposes configured state only
raw secret value not returned
```

If current wire representation is not ready for this, mark SECRET value-read unsupported for M1 instead of sending an unsafe or type-confused payload.

# 11. SEND/RX logging in Device


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


Recommended tags:

```text
settings_rx
settings_tx
settings_proto
```

RX command:

```c
ESP_LOGI("settings_rx",
         "[SETTINGS_RX] req=%lu cmd=%s dev=%s",
         ...);
```

Schema SEND:

```c
ESP_LOGD("settings_tx",
         "[SCHEMA_ITEM_TX] req=%lu seq=%u/%u id=%s internal_type=%u wire_type=%u flags=0x%04x",
         ...);
```

Values SEND:

```c
ESP_LOGD("settings_tx",
         "[VALUE_TX] req=%lu seq=%u id=%s internal_type=%u wire_type=%u",
         ...);
```

Transaction SET:

```text
BOOL/INT/ENUM -> value may be logged
STRING        -> len=N
SECRET        -> value=<redacted>
```

# 12. D0 test matrix

| ID | Test | Expected |
|---|---|---|
| DEV-WIRE-001 | capability supported | key32=1 |
| DEV-WIRE-002 | capability revision | key33 present |
| DEV-WIRE-010 | BOOL schema | wire type 1 |
| DEV-WIRE-011 | INT schema | wire type 2 |
| DEV-WIRE-012 | STRING schema | wire type 4 |
| DEV-WIRE-013 | ENUM schema | wire type 5 |
| DEV-WIRE-020 | BOOL value | key40 bool |
| DEV-WIRE-021 | INT value | key40 integer |
| DEV-WIRE-022 | STRING value | key40 text |
| DEV-WIRE-023 | ENUM value | key40 uint |
| DEV-TX-001..004 | typed SET decode | PASS |
| DEV-TX-005 | key40 before key38 | PASS |
| DEV-TX-010 | confirm dispatch | ACK + confirm handling |
| DEV-TX-020..024 | command strings | exact canonical strings |

# 13. Exit gate

```text
[ ] key32/33 present in actual capability output
[ ] all internal types map to canonical key38 values
[ ] all values encode correct key40 type/value
[ ] typed transaction key40 decode works independent of CBOR key order
[ ] commit_confirm reaches handler
[ ] canonical transaction command strings pass tests
[ ] Device Settings RX/TX logs present
[ ] secret plaintext absent from logs
```

# 14. Recommended commits

```text
fix(protocol): serialize settings support metadata
fix(settings): map internal setting type to canonical wire type
fix(settings): decode typed transaction values
fix(settings): route settings commit confirm
fix(settings): normalize transaction command strings
feat(settings-log): add safe settings rx/tx diagnostics
test(settings): cover actual device command pipeline
```
