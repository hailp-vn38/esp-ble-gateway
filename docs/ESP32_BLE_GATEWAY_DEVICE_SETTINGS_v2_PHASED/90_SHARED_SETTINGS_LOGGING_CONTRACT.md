# Shared Specification — Settings SEND/RX Logging

**Document set:** ESP32 BLE Gateway ↔ Device Settings v2  
**Version:** v2.1 phased documents  
**Date:** 2026-09-07  
**Gateway repo:** `hailp-vn38/esp-ble-gateway`  
**Gateway branch:** `dev-device-settings`  
**Device repo:** `hailp-vn38/esp-ble-device`  
**Device branch:** `main`  
**Protocol:** ESP-GATT Protocol v4  

---

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


# 1. Gateway tags

Recommended:

```text
ds_worker
ds_tx
ds_rx
ds_protocol
```

# 2. Device tags

Recommended:

```text
settings_rx
settings_tx
settings_proto
```

# 3. Gateway SEND

Common:

```c
ESP_LOGI("ds_tx",
         "[SETTINGS_TX] dev=%s req=%lu cmd=%s",
         device_id,
         (unsigned long)request_id,
         command);
```

Transaction BEGIN:

```text
[TX_BEGIN] dev=... req=... tx=... expected_rev=...
```

SET:

```text
[TX_SET] dev=... req=... tx=... id=... type=... value=...
```

Safe formatting:

```text
BOOL   value=0/1
INT    value=number
ENUM   value=index
STRING str_len=N
SECRET value=<redacted>
```

COMMIT:

```text
[TX_COMMIT]
```

CONFIRM:

```text
[TX_CONFIRM] ... new_rev=...
```

# 4. Gateway RX

Frame:

```text
[SETTINGS_RX] dev=... type=... cmd=... req=... seq=... total=...
```

Schema:

```text
[SCHEMA_BEGIN]
[SCHEMA_ITEM]
[SCHEMA_META]
[SCHEMA_OPTION]
[SCHEMA_END]
[SCHEMA_COMMIT]
```

Values:

```text
[VALUES_BEGIN]
[VALUE]
[VALUES_END]
[VALUES_COMMIT]
```

ACK:

```text
[SETTINGS_ACK] dev=... req=... cmd=... ok=... int=...
```

# 5. Protocol rejects

Canonical reasons:

```text
missing_request_id
missing_total
missing_sequence
missing_setting_id
missing_setting_type
missing_setting_value
wrong_command
wrong_device
request_mismatch
sequence_mismatch
total_mismatch
duplicate_id
invalid_type
invalid_flags
invalid_range
invalid_option_parent
invalid_option_index
string_too_long
unknown_setting
value_type_mismatch
revision_mismatch
builder_no_memory
stream_not_active
```

Format:

```text
[REJECT] dev=... req=... reason=...
```

# 6. Device RX

```text
[SETTINGS_RX]
[TX_BEGIN_RX]
[TX_SET_RX]
[TX_COMMIT_RX]
[TX_CONFIRM_RX]
```

# 7. Device TX

```text
[SCHEMA_BEGIN_TX]
[SCHEMA_ITEM_TX]
[SCHEMA_OPTION_TX]
[SCHEMA_END_TX]
[VALUES_BEGIN_TX]
[VALUE_TX]
[VALUES_END_TX]
```

During D0, schema/value logs should include both:

```text
internal_type
wire_type
```

to catch mapping regressions.

# 8. Optional hexdump

Kconfig:

```text
CONFIG_DEVICE_SETTINGS_TRACE_HEXDUMP
CONFIG_GATEWAY_SETTINGS_TRACE_HEXDUMP
```

Only:

```c
ESP_LOG_BUFFER_HEXDUMP(..., ESP_LOG_VERBOSE);
```

Rules:

```text
disabled by default
VERBOSE only
skip secret transaction frames
no duplicate decode just for logging
```

# 9. Testability

Do not write tests that compare complete formatted lines.

Prefer testing a sanitizer/instrumentation helper:

```c
typedef struct {
    direction;
    device_id;
    request_id;
    command;
    type;
    sequence;
    total;
    tx_id;
    revision;
    reason;
} settings_trace_event_t;
```

Logging adapter formats the event.

This keeps tests robust while preserving serial diagnostics.
