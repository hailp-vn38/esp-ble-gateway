# Gateway Phase G3 — Transaction Service & Async Operation Core


**Repo:** `hailp-vn38/esp-ble-gateway`  
**Baseline:** `dev-ws`  
**Target:** ESP32-S3  
**Feature:** Device Settings v2  
**Primary constraint:** bảo vệ internal SRAM; dữ liệu Settings lớn/long-lived phải dùng PSRAM-required.


## Goal

Orchestrate one atomic Settings update per device without bloating command queues/pending slots.

## Files

```text
components/device_settings/device_settings_transaction.c
components/device_settings/device_settings_operation.c
components/device_command_service/include/device_command_service.h
components/device_command_service/... implementation
```

## Command service

Add origin:

```text
DEVICE_CMD_ORIGIN_SETTINGS
```

Nhưng không copy full Settings payload vào fixed pending slot.

Queue item target:

```c
struct settings_cmd_queue_item {
    uint8_t op;
    uint8_t flags;
    uint16_t device_index;
    uint32_t request_id;
    void *payload;   // owned PSRAM object
};
```

Target `<= 32 B`.

Pending target contains only matching/deadline/context metadata `<= 64 B`.

## Transaction object

Variable data/change strings live in PSRAM-required allocation. HTTP caller ownership ends after transaction service deep-copies accepted request.

Fields:

```text
operation_id
transaction_id
device id/index
expected_revision
changes[] PSRAM
next change index
state
new_revision
error/status
```

## One active op/device

Second save while active:

- reject BUSY/409/423-like mapping per API phase;
- never interleave SET from two transactions.

Different devices may execute concurrently subject to BLE connection architecture.

## Sequence

### Prevalidate
Against acquired schema snapshot:

- id exists;
- writable;
- type;
- local min/max/length/enum.

This improves UX only; device remains final authority.

### BEGIN

Send expected config revision from UI/API snapshot. Device conflict -> operation `CONFLICT`; refresh values.

### SET

Send **sequentially**, one ACK before next SET. This limits pending slots/memory and preserves ordering.

### COMMIT

On ACK with new revision:

- record expected new revision;
- advance to Phase G4 confirm/reboot flow;
- do not mark final success yet.

## Ownership tests

- [ ] Request strings copied to PSRAM before HTTP buffer lifetime ends.
- [ ] Queue item pointer freed exactly once on all errors.
- [ ] cancel/disconnect path frees unconsumed changes.
- [ ] scalar-only changes avoid unnecessary per-item heap allocation where practical.

## Functional tests

- [ ] one BOOL.
- [ ] multiple mixed types.
- [ ] local validation fail sends no BEGIN.
- [ ] device revision conflict.
- [ ] device rejects one SET -> no COMMIT, ABORT best-effort.
- [ ] device commit error -> FAILED, old values remain.
- [ ] second operation same device -> BUSY.
- [ ] operations different devices do not corrupt state.

## Memory tests

Measure delta with max change request:

- internal heap should only change by small control metadata;
- variable arrays/strings visible in PSRAM delta;
- after completion both return near baseline.

## Checklist

- [ ] No full `gw_message_t` copies added to queue for Settings.
- [ ] SET serialized.
- [ ] Device-side revision conflict is respected.
- [ ] Operation registry bounded.
- [ ] Transaction variable allocations external-required.
- [ ] All ownership/free paths documented.

## Exit gate

Raw/backend transaction can update max supported changes atomically with no queue growth beyond targets and no internal heap fallback.
