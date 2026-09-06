# Gateway Phase G7 — Internal SRAM Protection, Soak, Fuzz & Release Hardening


**Repo:** `hailp-vn38/esp-ble-gateway`  
**Baseline:** `dev-ws`  
**Target:** ESP32-S3  
**Feature:** Device Settings v2  
**Primary constraint:** bảo vệ internal SRAM; dữ liệu Settings lớn/long-lived phải dùng PSRAM-required.


## Goal

Chứng minh feature không làm gateway mất ổn định khi Wi-Fi + BLE + WebSocket + HTTP cùng hoạt động.

## Metrics bắt buộc

At checkpoints capture:

```text
heap_caps_get_free_size(MALLOC_CAP_INTERNAL)
heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)
heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)
heap_caps_get_free_size(MALLOC_CAP_SPIRAM)
heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)
```

Also task stack high-water marks for Settings/Web/BLE-related workers.

## Checkpoints

1. boot idle;
2. Wi-Fi connected;
3. BLE connected;
4. device schema normal feature discovery done;
5. Settings schema/value loaded;
6. WebSocket client connected;
7. max Settings GET response active/completed;
8. max-change update active;
9. during reboot/reconnect;
10. after 100 cycles.

## Hard acceptance

- persistent control metadata `<= 64–128 B/device` target;
- queue item `<= 32 B`;
- pending metadata `<= 64 B`;
- discovery extra internal delta target `<= ~1 KiB` excluding unavoidable lower-layer BLE callbacks measured separately;
- large/long-lived Settings internal fallback = `0 B`;
- shared `gw_message_t` size growth due Settings strings = `0 B`.

## PSRAM exhaustion

Force external-required allocator fail during:

- schema build;
- values build;
- PUT deep-copy;
- operation changes;
- large GET workspace if applicable.

Expected:

```text
Settings request fails cleanly
no crash/reboot
no internal heap large fallback
BLE feature/control still works
Wi-Fi/WS remain operational
```

## Fragmentation

Repeated variable schema/string lengths, GET/update cycles, reconnects. Track largest PSRAM block + internal largest block for downward trend.

## Soak

### Discovery x100
No leaks/UAF/state stuck.

### Save/reboot x100
Verify revision exactly increments once and operation resolves.

### Random fault
Randomly inject disconnect at BEGIN/SET/COMMIT/CONFIRM/reboot reconnect boundaries. No gateway deadlock or permanent busy operation.

## Concurrency

- multiple simultaneous HTTP GET readers while BLE swaps snapshots;
- WS connected during updates;
- settings update one device while reading another;
- normal feature commands during Settings idle and, where policy allows, update.

## Fuzz

Malformed Settings response frames:

- truncated;
- duplicate/missing keys;
- impossible counts;
- invalid offsets/types;
- oversized strings/options.

Builder must reject without committed snapshot corruption.

## Observability

Add compact diagnostics/counters (not secrets):

```text
settings_discovery_success/fail
settings_psram_alloc_fail
settings_tx_success/fail/conflict
settings_outcome_unknown
settings_reconcile_success/fail
```

Avoid per-frame verbose production logs.

## Final checklist

- [ ] internal heap baseline compared before/after feature.
- [ ] max HTTP GET no large internal spike.
- [ ] 100 discovery pass.
- [ ] 100 save/reboot pass.
- [ ] PSRAM exhaustion pass.
- [ ] concurrent reader/snapshot swap pass.
- [ ] protocol fuzz pass.
- [ ] old device regression pass.
- [ ] Wi-Fi/BLE coexistence regression pass.
- [ ] no secret in logs/WS/API.

## Exit gate

All hard memory gates and cross-repo release matrix pass on target ESP32-S3 configuration.
