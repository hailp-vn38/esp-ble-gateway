# Gateway Phase G3 — Transaction Service & Async Operation Core ✅ DONE (2026-09-06)

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
components/device_command_service/device_command_request.c
```

## Command service

Added origin:

```text
DEVICE_CMD_ORIGIN_SETTINGS
```

Validation accepts: `set_settings`, `commit_settings`, `describe_settings`, `get_settings`.

No full Settings payload copied into fixed pending slot. Transaction service submits compact command-service requests (command name + setting_id + value) one at a time.

## Transaction object

Variable data/change strings live in PSRAM-required allocation. HTTP caller ownership ends after transaction service deep-copies accepted request.

Fields:

```text
device_id
state (DS_TX_*)
changes[] PSRAM (deep-copied)
change_count / next_change_index
expected_config_rev
new_config_rev
completion / context
```

## One active op/device

Second save while active:

- returns `ESP_ERR_INVALID_STATE` (BUSY mapping);
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

- [x] Request strings copied to PSRAM before HTTP buffer lifetime ends.
- [x] Queue item pointer freed exactly once on all errors.
- [x] cancel/disconnect path frees unconsumed changes.
- [x] scalar-only changes avoid unnecessary per-item heap allocation where practical.

## Functional tests

- [x] one BOOL.
- [x] multiple mixed types.
- [x] local validation fail sends no BEGIN.
- [x] device revision conflict.
- [x] device rejects one SET -> no COMMIT, ABORT best-effort.
- [x] device commit error -> FAILED, old values remain.
- [x] second operation same device -> BUSY.
- [x] operations different devices do not corrupt state.

## Memory tests

Measure delta with max change request:

- internal heap should only change by small control metadata;
- variable arrays/strings visible in PSRAM delta;
- after completion both return near baseline.

## Checklist

- [x] No full `gw_message_t` copies added to queue for Settings.
- [x] SET serialized.
- [x] Device-side revision conflict is respected.
- [x] Operation registry bounded.
- [x] Transaction variable allocations external-required.
- [x] All ownership/free paths documented.

## Exit gate

Raw/backend transaction can update max supported changes atomically with no queue growth beyond targets and no internal heap fallback.
