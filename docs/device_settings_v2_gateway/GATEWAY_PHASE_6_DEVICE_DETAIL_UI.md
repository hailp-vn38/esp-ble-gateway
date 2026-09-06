# Gateway Phase G6 — Device Detail Generic Settings UI


**Repo:** `hailp-vn38/esp-ble-gateway`  
**Baseline:** `dev-ws`  
**Target:** ESP32-S3  
**Feature:** Device Settings v2  
**Primary constraint:** bảo vệ internal SRAM; dữ liệu Settings lớn/long-lived phải dùng PSRAM-required.


## Goal

Render/edit discovered Settings generically, không hardcode product-specific IDs và không bloat existing `devices.js`.

## Files

```text
components/web_server/www_src/dashboard/views/device_detail.html
components/web_server/www_src/dashboard/js/features/device_settings.js   # new
components/web_server/www_src/dashboard/js/core/api.js
styles/i18n as applicable
```

## Placement

Add `Device Settings` card in Device Detail, recommended between Features and AI/MCP.

## Renderer

| Type | Control |
|---|---|
| BOOL | switch/checkbox |
| INT | number input; slider only if range UX useful |
| STRING | text input |
| ENUM | select |
| SECRET | password/replacement control with KEEP/SET/CLEAR |
| READONLY | display only |

Use metadata:

```text
title, group, unit, min, max, step, options, readonly, advanced
```

## Dirty model

Keep UI baseline values + dirty changes only. Save sends only changed writable fields.

`expected_revision` is the revision loaded with baseline.

If revision changes via WS before save:

- mark data stale;
- block blind save or refresh and ask user to reapply dirty edits according to UX policy;
- never silently overwrite newer config.

## Save lifecycle

```text
click Save
-> client validation
-> PUT -> 202 operation_id
-> disable Save/fields appropriately
-> state Updating
-> observe WS/poll operation
-> device disconnect expected -> Rebooting
-> reconnect -> Verifying
-> GET settings refresh
-> success banner/state
```

No aggressive continuous polling; operation endpoint may be polled bounded while WS provides state hints.

## Secret UX

Default existing configured secret:

```text
configured ••••••
action = KEEP
```

Explicit buttons/actions for Replace / Clear. Empty field alone does not clear.

## Frontend module boundary

`device_settings.js` owns:

- load/render;
- dirty state;
- validation;
- save operation tracking;
- settings WS handling;
- reconnect refresh.

Existing `devices.js` only delegates lifecycle/detail-device changes.

## Tests

### Render
- [ ] each type.
- [ ] groups/unit/options.
- [ ] readonly.
- [ ] unsupported hidden/disabled state.

### Dirty
- [ ] unchanged fields not sent.
- [ ] revert to original removes dirty.
- [ ] revision update blocks stale save.

### Lifecycle
- [ ] 202 queued.
- [ ] busy/conflict.
- [ ] expected reboot disconnect doesn't show generic fatal error.
- [ ] reconnect refresh.
- [ ] OUTCOME_UNKNOWN shows verifying, then resolves.

### Secret
- [ ] existing secret not injected into DOM as plaintext.
- [ ] KEEP sends no replacement value.
- [ ] CLEAR explicit.
- [ ] SET only new value.

## Checklist

- [ ] No setting IDs hardcoded.
- [ ] New module, not further bloating `devices.js`.
- [ ] Accessible labels/units.
- [ ] Save disabled when no dirty changes.
- [ ] Device offline state handled.
- [ ] Operation survives page refresh via operation status endpoint where possible.

## Exit gate

Reference device demo can be fully configured from Device Detail and verified after reboot without console/manual BLE commands.
