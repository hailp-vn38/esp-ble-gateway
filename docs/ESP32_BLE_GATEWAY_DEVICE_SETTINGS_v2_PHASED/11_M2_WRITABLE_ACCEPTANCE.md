# Milestone M2 — Writable Settings Acceptance

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

Prove Settings changes can be safely persisted, confirmed, rebooted, and verified.

Prerequisites:

```text
M1 PASS
G6 PASS
G7 PASS
Device D0 transaction fixes PASS
```

# 2. Required writable types

```text
BOOL
INT
STRING
ENUM
```

# 3. End-to-end acceptance flow

```text
Web/UI request
 ↓
Gateway prevalidation
 ↓
BEGIN
 ↓ ACK
SET*
 ↓ ACK each
COMMIT
 ↓ ACK(new revision)
CONFIRM
 ↓ ACK
Device reboot
 ↓
Gateway reconnect
 ↓
READ
 ↓
revision/value verification
 ↓
SUCCESS
```

# 4. Required negative cases

```text
readonly
INT below min
INT above max
INT invalid step
ENUM invalid index
STRING too long
revision conflict
Device reject during SET
timeout during COMMIT
disconnect before confirm
post-reboot revision mismatch
post-reboot value mismatch
```

# 5. API behavior

Do not return success immediately after COMMIT.

Recommended async operation states:

```text
saving
waiting_reboot
verifying
success
conflict
failed
outcome_unknown
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


A successful operation must be reconstructable from logs by transaction ID.

Example:

```text
TX_BEGIN tx=12
TX_SET tx=12 ...
TX_COMMIT tx=12
COMMIT_ACK tx=12 new_rev=8
TX_CONFIRM tx=12 new_rev=8
WAIT_REBOOT tx=12
RECONNECT tx=12
VERIFY_OK tx=12 rev=8
```

# 7. HIL cases

```text
M2-HIL-001 BOOL save
M2-HIL-002 INT save
M2-HIL-003 STRING save
M2-HIL-004 ENUM save
M2-HIL-005 multiple settings one transaction
M2-HIL-006 revision conflict
M2-HIL-007 disconnect after commit
M2-HIL-008 reboot reconciliation
```

# 8. Acceptance checklist

```text
[ ] all writable types save
[ ] readonly blocked locally
[ ] Device persists values
[ ] CONFIRM sent
[ ] Device reboots
[ ] Gateway reconnects
[ ] new revision verified
[ ] values verified
[ ] ambiguous outcome handled
[ ] secret plaintext absent
[ ] transaction trace complete in logs
```
