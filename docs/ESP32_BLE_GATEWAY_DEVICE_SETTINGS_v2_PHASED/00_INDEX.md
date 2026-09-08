# ESP32 BLE Settings v2 — Phased Implementation Index

**Document set:** ESP32 BLE Gateway ↔ Device Settings v2
**Version:** v2.1 phased documents
**Date:** 2026-09-07
**Gateway repo:** `hailp-vn38/esp-ble-gateway`
**Gateway branch:** `dev-device-settings`
**Device repo:** `hailp-vn38/esp-ble-device`
**Device branch:** `main`
**Protocol:** ESP-GATT Protocol v4

---

# 1. Goal

This document set replaces the previous monolithic v2 document.

Each implementation phase is isolated into one file so a developer can execute and verify one phase without searching through a 2,000+ line document.

The authoritative rule is:

```text
Device declared Protocol v4 contract
        ↓
Device runtime must conform to it
        ↓
Gateway must conform to the same contract
```

# 2. Required execution order

```text
D0  Device baseline stabilization
 ↓
G0  Cross-repository golden vectors
 ↓
G1  Gateway codec alignment
 ↓
G2  Gateway Settings capability detection
 ↓
G3  Gateway Settings worker/coordinator
 ↓
G4  Gateway schema discovery
 ↓
G5  Gateway values discovery
 ↓
M1  Read/display acceptance
 ↓
G6  Command-service Settings payload
 ↓
G7  Transaction flow
 ↓
M2  Writable Settings acceptance
 ↓
H1  HIL/fault/memory/soak qualification
```

# 3. Documents

| Order | File | Repository focus | Purpose |
|---:|---|---|---|
| 1 | `01_D0_DEVICE_BASELINE_STABILIZATION.md` | Device | Make Device runtime conform to its own protocol |
| 2 | `02_G0_CROSS_REPO_GOLDEN_VECTORS.md` | Both | Freeze actual compatible bytes |
| 3 | `03_G1_GATEWAY_CODEC_ALIGNMENT.md` | Gateway | Replace incompatible Settings wire interpretation |
| 4 | `04_G2_SETTINGS_CAPABILITY_DETECTION.md` | Gateway | Detect support/revision correctly |
| 5 | `05_G3_SETTINGS_WORKER_COORDINATOR.md` | Gateway | Execute queued Settings operations safely ✅ DONE (2026-09-07) |
| 6 | `06_G4_SETTINGS_SCHEMA_DISCOVERY.md` | Gateway | Decode and commit descriptors/options ✅ DONE (2026-09-07) |
| 7 | `07_G5_SETTINGS_VALUES_DISCOVERY.md` | Gateway | Decode and commit typed values ✅ DONE (2026-09-08) |
| 8 | `08_M1_READ_DISPLAY_ACCEPTANCE.md` | Both | End-to-end read/display gate |
| 9 | `09_G6_COMMAND_SERVICE_SETTINGS_PAYLOAD.md` | Gateway | Add typed Settings command payload |
| 10 | `10_G7_SETTINGS_TRANSACTION_FLOW.md` | Gateway + Device | BEGIN/SET/COMMIT/CONFIRM/reconcile |
| 11 | `11_M2_WRITABLE_ACCEPTANCE.md` | Both | End-to-end writable gate |
| 12 | `12_H1_HIL_FAULT_MEMORY_SOAK.md` | Both | Final hardening and release qualification |
| — | `90_SHARED_SETTINGS_LOGGING_CONTRACT.md` | Both | Shared SEND/RX logging contract |
| — | `91_SHARED_PROTOCOL_CONTRACT.md` | Both | Single source protocol reference |

# 4. Phase ownership

## Device-only

```text
D0
```

## Gateway-only

```text
G1
G2
G3
G4
G5
G6
```

## Both repositories

```text
G0
M1
G7
M2
H1
```

# 5. Merge policy

Do not merge downstream phase work as "complete" when the upstream exit gate is not satisfied.

Examples:

```text
G1 may be developed in parallel with D0
but G0 fixtures must not be frozen until D0 passes.

M1 must not be declared complete until D0 + G1..G5 all pass.

M2 must not be declared complete until G6 + G7 + Device transaction fixes pass.
```

# 6. Recommended branch strategy

Device:

```text
dev-settings-v2-d0
```

Gateway:

```text
dev-device-settings
```

Recommended commit granularity is documented in each phase file.

# 7. Global non-negotiable rules

- Protocol stays v4.
- Device declared key map is authoritative.
- Internal Device setting enum values are not wire values.
- Gateway must not require `device_id` or `snapshot_id` inside Settings stream frames.
- `request_id` is the primary Settings operation correlation field.
- Settings streams must be serialized while global builders are used.
- `read_settings` is the outbound values request.
- Current values stream may respond with `command=get_settings`; final ACK echoes `read_settings`.
- Secret plaintext must never appear in logs, API payloads, or diagnostics.
