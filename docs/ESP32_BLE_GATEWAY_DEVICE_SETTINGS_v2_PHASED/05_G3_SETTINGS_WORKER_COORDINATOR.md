# Phase G3 — Settings Worker and Coordinator ✅ DONE (2026-09-07)

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

Turn queued Settings operations into real BLE command submissions while preserving memory and multi-device correctness.

# 2. Files

```text
components/device_settings/device_settings_operation.c
components/device_settings/device_settings_worker.c        # new
components/device_settings/device_settings.c
components/device_settings/include/device_settings.h
components/device_command_service/*
main/main.c
```

# 3. Worker architecture

Recommended queue:

```c
#define DEVICE_SETTINGS_QUEUE_DEPTH 8

typedef enum {
    DS_OP_DESCRIBE,
    DS_OP_READ,
    DS_OP_TX_RECONCILE,
} ds_op_kind_t;

typedef struct {
    char device_id[GW_MSG_DEVICE_ID_LEN];
    ds_op_kind_t kind;
    uint32_t generation;
} ds_work_item_t;
```

# 4. Active operation

Use one active Settings stream globally while global builders exist:

```c
typedef struct {
    bool active;
    char device_id[GW_MSG_DEVICE_ID_LEN];
    ds_op_kind_t kind;
    uint32_t generation;
    uint32_t request_id;
    uint8_t retry_count;
} ds_active_op_t;
```

# 5. Why serialize globally

```text
current schema builder = global static
current values builder = global static
```

Allowing Device A and B Settings streams concurrently can mix data.

Global serialization is acceptable because Settings discovery is low-frequency and saves RAM compared with per-device large builders.

# 6. Command submission

DESCRIBE:

```text
origin = DEVICE_CMD_ORIGIN_SETTINGS
command = describe_settings
```

READ:

```text
origin = DEVICE_CMD_ORIGIN_SETTINGS
command = read_settings
```

Never send:

```text
get_settings
```

as the outbound read command.

# 7. BUSY handling

A capability command ACK can still be completing when the Settings callback queues DESCRIBE.

Treat command service BUSY as retryable.

Recommended bounded backoff:

```text
100 ms
250 ms
500 ms
then fail
```

Do not spin in a tight loop.

# 8. Completion ordering

DESCRIBE completion callback after ACK:

```text
mark describe complete
queue READ
release pending slot
```

Do not queue READ from `settings_end`.

# 9. Duplicate coalescing

For same:

```text
device_id + operation kind + schema revision/generation
```

avoid duplicate queued entries.

# 10. Disconnect

On disconnect:

```text
cancel active Settings command
reset builder ownership
increment generation
preserve last committed schema/values
allow queued work for other device to continue
```

# 11. Logging


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


Recommended events:

```text
[QUEUE]
[DEQUEUE]
[START]
[BUSY_RETRY]
[COMPLETE]
[CANCEL]
[DISCONNECT]
[OWNER_RELEASE]
```

Example:

```c
ESP_LOGI("ds_worker",
         "[START] dev=%s op=%s gen=%lu req=%lu",
         ...);
```

# 12. Tests

```text
[x] DS-WORK-001 capability -> DESCRIBE
[x] DS-WORK-002 same rev -> READ
[x] DS-WORK-003 command BUSY retry
[x] DS-WORK-004 retry exhausted
[x] DS-WORK-005 timeout cleanup
[x] DS-WORK-006 disconnect cleanup
[x] DS-WORK-007 duplicate trigger coalescing
[x] DS-WORK-008 READ only after DESCRIBE ACK
[x] DS-WORK-009 queue full explicit error
[x] DS-MULTI-001 A then B serialization (verified during G4 integration)
[x] DS-MULTI-002 active owner enforcement (verified during G4 integration)
[x] DS-MULTI-003 wrong-device frame rejected (verified during G4 integration)
```

# 13. Exit gate ✅ DONE (2026-09-07)

```text
[x] queued operations are actually transmitted
[x] one global Settings stream owner
[x] BUSY bounded retry implemented
[x] READ starts only after DESCRIBE ACK
[x] duplicate operations coalesced
[x] disconnect releases ownership
```

Verified on the connected ESP32-S3: DS-WORK-001 through DS-WORK-009 passed in two consecutive test-run boots. The complete repository suite still resets later because of an unrelated `main` task stack overflow; it occurs after the G3 tests have passed.
