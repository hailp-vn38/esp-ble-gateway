# Milestone M1 — Read and Display Settings Acceptance

**Document set:** ESP32 BLE Gateway ↔ Device Settings v2  
**Version:** v2.1 phased documents  
**Date:** 2026-09-07  
**Gateway repo:** `hailp-vn38/esp-ble-gateway`  
**Gateway branch:** `dev-device-settings`  
**Device repo:** `hailp-vn38/esp-ble-device`  
**Device branch:** `main`  
**Protocol:** ESP-GATT Protocol v4  

---

# 1. Objective

Prove Device Settings can be discovered and displayed end-to-end without manual intervention.

Prerequisites:

```text
D0 PASS
G0 PASS
G1 PASS
G2 PASS
G3 PASS
G4 PASS
G5 PASS
```

# 2. End-to-end flow

```text
Device connect
  ↓
Gateway describe_capabilities
  ↓
Device key32/key33
  ↓
Gateway schema commit listener
  ↓
Gateway describe_settings
  ↓
Device schema stream
  ↓
Gateway schema commit
  ↓
describe_settings ACK
  ↓
Gateway read_settings
  ↓
Device values stream
  ↓
Gateway values commit
  ↓
read_settings ACK
  ↓
REST API / Web UI / MCP-readable state
```

# 3. Web API requirements

State mapping:

```text
UNSUPPORTED -> 404 settings_unsupported
UNKNOWN     -> 503 settings_not_discovered
DISCOVERING -> 503 settings_discovering
ERROR       -> 503 settings_error
READY       -> 200
```

Descriptor output:

```json
{
  "id": "target_temp",
  "title": "Nhiệt độ sấy",
  "group": "drying",
  "unit": "°C",
  "type": "integer",
  "readonly": false,
  "secret": false,
  "advanced": false,
  "minimum": 30,
  "maximum": 90,
  "step": 1,
  "value": 60
}
```

# 4. UI requirements

Must correctly render:

```text
BOOL
INT
STRING
ENUM
readonly
title
group
unit
```

Secret:

```text
never display plaintext
```

# 5. Required Device demo profile

At minimum:

```text
BOOL writable
BOOL readonly
INT writable with range
STRING writable
ENUM writable with >=3 options
SECRET descriptor
ADVANCED descriptor
```

# 6. Logging acceptance


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


Expected sequence:

```text
[CAPABILITY]
[QUEUE op=DESCRIBE]
[SETTINGS_TX cmd=describe_settings]
[SCHEMA_BEGIN]
[SCHEMA_ITEM]*
[SCHEMA_OPTION]*
[SCHEMA_END]
[SCHEMA_COMMIT]
[SETTINGS_ACK cmd=describe_settings]
[QUEUE op=READ]
[SETTINGS_TX cmd=read_settings]
[VALUES_BEGIN]
[VALUE]*
[VALUES_END]
[VALUES_COMMIT]
[SETTINGS_ACK cmd=read_settings]
```

# 7. HIL cases

```text
M1-HIL-001 cold boot discovery
M1-HIL-002 reboot Device
M1-HIL-003 Gateway reboot with Device online
M1-HIL-004 disconnect during schema
M1-HIL-005 disconnect during values
M1-HIL-006 two physical Devices
M1-HIL-007 Web UI active during discovery
```

# 8. Acceptance checklist

```text
[ ] key32/key33 visible on wire
[ ] Gateway detects support
[ ] describe_settings automatically sent
[ ] title/group/unit correct
[ ] enum options correct
[ ] readonly correct
[ ] read_settings only after describe ACK
[ ] BOOL value correct
[ ] INT value correct
[ ] STRING value correct
[ ] ENUM value correct
[ ] no required snapshot_id
[ ] no required response device_id
[ ] Web API READY
[ ] Web UI correct
[ ] logs sufficient to reconstruct operation
[ ] no secret plaintext in log/API/UI
[ ] two-device HIL passes
```

# 9. M1 failure policy

Do not proceed to writable M2 because UI appears to work partially.

Any protocol mismatch, cross-device builder corruption, or secret leakage is an M1 blocker.
