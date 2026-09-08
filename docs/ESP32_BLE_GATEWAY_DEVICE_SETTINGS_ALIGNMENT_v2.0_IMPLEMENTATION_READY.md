# ESP32 BLE Gateway ↔ Device Settings v2 — Implementation Guide v2

**Document version:** v2.0
**Date:** 2026-09-07
**Gateway repository:** `hailp-vn38/esp-ble-gateway`
**Gateway target branch:** `dev-device-settings`
**Device repository:** `hailp-vn38/esp-ble-device`
**Device baseline branch:** `main`
**Protocol:** ESP-GATT Protocol v4
**Primary rule:** **The Device-declared Settings wire contract is the source of truth. Both Device runtime and Gateway must conform to it.**

---

# 1. Purpose

This document replaces v1.1.

v1.1 correctly identified the main Gateway incompatibilities, but review against the current Device runtime found additional Device-side implementation defects. Therefore v2 separates two concepts:

```text
Device declared protocol
    components/gateway_protocol/include/gateway_protocol.h
            │
            └── authoritative contract

Device current runtime implementation
    gateway_protocol.c
    gateway_settings.c
    device_command.c
            │
            └── must be fixed where it violates the declared contract
```

The implementation order is now:

```text
D0  Stabilize Device runtime against its own declared protocol
 ↓
G0  Freeze cross-repository golden vectors
 ↓
G1  Align Gateway codec / Settings parser
 ↓
G2  Capability detection
 ↓
G3  Settings worker / operation orchestration
 ↓
G4  Schema discovery
 ↓
G5  Values discovery
 ↓
M1  Read/display Settings works end-to-end
 ↓
G6  Transaction request model
 ↓
G7  Transaction BEGIN/SET/COMMIT/CONFIRM/reconcile
 ↓
M2  Writable Settings works end-to-end
 ↓
H1  HIL / fault / memory / soak qualification
```

A second major addition in v2 is a mandatory Settings logging contract for both SEND and RX paths.

---

# 2. Non-negotiable protocol decisions

## 2.1 Protocol version

```c
GW_PROTOCOL_VERSION = 4
```

Do not introduce Protocol v5 to fix these implementation issues.

## 2.2 Canonical Settings keys

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

Gateway must delete/replace its old incompatible interpretation:

```text
32 SETTINGS_BEGIN
33 SETTINGS_ITEM
34 SETTINGS_END
...
```

## 2.3 Canonical Settings wire types

```c
GW_SETTING_TYPE_NONE   = 0,
GW_SETTING_TYPE_BOOL   = 1,
GW_SETTING_TYPE_INT    = 2,
GW_SETTING_TYPE_FLOAT  = 3,
GW_SETTING_TYPE_STRING = 4,
GW_SETTING_TYPE_ENUM   = 5,
```

These are **wire values**.

They are not the same as Device internal `device_setting_type_t`.

## 2.4 Device internal types are not wire types

Current Device internal type enum:

```c
DEVICE_SETTING_BOOL   = 0,
DEVICE_SETTING_INT    = 1,
DEVICE_SETTING_STRING = 2,
DEVICE_SETTING_ENUM   = 3,
```

Therefore this is forbidden:

```c
(uint8_t)desc->type
```

when encoding a wire Settings type.

Always use:

```c
settings_type_to_wire(desc->type)
```

or one centralized equivalent mapper.

## 2.5 Settings flags

Device wire:

```c
GW_SETTING_FLAG_READONLY = 1 << 0
GW_SETTING_FLAG_SECRET   = 1 << 1
GW_SETTING_FLAG_ADVANCED = 1 << 2
```

Gateway internal flags must be translated.

Example:

```c
static uint16_t ds_flags_from_wire(uint16_t wire_flags)
{
    uint16_t flags = 0;

    if ((wire_flags & GW_SETTING_FLAG_READONLY) == 0) {
        flags |= DS_FLAG_WRITABLE;
    }
    if (wire_flags & GW_SETTING_FLAG_SECRET) {
        flags |= DS_FLAG_SECRET;
    }
    if (wire_flags & GW_SETTING_FLAG_ADVANCED) {
        flags |= DS_FLAG_ADVANCED;
    }

    return flags;
}
```

Never copy Device flags directly into Gateway internal flags.

---

# 3. Canonical message families

## Capability

Request:

```text
type    = device_command
command = describe_capabilities
```

Device capability stream must include:

```text
32 settings_supported
33 settings_schema_revision
```

when Settings is supported.

## Schema discovery

Request:

```text
type       = device_command
command    = describe_settings
request_id = non-zero
```

Response:

```text
settings_begin
settings_item*
settings_option_item*
settings_end
device_ack(command=describe_settings)
```

## Values discovery

Request:

```text
type       = device_command
command    = read_settings
request_id = non-zero
```

Current Device response frames use:

```text
settings_values_begin  command=get_settings
settings_values_value  command=get_settings
settings_values_end    command=get_settings
```

Final ACK echoes:

```text
command=read_settings
```

Gateway must accept this asymmetry.

## Transaction

Canonical commands:

```text
settings_tx_begin
settings_tx_set
settings_tx_commit
settings_tx_abort
settings_commit_confirm
```

---

# 4. Phase D0 — Stabilize Device runtime

This phase is now mandatory before freezing golden vectors.

Repository:

```text
hailp-vn38/esp-ble-device
```

---

## D0.1 Encode `settings_supported` and schema revision

### Problem

`device_command.c` sets:

```c
message->settings_supported = 1;
message->has_settings_supported = 1;
message->settings_schema_revision = 1;
message->has_settings_schema_revision = 1;
```

but the generic Device encoder must actually serialize keys 32/33.

### Files

```text
components/gateway_protocol/gateway_protocol.c
components/gateway_protocol/include/gateway_protocol.h
test/host/test_gateway_protocol.c
test/host/test_gateway_settings.c
```

### Required encoder logic

Add to `pair_count`:

```c
if (msg->has_settings_supported) pair_count++;
if (msg->has_settings_schema_revision) pair_count++;
```

Add serialization:

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

### Canonical representation

Use integer:

```text
32: 0/1
```

for the Device canonical golden vector.

Gateway may accept CBOR bool additionally, but Device canonical output should remain deterministic.

### Tests

```text
DEV-WIRE-001 capabilities_begin contains key32=1
DEV-WIRE-002 capabilities_begin contains key33=current revision
DEV-WIRE-003 unsupported device omits or emits key32=0 according to frozen policy
```

Recommended policy:

```text
supported -> emit key32=1 + key33
unsupported -> emit key32=0
```

---

## D0.2 Fix schema type conversion

### Problem

Current schema stream calls:

```c
gw_settings_encode_item(..., (uint8_t)desc->type, ...)
```

This is invalid because internal and wire enum values differ.

### File

```text
components/device_command/device_command.c
```

### Replace

```c
(uint8_t)desc->type
```

with:

```c
settings_type_to_wire(desc->type)
```

### Mandatory tests

```text
DEV-WIRE-010 BOOL descriptor -> wire type 1
DEV-WIRE-011 INT descriptor -> wire type 2
DEV-WIRE-012 STRING descriptor -> wire type 4
DEV-WIRE-013 ENUM descriptor -> wire type 5
```

Tests must capture the actual Device notify output from the command pipeline, not only call `gw_settings_encode_item()` directly.

---

## D0.3 Fix values type conversion

### Problem

Current read path passes internal `desc->type` directly into `gw_settings_encode_value()`.

### Required change

Replace:

```c
(uint8_t)desc->type
```

with:

```c
settings_type_to_wire(desc->type)
```

for normal runtime value frames.

### Tests

```text
DEV-WIRE-020 BOOL value encodes key38=1 + key40=bool
DEV-WIRE-021 INT value encodes key38=2 + key40=int
DEV-WIRE-022 STRING value encodes key38=4 + key40=text
DEV-WIRE-023 ENUM value encodes key38=5 + key40=uint
```

---

## D0.4 Decode Settings value key 40 for transaction SET

### Problem

The Device contract declares:

```text
40 = SETTINGS_VALUE
```

but transaction decode must populate a typed Settings value.

### Recommended model

Add a bounded Settings value field to `gw_message_t` or a specialized Settings decode structure.

Recommended maximum string value:

```c
#define GW_SETTINGS_VALUE_STR_LEN 64
```

Do not default to 128 bytes.

Example:

```c
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

Decode key 40 based on key 38.

### Important decoder rule

CBOR map key order must not be assumed.

Therefore do **not** require key38 to appear before key40 unless the entire Settings message is decoded in two passes or via targeted lookup.

Preferred implementation:

```text
generic decode:
    locate settings_type
    locate settings_value
    decode typed value after type is known
```

### Tests

```text
DEV-TX-001 bool tx_set decode
DEV-TX-002 int tx_set decode
DEV-TX-003 string tx_set decode
DEV-TX-004 enum tx_set decode
DEV-TX-005 key40 before key38 still decodes correctly
```

---

## D0.5 Route `settings_commit_confirm`

### Problem

The Settings handler supports the command, but the outer worker dispatch must route it.

### File

```text
components/device_command/device_command.c
```

### Required worker condition

Include:

```c
strcmp(msg.command, GW_MSG_TYPE_SETTINGS_COMMIT_CONFIRM) == 0
```

in the Settings dispatch condition.

### Test

```text
DEV-TX-010 commit_confirm reaches handle_settings_tx_command()
```

---

## D0.6 Normalize transaction helper command strings

All specialized TX encoder helpers and dispatchers must use one canonical command set:

```text
settings_tx_begin
settings_tx_set
settings_tx_commit
settings_tx_abort
settings_commit_confirm
```

No helper used by production flow should emit legacy:

```text
set_settings
commit_settings
```

unless explicitly marked and tested as legacy compatibility.

### Tests

```text
DEV-TX-020 begin encoder command exact
DEV-TX-021 set encoder command exact
DEV-TX-022 commit encoder command exact
DEV-TX-023 abort encoder command exact
DEV-TX-024 confirm encoder command exact
```

---

## D0.7 Secret setting runtime value

Secret values must never send plaintext.

Current product behavior should be normalized to one explicit contract.

Recommended v2 behavior:

```text
descriptor type = original type
descriptor flags contains SECRET
runtime value = configured/not-configured metadata only
```

If changing the wire representation is undesirable now, mark secret values unsupported for M1 and do not make them an M1 release blocker.

Gateway and Device logs must never print secret values.

---

## D0 exit gate

Do not freeze Device fixtures until:

```text
[ ] capability key32/33 appears on actual wire output
[ ] schema BOOL/INT/STRING/ENUM wire types are canonical
[ ] values BOOL/INT/STRING/ENUM wire types are canonical
[ ] transaction key40 can be decoded
[ ] commit_confirm reaches transaction handler
[ ] tx helper command strings are canonical
```

---

# 5. Phase G0 — Freeze golden vectors

Golden vectors must come from two layers.

## Layer 1 — codec vectors

Direct specialized codec output:

```text
gw_settings_encode_begin()
gw_settings_encode_item()
gw_settings_encode_option_item()
gw_settings_encode_values_begin()
gw_settings_encode_value()
...
```

## Layer 2 — actual Device command pipeline vectors

Input:

```text
device_setting_descriptor_t
device_setting runtime callbacks
Gateway request
```

Capture:

```text
actual bytes passed to Device notify function
```

This catches integration bugs such as incorrect internal→wire type conversion.

### Required fixtures

```text
test/fixtures/settings_v2/
├── capabilities_settings_supported.cbor
├── settings_begin.cbor
├── settings_bool_item.cbor
├── settings_int_item.cbor
├── settings_string_item.cbor
├── settings_enum_item.cbor
├── settings_enum_option_0.cbor
├── settings_values_begin.cbor
├── settings_value_bool.cbor
├── settings_value_int.cbor
├── settings_value_string.cbor
├── settings_value_enum.cbor
├── settings_values_end.cbor
├── tx_begin.cbor
├── tx_set_bool.cbor
├── tx_set_int.cbor
├── tx_set_string.cbor
├── tx_set_enum.cbor
├── tx_commit.cbor
└── tx_confirm.cbor
```

CI rule:

```text
actual Device output -> Gateway decoder PASS
Gateway request output -> Device decoder PASS
```

---

# 6. Gateway protocol architecture

Repository:

```text
hailp-vn38/esp-ble-gateway
branch dev-device-settings
```

Recommended architecture:

```text
cbor_codec
    base Protocol v4
    settings capability keys 32/33
         │
         ├── common gw_message_t
         │
         └── shared key constants
                    │
                    ▼
settings_codec / gw_settings_view
    Settings-specific metadata/value decode
    no private key numbering
                    │
                    ▼
device_settings_protocol
    stream semantics / builder / commit
```

v2 changes the v1.1 recommendation slightly:

> `gw_settings_view` does not have to be deleted.
> The duplicate wire contract must be deleted.

A specialized Settings codec is acceptable and can keep the common `gw_message_t` smaller.

---

# 7. Phase G1 — Align Gateway codec

## Files

```text
components/cbor_codec/include/cbor_codec.h
components/cbor_codec/cbor_codec.c
components/cbor_codec/include/gw_settings_view.h
components/cbor_codec/gw_settings_view.c
```

## Required changes

- one key table only;
- keys 32–47 exactly match Device;
- decode key32/33;
- decode schema metadata;
- decode option index;
- decode key40 typed values;
- decode transaction fields;
- decode Settings sequence key47 correctly;
- remove old keys 48–52 unless placed in a separately versioned extension.

## Do not

```text
do not map key47 to has_setting_id
do not interpret key34 as SETTINGS_END
do not overload feature_id as setting_id in the wire codec
```

---

# 8. Phase G2 — Capability Settings detection

## Gateway files

```text
components/device_schema/include/device_schema.h
components/device_schema/device_schema_protocol.c
```

Add to committed schema:

```c
uint16_t settings_schema_revision;
```

Use:

```c
message->has_settings_supported &&
message->settings_supported
```

for support detection.

Do not use:

```c
DEVICE_SCHEMA_FLAG_SETTINGS_SUPPORT
```

as the wire support mechanism.

### Policy

```text
settings_supported=1 -> READY/support available
settings_supported=0 -> UNSUPPORTED
missing key32 -> UNSUPPORTED legacy device
```

---

# 9. Phase G3 — Settings coordinator and worker ✅ DONE (2026-09-07)

The current Gateway operation layer queues records but needs a real execution path.

Recommended new file:

```text
components/device_settings/device_settings_worker.c
```

Use one bounded queue:

```c
typedef struct {
    char device_id[GW_MSG_DEVICE_ID_LEN];
    ds_op_kind_t kind;
    uint32_t generation;
} ds_work_item_t;
```

Suggested:

```c
#define DEVICE_SETTINGS_QUEUE_DEPTH 8
```

## Global Settings stream serialization

Keep only one active schema/value builder at a time.

```c
typedef struct {
    bool active;
    char device_id[GW_MSG_DEVICE_ID_LEN];
    ds_op_kind_t kind;
    uint32_t generation;
    uint32_t request_id;
} ds_active_stream_t;
```

Reason:

- current builders are global;
- prevents cross-device corruption;
- minimizes SRAM;
- Settings discovery is low frequency.

---

# 10. Settings orchestration lifecycle

Use schema commit listener from `main.c`.

```text
capability schema committed
    ↓
settings supported?
    ├─ no -> UNSUPPORTED
    └─ yes
         ↓
advertised schema revision changed?
    ├─ yes -> DESCRIBE
    └─ no  -> READ
```

## Critical ordering

Correct:

```text
SEND describe_settings
RX settings_begin
RX settings_item*
RX settings_option_item*
RX settings_end
RX device_ack(describe_settings)
operation DESCRIBE complete
queue READ
SEND read_settings
```

Incorrect:

```text
RX settings_end
immediately SEND read_settings
before describe ACK
```

**G3 verification (2026-09-07):** `DS-WORK-001` through `DS-WORK-009` passed twice on the connected ESP32-S3, covering transmission, global serialization, bounded BUSY retry, ACK-gated READ, duplicate coalescing, disconnect cleanup, and operation-queue exhaustion.

---

# 11. Phase G4 — Schema parser ✅ DONE (2026-09-07)

## `settings_begin`

Require:

```text
protocol_version=4
type=settings_begin
command=describe_settings
request_id
total
```

Do not require:

```text
device_id
snapshot_id
capability_revision
```

The BLE callback device context is authoritative for routing.

## `settings_item`

Require:

```text
request_id
total
settings_sequence
setting_id
setting_type
```

Preserve:

```text
title
group
unit
flags
max_length
min
max
step
```

## Enum options

Handle:

```text
settings_option_item
```

with:

```text
settings_sequence = parent index
settings_option_index = option index
settings_title = option label
```

## End

Validate:

```text
same active device
same active request
total matches
received count matches
```

Do not submit READ inside `settings_end`.

**G4 verification (2026-09-07):** the connected ESP32-S3 passed all
`DS-SCHEMA-001` through `DS-SCHEMA-016` cases plus CBOR metadata roundtrip,
interleaved multi-ENUM options, request/device ownership, and A-then-B stream
serialization (`23 Tests, 0 Failures, 0 Ignored`). The production firmware
also built, flashed, joined Wi-Fi, initialized Settings/worker/BLE/Web/MCP,
and reached `gateway_ready`.

---

# 12. Phase G5 — Values parser

## `settings_values_begin`

Accept:

```text
request_id
total
capability_revision
```

Current Device uses base key21 for current config revision in this message family.

Normalize locally:

```c
config_revision = msg->capability_revision;
```

Do not globally rename key21.

## Value

Require:

```text
request_id
settings_sequence
setting_id
setting_type
setting_value(key40)
```

Never fallback to generic `int_value`.

## End

Validate:

```text
total
count
revision
active request/device
```

Commit atomically and retain old values on failed refresh.

---

# 13. Milestone M1

M1 now requires both Device D0 and Gateway G1–G5.

Flow:

```text
Device fixed runtime
    ↓
Gateway describe_capabilities
    ↓
key32/key33 received
    ↓
Gateway describe_settings
    ↓
schema decoded
    ↓
Gateway read_settings
    ↓
values decoded
    ↓
Web API
    ↓
Web UI
```

M1 pass criteria:

```text
[ ] BOOL descriptor correct
[ ] INT descriptor correct
[ ] STRING descriptor correct
[ ] ENUM descriptor + options correct
[ ] title/group/unit preserved
[ ] readonly correct
[ ] values correct
[ ] no snapshot_id required
[ ] no response device_id required
[ ] reconnect works
[ ] two devices do not mix builders
```

---

# 14. Phase G6 — Command-service Settings payload

Current request model is too generic for transaction Settings.

Add a bounded Settings payload:

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

Do not repurpose:

```text
feature_id
property_id
generic int_value
```

for Settings-specific semantics.

---

# 15. Phase G7 — Transaction state machine

Correct flow:

```text
BEGIN
  command=settings_tx_begin
  key41=tx_id
  key42=expected_revision
    ↓ ACK

SET*
  command=settings_tx_set
  key41=tx_id
  key34=setting_id
  key38=wire_type
  key40=typed value
    ↓ ACK each

COMMIT
  command=settings_tx_commit
  key41=tx_id
    ↓ ACK(new revision)

CONFIRM
  command=settings_commit_confirm
  key41=tx_id
  key43=new revision
    ↓ ACK

WAITING_REBOOT
    ↓ disconnect
    ↓ reconnect
READ
    ↓
VERIFY revision + values
    ↓
SUCCESS
```

Recommended states:

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
```

Success must not be reported at COMMIT ACK.

---

# 16. Settings SEND/RX logging contract

This section is mandatory in v2.

The goal is to make BLE Settings failures diagnosable from one serial log without enabling raw packet dumps.

---

## 16.1 Logging principles

Use:

```text
INFO  operation lifecycle and state transitions
DEBUG per-frame metadata
WARN  protocol mismatch / retry / reject
ERROR unrecoverable operation failure
VERBOSE optional bounded hexdump
```

Never log:

```text
Wi-Fi password
secret setting plaintext
tokens
credentials
full SECRET setting values
```

For SECRET:

```text
value=<redacted>
configured=0/1
```

For normal STRING values, default log should be:

```text
len=N
```

not plaintext.

---

# 17. Gateway logging tags

Recommended:

```c
static const char *TAG_DS_WORK = "ds_worker";
static const char *TAG_DS_RX   = "ds_rx";
static const char *TAG_DS_TX   = "ds_tx";
static const char *TAG_DS_PROT = "ds_protocol";
```

Existing per-file static tags are also acceptable:

```text
ds_worker
ds_protocol
ds_tx
device_cmd_svc
```

Keep naming consistent.

---

# 18. Gateway SEND logs

## Location

Primary location:

```text
components/device_command_service/device_command_worker.c
```

Immediately before:

```c
g_dcs.hooks.send_command(...)
```

Existing generic log:

```text
[SEND] device=... request_id=... command=...
```

should be extended for Settings.

### Example helper

```c
static bool dcs_is_settings_command(const char *command)
{
    return strcmp(command, "describe_settings") == 0 ||
           strcmp(command, "read_settings") == 0 ||
           strcmp(command, "settings_tx_begin") == 0 ||
           strcmp(command, "settings_tx_set") == 0 ||
           strcmp(command, "settings_tx_commit") == 0 ||
           strcmp(command, "settings_tx_abort") == 0 ||
           strcmp(command, "settings_commit_confirm") == 0;
}
```

### Common SEND log

```c
ESP_LOGI("ds_tx",
         "[SETTINGS_TX] dev=%s req=%lu cmd=%s",
         slot->device_id,
         (unsigned long)slot->request_id,
         slot->command);
```

### BEGIN

```c
ESP_LOGD("ds_tx",
         "[TX_BEGIN] dev=%s req=%lu tx=%llu expected_rev=%lu",
         slot->device_id,
         (unsigned long)slot->request_id,
         (unsigned long long)payload->transaction_id,
         (unsigned long)payload->expected_revision);
```

### SET

```c
ESP_LOGD("ds_tx",
         "[TX_SET] dev=%s req=%lu tx=%llu id=%s type=%u%s",
         slot->device_id,
         (unsigned long)slot->request_id,
         (unsigned long long)payload->transaction_id,
         payload->setting_id,
         (unsigned)payload->setting_type,
         is_secret ? " value=<redacted>" : "");
```

For non-secret values, do not concatenate a universal plaintext logger.

Use type-specific safe logs:

```c
switch (payload->setting_type) {
case GW_SETTING_TYPE_BOOL:
    ESP_LOGD(TAG, "... value=%d", payload->value.bool_value);
    break;
case GW_SETTING_TYPE_INT:
    ESP_LOGD(TAG, "... value=%ld", (long)payload->value.int_value);
    break;
case GW_SETTING_TYPE_ENUM:
    ESP_LOGD(TAG, "... value=%u", payload->value.enum_value);
    break;
case GW_SETTING_TYPE_STRING:
    ESP_LOGD(TAG, "... str_len=%u",
             (unsigned)strnlen(payload->value.string_value,
                              sizeof(payload->value.string_value)));
    break;
}
```

### COMMIT

```c
ESP_LOGI("ds_tx",
         "[TX_COMMIT] dev=%s req=%lu tx=%llu",
         device_id, request_id, tx_id);
```

### CONFIRM

```c
ESP_LOGI("ds_tx",
         "[TX_CONFIRM] dev=%s req=%lu tx=%llu new_rev=%lu",
         device_id, request_id, tx_id, new_revision);
```

---

# 19. Gateway RX Settings logs

## Location

Primary entry:

```text
components/device_settings/device_settings_protocol.c
```

At the beginning of:

```c
device_settings_on_notify()
```

only when message type is a Settings stream type.

### Common RX frame helper

```c
static void ds_log_rx_frame(const char *device_id,
                            const gw_message_t *msg)
{
    ESP_LOGD("ds_rx",
             "[SETTINGS_RX] dev=%s type=%s cmd=%s req=%lu"
             " seq=%u%s total=%u%s",
             device_id,
             msg->type,
             msg->command,
             (unsigned long)(msg->has_request_id ? msg->request_id : 0),
             (unsigned)(msg->has_settings_sequence
                            ? msg->settings_sequence : 0),
             msg->has_settings_sequence ? "" : "(na)",
             (unsigned)(msg->has_total ? msg->total : 0),
             msg->has_total ? "" : "(na)");
}
```

Do not log every non-Settings BLE packet from this helper.

---

# 20. Gateway RX lifecycle logs

## `settings_begin`

```c
ESP_LOGI(TAG,
         "[SCHEMA_BEGIN] dev=%s req=%lu total=%u schema_rev=%u",
         device_id,
         request_id,
         total,
         advertised_schema_revision);
```

## `settings_item`

```c
ESP_LOGD(TAG,
         "[SCHEMA_ITEM] dev=%s req=%lu seq=%u/%u id=%s type=%u"
         " flags=0x%04x",
         device_id,
         request_id,
         seq,
         total,
         setting_id,
         setting_type,
         setting_flags);
```

Optional metadata log:

```c
ESP_LOGD(TAG,
         "[SCHEMA_META] id=%s title='%s' group='%s' unit='%s'"
         " min=%ld max=%ld step=%lu max_len=%u",
         ...);
```

Avoid logging title/group at INFO to reduce noise.

## Enum option

```c
ESP_LOGD(TAG,
         "[SCHEMA_OPTION] dev=%s req=%lu parent=%u option=%u label='%s'",
         device_id,
         request_id,
         parent_index,
         option_index,
         option_label);
```

## Schema end

```c
ESP_LOGI(TAG,
         "[SCHEMA_END] dev=%s req=%lu expected=%u received=%u",
         device_id,
         request_id,
         expected_count,
         received_count);
```

## Commit

```c
ESP_LOGI(TAG,
         "[SCHEMA_COMMIT] dev=%s count=%u rev=%u",
         device_id,
         schema->setting_count,
         schema->schema_revision);
```

---

# 21. Gateway values RX logs

## Values begin

```c
ESP_LOGI(TAG,
         "[VALUES_BEGIN] dev=%s req=%lu total=%u config_rev=%lu",
         device_id,
         request_id,
         total,
         (unsigned long)config_revision);
```

## Value

Base log:

```c
ESP_LOGD(TAG,
         "[VALUE] dev=%s req=%lu seq=%u id=%s type=%u",
         device_id,
         request_id,
         sequence,
         setting_id,
         setting_type);
```

Then type-safe value:

```c
BOOL   -> value=0/1
INT    -> value=<number>
ENUM   -> value=<index>
STRING -> len=<N>
SECRET -> value=<redacted>
```

## Values end

```c
ESP_LOGI(TAG,
         "[VALUES_END] dev=%s req=%lu expected=%u received=%u"
         " config_rev=%lu",
         ...);
```

## Values commit

```c
ESP_LOGI(TAG,
         "[VALUES_COMMIT] dev=%s count=%u config_rev=%lu",
         ...);
```

---

# 22. Gateway ACK logs

The command service already owns ACK matching.

For Settings ACK, log:

```c
ESP_LOGI("ds_rx",
         "[SETTINGS_ACK] dev=%s req=%lu cmd=%s ok=%d int=%ld",
         device_id,
         request_id,
         command,
         accepted,
         (long)int_value);
```

For mismatch:

```c
ESP_LOGW("ds_rx",
         "[ACK_MISMATCH] dev=%s req=%lu expected_cmd=%s got=%s",
         ...);
```

For unmatched:

```c
ESP_LOGW("ds_rx",
         "[ACK_UNMATCHED] dev=%s req=%lu cmd=%s",
         ...);
```

For transaction commit ACK:

```text
int_value = new_config_revision
```

Log it explicitly:

```c
ESP_LOGI("ds_tx",
         "[COMMIT_ACK] dev=%s tx=%llu new_rev=%lu",
         ...);
```

---

# 23. Gateway protocol error logs

Replace silent `return` in Settings handlers with a reason log during development.

Recommended helper:

```c
#define DS_PROTO_REJECT(dev, req, reason) \
    ESP_LOGW("ds_protocol", \
             "[REJECT] dev=%s req=%lu reason=%s", \
             (dev), (unsigned long)(req), (reason))
```

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

These reason strings are useful in HIL and automated serial-log analysis.

---

# 24. Device RX Settings logs

## Location

```text
components/device_command/device_command.c
```

After `gw_message_decode()` and before Settings dispatch.

### Common log

```c
if (is_settings_command(msg.command)) {
    ESP_LOGI("settings_rx",
             "[SETTINGS_RX] req=%lu cmd=%s dev=%s",
             (unsigned long)(msg.has_request_id ? msg.request_id : 0),
             msg.command,
             msg.has_device_id ? msg.device_id : "<none>");
}
```

### Transaction fields

BEGIN:

```c
ESP_LOGD("settings_rx",
         "[TX_BEGIN_RX] req=%lu tx=%llu expected_rev=%lu",
         ...);
```

SET:

```c
ESP_LOGD("settings_rx",
         "[TX_SET_RX] req=%lu tx=%llu id=%s type=%u",
         ...);
```

Do not log secret plaintext.

COMMIT:

```c
ESP_LOGI("settings_rx",
         "[TX_COMMIT_RX] req=%lu tx=%llu",
         ...);
```

CONFIRM:

```c
ESP_LOGI("settings_rx",
         "[TX_CONFIRM_RX] req=%lu tx=%llu new_rev=%lu",
         ...);
```

---

# 25. Device TX Settings logs

Because Device Settings stream uses specialized encoders that return raw bytes, avoid decoding the bytes again merely for logging.

Log semantic metadata before calling `send_settings_frame()`.

## Schema begin

```c
ESP_LOGI("settings_tx",
         "[SCHEMA_BEGIN_TX] req=%lu total=%u",
         request_id, total);
```

## Schema item

```c
ESP_LOGD("settings_tx",
         "[SCHEMA_ITEM_TX] req=%lu seq=%u/%u id=%s"
         " internal_type=%u wire_type=%u flags=0x%04x",
         request_id,
         i,
         total,
         desc->id,
         (unsigned)desc->type,
         (unsigned)settings_type_to_wire(desc->type),
         (unsigned)desc->flags);
```

The `internal_type` + `wire_type` pair is intentionally logged during development to detect conversion mistakes.

## Option

```c
ESP_LOGD("settings_tx",
         "[SCHEMA_OPTION_TX] req=%lu parent=%u idx=%u label='%s'",
         ...);
```

## Schema end

```c
ESP_LOGI("settings_tx",
         "[SCHEMA_END_TX] req=%lu total=%u",
         ...);
```

## Values begin

```c
ESP_LOGI("settings_tx",
         "[VALUES_BEGIN_TX] req=%lu total=%u config_rev=%lu",
         ...);
```

## Values item

```c
ESP_LOGD("settings_tx",
         "[VALUE_TX] req=%lu seq=%u id=%s internal_type=%u wire_type=%u",
         ...);
```

Type-safe value logging follows the same secret/string policy as Gateway.

## Values end

```c
ESP_LOGI("settings_tx",
         "[VALUES_END_TX] req=%lu total=%u config_rev=%lu",
         ...);
```

---

# 26. Optional hexdump

Raw CBOR hexdumps are useful only when debugging codec mismatch.

Do not enable them permanently at INFO/DEBUG.

Recommended Kconfig:

```text
CONFIG_DEVICE_SETTINGS_TRACE_HEXDUMP
CONFIG_GATEWAY_SETTINGS_TRACE_HEXDUMP
```

When enabled:

```c
ESP_LOG_BUFFER_HEXDUMP("ds_cbor_tx", data, len, ESP_LOG_VERBOSE);
ESP_LOG_BUFFER_HEXDUMP("ds_cbor_rx", data, len, ESP_LOG_VERBOSE);
```

Rules:

```text
[ ] VERBOSE only
[ ] disabled by default
[ ] do not use for secret transaction frames in production
[ ] bounded to actual packet length
```

For Settings TX containing SECRET values, skip raw hexdump entirely.

---

# 27. Example expected serial flow — M1

Gateway:

```text
I ds_worker: [QUEUE] dev=device-01 op=DESCRIBE gen=2 schema_rev=1
I ds_tx: [SETTINGS_TX] dev=device-01 req=42 cmd=describe_settings

D ds_rx: [SETTINGS_RX] dev=device-01 type=settings_begin cmd=describe_settings req=42 total=5
I ds_protocol: [SCHEMA_BEGIN] dev=device-01 req=42 total=5 schema_rev=1
D ds_protocol: [SCHEMA_ITEM] dev=device-01 req=42 seq=0/5 id=enabled type=1 flags=0x0000
D ds_protocol: [SCHEMA_ITEM] dev=device-01 req=42 seq=1/5 id=target_temp type=2 flags=0x0000
D ds_protocol: [SCHEMA_ITEM] dev=device-01 req=42 seq=2/5 id=device_name type=4 flags=0x0000
D ds_protocol: [SCHEMA_ITEM] dev=device-01 req=42 seq=3/5 id=fan_mode type=5 flags=0x0000
D ds_protocol: [SCHEMA_OPTION] dev=device-01 req=42 parent=3 option=0 label='Auto'
D ds_protocol: [SCHEMA_OPTION] dev=device-01 req=42 parent=3 option=1 label='Manual'
D ds_protocol: [SCHEMA_ITEM] dev=device-01 req=42 seq=4/5 id=serial type=4 flags=0x0001
I ds_protocol: [SCHEMA_END] dev=device-01 req=42 expected=5 received=5
I ds_protocol: [SCHEMA_COMMIT] dev=device-01 count=5 rev=1
I ds_rx: [SETTINGS_ACK] dev=device-01 req=42 cmd=describe_settings ok=1 int=0

I ds_worker: [QUEUE] dev=device-01 op=READ gen=3
I ds_tx: [SETTINGS_TX] dev=device-01 req=43 cmd=read_settings
I ds_protocol: [VALUES_BEGIN] dev=device-01 req=43 total=5 config_rev=7
D ds_protocol: [VALUE] dev=device-01 req=43 seq=0 id=enabled type=1 value=1
D ds_protocol: [VALUE] dev=device-01 req=43 seq=1 id=target_temp type=2 value=60
D ds_protocol: [VALUE] dev=device-01 req=43 seq=2 id=device_name type=4 len=7
D ds_protocol: [VALUE] dev=device-01 req=43 seq=3 id=fan_mode type=5 value=1
D ds_protocol: [VALUE] dev=device-01 req=43 seq=4 id=serial type=4 value=<redacted>
I ds_protocol: [VALUES_END] dev=device-01 req=43 expected=5 received=5 config_rev=7
I ds_protocol: [VALUES_COMMIT] dev=device-01 count=5 config_rev=7
I ds_rx: [SETTINGS_ACK] dev=device-01 req=43 cmd=read_settings ok=1 int=0
```

---

# 28. Example expected serial flow — transaction

```text
I ds_tx: [TX_BEGIN] dev=device-01 req=50 tx=12 expected_rev=7
I ds_rx: [SETTINGS_ACK] dev=device-01 req=50 cmd=settings_tx_begin ok=1 int=7

D ds_tx: [TX_SET] dev=device-01 req=51 tx=12 id=target_temp type=2 value=65
I ds_rx: [SETTINGS_ACK] dev=device-01 req=51 cmd=settings_tx_set ok=1 int=0

I ds_tx: [TX_COMMIT] dev=device-01 req=52 tx=12
I ds_rx: [SETTINGS_ACK] dev=device-01 req=52 cmd=settings_tx_commit ok=1 int=8
I ds_tx: [COMMIT_ACK] dev=device-01 tx=12 new_rev=8

I ds_tx: [TX_CONFIRM] dev=device-01 req=53 tx=12 new_rev=8
I ds_rx: [SETTINGS_ACK] dev=device-01 req=53 cmd=settings_commit_confirm ok=1 int=0

I ds_tx: [WAIT_REBOOT] dev=device-01 tx=12 expected_rev=8
I ds_tx: [DISCONNECTED_AFTER_CONFIRM] dev=device-01 tx=12
I ds_tx: [RECONNECT] dev=device-01 tx=12
I ds_tx: [VERIFY_BEGIN] dev=device-01 tx=12
I ds_tx: [VERIFY_OK] dev=device-01 tx=12 rev=8
```

---

# 29. Logging performance rules

Do not let logging become a new memory or BLE timing problem.

Rules:

```text
INFO:
  one line per operation begin/end/ACK/state transition

DEBUG:
  one line per schema item/value/option

VERBOSE:
  optional hexdump

no dynamic allocation for logs
no cJSON formatting in BLE path
no temporary >256 byte log buffer
```

Use direct `ESP_LOGx()` formatting.

During production builds:

```text
INFO enabled
DEBUG optional
VERBOSE disabled
```

During HIL protocol qualification:

```text
DEBUG enabled
VERBOSE only when investigating a specific codec failure
```

---

# 30. Web API behavior

State mapping:

```text
UNSUPPORTED -> 404 settings_unsupported
UNKNOWN     -> 503 settings_not_discovered
DISCOVERING -> 503 settings_discovering
ERROR       -> 503 settings_error
READY       -> 200
```

Return Device metadata:

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

For SECRET:

```json
{
  "secret": true,
  "configured": true
}
```

Never return plaintext secret value.

---

# 31. Required test suites

## Device baseline tests

```text
DEV-WIRE-001..003 capability keys
DEV-WIRE-010..013 schema type conversion
DEV-WIRE-020..023 values type conversion
DEV-TX-001..005 key40 typed decode
DEV-TX-010 confirm routing
DEV-TX-020..024 command strings
```

## Gateway codec tests

```text
DS-CBOR-001 supported key32 integer
DS-CBOR-002 schema revision key33
DS-CBOR-003 schema metadata
DS-CBOR-004 readonly translation
DS-CBOR-005 enum option
DS-CBOR-006 bool key40
DS-CBOR-007 int key40
DS-CBOR-008 string key40
DS-CBOR-009 enum key40
DS-CBOR-010 key order independence
```

## Gateway schema tests

```text
DS-SCHEMA-001 complete bool
DS-SCHEMA-002 int range
DS-SCHEMA-003 string max length
DS-SCHEMA-004 enum options
DS-SCHEMA-005 duplicate ID
DS-SCHEMA-006 sequence gap
DS-SCHEMA-007 total mismatch
DS-SCHEMA-008 no device_id
DS-SCHEMA-009 no snapshot_id
DS-SCHEMA-010 readonly
```

## Values

```text
DS-VAL-001 bool
DS-VAL-002 int
DS-VAL-003 string
DS-VAL-004 enum
DS-VAL-005 missing key40 reject
DS-VAL-006 type mismatch
DS-VAL-007 old snapshot preserved on failure
```

## Worker

```text
DS-WORK-001 capability -> describe
DS-WORK-002 same revision -> read
DS-WORK-003 BUSY retry
DS-WORK-004 ACK-before-READ ordering
DS-WORK-005 disconnect cleanup
DS-WORK-006 duplicate trigger coalescing
```

## Multi-device

```text
DS-MULTI-001 two devices
DS-MULTI-002 builder ownership
DS-MULTI-003 wrong-device frame
DS-MULTI-004 owner disconnect promotes next
```

## Transaction

```text
DS-TX-001 BEGIN
DS-TX-002 BOOL SET
DS-TX-003 INT SET
DS-TX-004 STRING SET
DS-TX-005 ENUM SET
DS-TX-006 readonly local reject
DS-TX-007 range local reject
DS-TX-008 conflict
DS-TX-009 COMMIT
DS-TX-010 CONFIRM
DS-TX-011 WAITING_REBOOT only after confirm ACK
DS-TX-012 post-reboot verify
DS-TX-013 outcome unknown
```

---

# 32. Logging tests

Logs are diagnostic behavior; do not write fragile tests that compare complete formatted log lines.

Test semantic events instead.

## LOG-001 Gateway SEND

Mock send hook and assert instrumentation callback receives:

```text
direction=TX
device
request_id
command
```

## LOG-002 Gateway RX

Feed Settings frame and assert instrumentation event includes:

```text
direction=RX
type
request_id
sequence/total when present
```

## LOG-003 secret redaction

With SECRET descriptor:

```text
no log callback contains plaintext secret
```

If direct log interception is difficult, test the sanitizer helper.

## LOG-004 string policy

Normal string value logger emits length, not full value.

## LOG-005 protocol rejection reason

Malformed frame generates one canonical rejection reason.

---

# 33. HIL acceptance sequence

## HIL-M1

1. Flash Device with D0 fixes.
2. Flash Gateway v2 implementation.
3. Erase relevant Gateway Settings cache.
4. Connect Device.
5. Verify capability log contains Settings support.
6. Verify `describe_settings` SEND log.
7. Verify schema RX logs.
8. Verify `describe_settings` ACK.
9. Verify `read_settings` SEND occurs after ACK.
10. Verify values RX logs.
11. Verify Web API.
12. Verify Web UI.
13. Reboot Device.
14. Verify reconnect refresh.
15. Connect second Device.
16. Verify stream isolation.

## HIL-M2

1. Change writable bool.
2. Save.
3. Verify BEGIN SEND/RX ACK.
4. Verify SET SEND/RX ACK.
5. Verify COMMIT SEND/RX ACK new revision.
6. Verify CONFIRM SEND/RX ACK.
7. Verify Device reboot.
8. Verify Gateway reconnect.
9. Verify READ.
10. Verify post-reboot value/revision.
11. Mark transaction success.

---

# 34. Memory / soak gates

## 100 refresh cycles

Track:

```text
internal free heap
largest internal block
PSRAM free
largest PSRAM block
```

Pass:

```text
no monotonic leak
```

## 100 reconnect cycles

Pass:

```text
no builder leak
no queue leak
no timer leak
```

## 100 save/reboot cycles

M2 gate:

```text
no transaction object leak
no reconciliation timer leak
no pending command leak
```

---

# 35. File-by-file implementation checklist

## Device

### `components/gateway_protocol/gateway_protocol.c`

- [ ] encode key32
- [ ] encode key33
- [ ] typed key40 decode
- [ ] key-order-independent Settings value decode
- [ ] tests

### `components/device_command/device_command.c`

- [ ] schema internal→wire type mapping
- [ ] value internal→wire type mapping
- [ ] route commit_confirm
- [ ] Device RX Settings logs
- [ ] Device TX schema logs
- [ ] Device TX values logs

### `components/gateway_protocol/gateway_settings.c`

- [ ] canonical TX command strings
- [ ] bounded string handling
- [ ] tests

---

## Gateway

### `components/cbor_codec/*`

- [ ] one Device-compatible key table
- [ ] key32/33 decode
- [ ] schema fields
- [ ] option fields
- [ ] typed key40
- [ ] transaction fields

### `components/device_schema/*`

- [ ] support from key32
- [ ] schema revision from key33
- [ ] remove capability flag dependency

### `components/device_settings/device_settings_worker.c`

- [ ] add new worker
- [ ] serialize stream
- [ ] BUSY retry
- [ ] logging

### `components/device_settings/device_settings_protocol.c`

- [ ] no response device_id requirement
- [ ] no snapshot_id requirement
- [ ] request correlation
- [ ] enum options
- [ ] metadata
- [ ] values key40
- [ ] protocol rejection reason logs
- [ ] RX lifecycle logs

### `components/device_command_service/*`

- [ ] Settings payload
- [ ] canonical command validation
- [ ] Settings SEND logs
- [ ] Settings ACK logs

### `components/device_settings/device_settings_transaction.c`

- [ ] tx_id
- [ ] BEGIN
- [ ] SET
- [ ] COMMIT
- [ ] CONFIRM
- [ ] reboot reconcile
- [ ] transaction logs

### `main/main.c`

- [ ] schema commit listener -> Settings coordinator

### Web API/UI

- [ ] state distinction
- [ ] title/group/unit
- [ ] readonly/secret/advanced
- [ ] enum options
- [ ] secret redaction

---

# 36. Recommended commit sequence

```text
1. fix(device-protocol): emit Settings support and schema revision
2. fix(device-settings): map internal setting types to canonical wire types
3. fix(device-settings): decode typed Settings transaction values
4. fix(device-settings): route commit-confirm and normalize tx commands
5. test(protocol): freeze Device actual-pipeline golden vectors

6. fix(gateway-cbor): align Settings keys with Device contract
7. fix(gateway-schema): consume Settings support/schema revision
8. feat(gateway-settings): add serialized Settings worker
9. fix(gateway-settings): decode schema metadata and enum options
10. fix(gateway-settings): decode typed Settings values
11. feat(settings-log): add safe Settings SEND/RX diagnostics
12. test(settings): add cross-repo/multidevice/fault coverage

13. feat(settings-tx): add Settings command payload
14. feat(settings-tx): align BEGIN/SET/COMMIT/CONFIRM
15. feat(settings-tx): add reboot reconciliation
16. test(hil): qualify M1/M2 + memory soak
```

---

# 37. Release gates

## M1

```text
[ ] Device D0 complete
[ ] actual Device pipeline golden vectors frozen
[ ] Gateway key map aligned
[ ] Settings support recognized
[ ] schema revision recognized
[ ] describe automatically sent
[ ] schema stream committed
[ ] enum options committed
[ ] read sent only after describe ACK
[ ] values stream committed
[ ] Web UI correct
[ ] Settings SEND/RX logs available
[ ] secrets not leaked in logs
[ ] 2-device HIL passes
[ ] 100 refresh soak passes
```

## M2

```text
[ ] typed key40 transaction decode
[ ] BEGIN
[ ] SET all supported types
[ ] COMMIT
[ ] CONFIRM
[ ] reboot
[ ] reconnect
[ ] revision verify
[ ] value verify
[ ] conflict test
[ ] timeout/outcome-unknown test
[ ] 100 save/reboot soak passes
```

---

# 38. Final target behavior

```text
Device
  declared Protocol v4 contract
          │
          ▼
Device runtime emits canonical Settings frames
          │
          ▼
BLE
          │
          ▼
Gateway codec
          │
          ▼
Settings coordinator
          │
          ├─ schema cache
          ├─ values cache
          ├─ transaction state
          └─ diagnostic logs
          │
          ├────────► Web UI
          ├────────► REST API
          └────────► MCP / Xiaozhi MCP
```

Every Settings operation must be traceable from logs using:

```text
device_id
request_id
command
stream sequence
transaction_id when applicable
schema/config revision when applicable
```

and no secret plaintext may appear in those logs.

---

# 39. Summary of v2 corrections vs v1.1

v2 changes the previous document in these important ways:

```text
1. M1 is no longer described as Gateway-only.
2. Device runtime stabilization is mandatory first.
3. Device internal type != wire type is explicitly enforced.
4. key32/33 actual encoder gap is included.
5. key40 transaction decode is included before M2.
6. commit_confirm outer dispatch gap is included.
7. golden vectors must include actual Device command pipeline output.
8. string Settings command buffer recommendation reduced to 64 bytes.
9. gw_settings_view may remain, but duplicate key definitions may not.
10. SEND/RX Settings logging is mandatory on Gateway and Device.
11. secret/string logging rules are defined.
12. canonical rejection reasons are defined for protocol debugging.
