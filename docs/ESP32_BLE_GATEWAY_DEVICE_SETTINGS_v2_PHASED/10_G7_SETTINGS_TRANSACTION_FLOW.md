# Phase G7 — Settings Transaction Flow ✅ DONE (2026-09-08)

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

Implement correct Gateway transaction state machine against Device's canonical BEGIN/SET/COMMIT/CONFIRM flow.

# 2. Files

```text
components/device_settings/device_settings_transaction.c
components/device_settings/include/device_settings.h
components/device_command_service/*
components/device_settings/device_settings_worker.c
```

# 3. Transaction states

Recommended:

```c
DS_TX_IDLE
DS_TX_PREVALIDATING
DS_TX_BEGIN_SENT
DS_TX_SET_SENT
DS_TX_COMMIT_SENT
DS_TX_CONFIRM_SENT
DS_TX_WAITING_REBOOT
DS_TX_VERIFYING
DS_TX_SUCCEEDED
DS_TX_FAILED
DS_TX_CONFLICT
DS_TX_CANCELLED
DS_TX_OUTCOME_UNKNOWN
```

# 4. Generate transaction ID

Requirements:

```text
non-zero
uint64
unique among active transactions
```

A monotonic persisted ID is not required unless product semantics need it.

# 5. Prevalidation

Before BLE transmission:

```text
setting exists
writable
type matches
INT min/max/step
ENUM index valid
STRING length valid
SECRET policy valid
```

Reject locally when invalid.

# 6. BEGIN

Send:

```text
command=settings_tx_begin
key41=transaction_id
key42=expected_revision
```

On rejection due to revision mismatch:

```text
state=DS_TX_CONFLICT
do not send SET
```

# 7. SET

For each changed setting:

```text
command=settings_tx_set
key41=transaction_id
key34=setting_id
key38=wire_type
key40=typed value
```

Wait for ACK before next SET to remain compatible with command service pending policy.

# 8. COMMIT

Send:

```text
command=settings_tx_commit
key41=transaction_id
```

On success capture new revision from Device ACK result field according to frozen contract.

Do **not** declare success yet.

# 9. CONFIRM

Send:

```text
command=settings_commit_confirm
key41=transaction_id
key43=new_revision
```

Only after CONFIRM ACK:

```text
state=DS_TX_WAITING_REBOOT
```

# 10. Reboot/reconnect reconciliation

Expected:

```text
confirm ACK
 ↓
Device reboot/disconnect
 ↓
Gateway reconnect
 ↓
read_settings
 ↓
config revision == new revision
 ↓
changed values == requested values
 ↓
SUCCESS
```

# 11. Ambiguous outcomes

Timeout after COMMIT may mean Device committed but ACK was lost.

Use:

```text
DS_TX_OUTCOME_UNKNOWN
```

Then reconcile after reconnect/read.

Do not report hard failure until verification disproves commit.

# 12. Disconnect semantics

Before COMMIT:

```text
fail/cancel transaction
```

After COMMIT but before known outcome:

```text
outcome unknown
```

After CONFIRM ACK:

```text
expected reboot path
```

# 13. Logging


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
[TX_BEGIN]
[TX_SET]
[TX_COMMIT]
[COMMIT_ACK]
[TX_CONFIRM]
[WAIT_REBOOT]
[RECONNECT]
[VERIFY_BEGIN]
[VERIFY_OK]
[VERIFY_FAIL]
[OUTCOME_UNKNOWN]
```

# 14. Tests

```text
DS-TX-001 non-zero tx_id
DS-TX-002 unique active tx_id
DS-TX-003 BEGIN
DS-TX-004 BOOL SET
DS-TX-005 INT SET
DS-TX-006 STRING SET
DS-TX-007 ENUM SET
DS-TX-008 readonly reject
DS-TX-009 range reject
DS-TX-010 invalid enum reject
DS-TX-011 revision conflict
DS-TX-012 SET reject stops transaction
DS-TX-013 COMMIT captures revision
DS-TX-014 CONFIRM emitted
DS-TX-015 WAITING_REBOOT only after confirm ACK
DS-TX-016 reconcile success
DS-TX-017 revision mismatch fail
DS-TX-018 value mismatch fail
DS-TX-019 commit timeout -> outcome unknown
DS-TX-020 cancel
DS-TX-021 second tx same device busy
```

# 15. Exit gate

```text
[x] BEGIN/SET/COMMIT/CONFIRM canonical
[x] local validation
[x] no success before post-reboot verify
[x] ambiguous outcomes represented explicitly
[x] reconnect reconciliation implemented
[x] transaction logs complete
```
