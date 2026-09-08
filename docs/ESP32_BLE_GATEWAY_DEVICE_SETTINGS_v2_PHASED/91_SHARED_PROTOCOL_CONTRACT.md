# Shared Specification — Settings Protocol v4 Contract

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


# 1. Device is the declared source of truth

Authoritative declarations originate from:

```text
esp-ble-device/components/gateway_protocol/include/gateway_protocol.h
esp-ble-device/components/gateway_protocol/include/gateway_settings.h
```

Gateway must mirror/import the same semantics.

# 2. Capability

```text
request:
  command=describe_capabilities

response capability stream:
  key32 settings_supported
  key33 settings_schema_revision
```

# 3. Schema

Request:

```text
type=device_command
command=describe_settings
request_id!=0
```

Response:

```text
settings_begin
settings_item*
settings_option_item*
settings_end
device_ack command=describe_settings
```

# 4. Values

Request:

```text
command=read_settings
```

Current Device stream command:

```text
get_settings
```

Final ACK:

```text
read_settings
```

# 5. Transaction

```text
settings_tx_begin
settings_tx_set
settings_tx_commit
settings_tx_abort
settings_commit_confirm
```

# 6. Correlation

Primary:

```text
BLE device context + request_id
```

Do not require Settings stream frames to contain:

```text
device_id
snapshot_id
```

# 7. Revisions

```text
settings_schema_revision -> key33
values config revision    -> current Device uses key21 inside values begin/end
transaction expected      -> key42
transaction new revision  -> key43
```

Do not globally reinterpret key21.

# 8. Enum options

```text
key47 parent settings sequence
key44 option index/value
key35 option label
```

# 9. Secret policy

Protocol implementation must never cause secret plaintext to leak into:

```text
serial log
Web API
Web UI
MCP diagnostics
```
