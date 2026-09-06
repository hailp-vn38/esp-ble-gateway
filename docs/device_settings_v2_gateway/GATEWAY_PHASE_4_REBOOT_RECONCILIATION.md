# Gateway Phase G4 — Commit Confirmation, Reboot & Outcome Reconciliation ✅ DONE (2026-09-06)


**Repo:** `hailp-vn38/esp-ble-gateway`  
**Baseline:** `dev-ws`  
**Target:** ESP32-S3  
**Feature:** Device Settings v2  
**Primary constraint:** bảo vệ internal SRAM; dữ liệu Settings lớn/long-lived phải dùng PSRAM-required.


## Goal

Correctly handle device reboot and ambiguous network outcomes after persistence.

## Normal flow

```text
COMMIT ACK(new_revision)
-> send COMMIT_CONFIRM(transaction_id,new_revision)
-> operation WAITING_REBOOT
-> WS/device.connection false expected
-> reconnect
-> refresh settings values
-> verify revision+changed values
-> SUCCEEDED
```

Do not declare final success solely from COMMIT ACK if product contract requires post-reboot verification.

## OUTCOME_UNKNOWN

Enter when gateway cannot know whether COMMIT persisted, e.g. disconnect/timeout around commit boundary.

Store bounded operation evidence:

```text
old_revision
expected new revision (if known)
requested changes
transaction_id
```

Changes already live in PSRAM operation object.

## Reconciliation algorithm

After reconnect/read:

### revision == expected new revision
Verify requested non-secret values. Secret verify via configured semantics only. If match -> SUCCEEDED.

### revision == old revision
Commit did not persist -> FAILED (or retry only if explicit policy; V2 default no automatic write retry to avoid duplicate side effects).

### revision other
External/newer change occurred -> CONFLICT / outcome requiring refresh; never overwrite automatically.

If COMMIT ACK was lost and expected new revision can be inferred as `old+1`, still verify values before success.

## WebSocket/event integration

Publish compact event only:

```text
device_id
settings state
config_revision
operation_id/status where needed
```

Never publish full settings or secret values.

## Tests

Fault injection:

- [x] normal ACK+confirm.
- [x] drop COMMIT ACK after device persisted.
- [x] drop COMMIT_CONFIRM.
- [x] disconnect immediately after persisted commit.
- [x] disconnect before COMMIT reaches device.
- [x] reconnect with revision `old+1` + matching values => success.
- [x] reconnect with old revision => failure.
- [x] reconnect with unrelated later revision => conflict.
- [x] page/API polling survives gateway operation state transitions.

## Memory checklist

- [x] Reconciliation does not duplicate whole schema.
- [x] Operation stores only requested changes, not full values snapshot.
- [x] Compact WS event doesn't enlarge global event struct materially; prefer union/reuse if needed.
- [x] Completed operation history bounded/TTL and large detail not retained indefinitely.

## Exit gate

No false-failure when commit persisted but ACK lost, and no false-success when revision/value verification fails.
