# Phase G2 — Settings Capability Detection ✅ DONE (2026-09-07)

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

Make Gateway decide Settings support from Device's dedicated Settings capability fields.

# 2. Files

```text
components/device_schema/include/device_schema.h
components/device_schema/device_schema_protocol.c
components/device_schema/device_schema.c
main/main.c
```

# 3. Current incorrect behavior

Do not infer Settings support from:

```c
capability_flags & DEVICE_SCHEMA_FLAG_SETTINGS_SUPPORT
```

Device uses:

```text
key32 settings_supported
key33 settings_schema_revision
```

# 4. Snapshot model

Add:

```c
device_settings_state_t settings_state;
uint16_t settings_schema_revision;
```

Recommended support states:

```c
DS_SCHEMA_UNKNOWN
DS_SCHEMA_UNSUPPORTED
DS_SCHEMA_DISCOVERING
DS_SCHEMA_READY
DS_SCHEMA_ERROR
```

# 5. Parse policy

```c
if (msg->has_settings_supported) {
    if (msg->settings_supported) {
        snapshot->settings_state = DS_SCHEMA_READY; /* capability available */
        snapshot->settings_schema_revision =
            msg->has_settings_schema_revision
                ? msg->settings_schema_revision
                : 0;
    } else {
        snapshot->settings_state = DS_SCHEMA_UNSUPPORTED;
    }
}
```

At capability commit, normalize missing key32 as:

```text
legacy unsupported
```

unless there is a product requirement for UNKNOWN.

# 6. Schema commit listener

Use existing listener infrastructure.

`main.c`:

```c
static void on_schema_commit_for_settings(
    const char *device_id,
    uint32_t generation,
    void *context)
{
    device_schema_snapshot_t snapshot;

    if (device_schema_get(device_id, &snapshot) != ESP_OK) {
        return;
    }

    device_settings_on_capability(
        device_id,
        snapshot.settings_state != DS_SCHEMA_UNSUPPORTED,
        snapshot.settings_schema_revision);
}
```

Listener must queue work only.

Do not perform BLE IO directly inside schema commit callback.

# 7. Revision behavior

```text
unsupported:
    no Settings work

supported + no cached schema:
    DESCRIBE

supported + cached rev != advertised rev:
    DESCRIBE

supported + cached rev == advertised rev:
    READ
```

# 8. Logging


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


Recommended:

```text
I ds_worker: [CAPABILITY] dev=... supported=1 schema_rev=1
I ds_worker: [CAPABILITY] dev=... supported=0
I ds_worker: [SCHEMA_REV_CHANGED] dev=... old=1 new=2
```

# 9. Tests

```text
DS-CAP-001 supported key32=1
DS-CAP-002 unsupported key32=0
DS-CAP-003 missing key32 legacy policy
DS-CAP-004 capability_flags bit2 does not enable Settings
DS-CAP-005 revision changed -> DESCRIBE
DS-CAP-006 revision same -> READ
DS-CAP-007 listener queues only, no direct BLE call
```

# 10. Exit gate

```text
[x] support no longer depends on capability_flags bit
[x] schema revision persisted in snapshot
[x] listener wired
[x] supported device triggers correct next operation
[x] unsupported device queues nothing
```
