# Gateway Phase G2 — BLE Discovery, Schema/Values Builders & Cache State ✅ DONE (2026-09-06)


**Repo:** `hailp-vn38/esp-ble-gateway`  
**Baseline:** `dev-ws`  
**Target:** ESP32-S3  
**Feature:** Device Settings v2  
**Primary constraint:** bảo vệ internal SRAM; dữ liệu Settings lớn/long-lived phải dùng PSRAM-required.


## Goal

Consume device Settings streams and atomically publish PSRAM-backed immutable snapshots.

## Inputs

```text
settings_begin/item/option/end
settings_values_begin/value/end
```

## Discovery flow

```text
capability says supported
-> state DISCOVERING
-> send describe_settings
-> build staging schema in PSRAM
-> validate totals/revision/IDs/options
-> commit schema pointer swap
-> send read_settings
-> build staging values in PSRAM
-> validate config_revision/count/types
-> commit values pointer swap
-> state READY
```

## Builder requirements

- growth only in PSRAM;
- bounds from protocol hard limits;
- reject duplicate setting ids;
- option references must resolve;
- value setting id/type must match schema;
- secret stores configured bit only;
- no partial committed snapshot.

## Reconnect/cache policy

Device remains authoritative. Gateway may keep last schema/value RAM snapshot across short disconnect only if state marks stale/not connected; after reconnect, refresh/revision-check before writes.

Do not persist current values to gateway NVS.

## Integration with device schema

Device Detail can expose settings summary:

```json
{
  "supported": true,
  "state": "ready",
  "count": 7,
  "schema_revision": 2,
  "config_revision": 9
}
```

Do not inject all values into existing `/api/devices/detail` response.

## Tests

### Parser/build
- [x] zero settings.
- [x] all 4 types.
- [x] enum options.
- [x] duplicate id reject.
- [x] mismatched total reject.
- [x] missing end reject.
- [x] invalid value type reject.
- [x] secret plaintext ignored/rejected by contract.

### Disconnect
- [x] disconnect mid-schema => staging free, committed unchanged.
- [x] disconnect mid-values => staging values free, committed unchanged.
- [x] reconnect => clean rediscovery.

### PSRAM fail
Inject fail at each builder growth step:

- [x] state becomes ERROR/appropriate failure.
- [x] old committed snapshot preserved.
- [x] no internal fallback.
- [x] command/features path still works.

### 100-cycle discovery
Measure before/after:

```text
free internal heap
minimum free internal heap
largest internal block
free PSRAM
largest PSRAM block
```

No monotonic leak trend.

## Checklist

- [x] Discovery uses settings-specific codec, not expanded `gw_message_t`.
- [x] Snapshot commit is all-or-nothing.
- [x] HTTP reader lifetime API used everywhere.
- [x] Device unsupported path sends no settings commands.
- [x] Values are not gateway-NVS persisted.

## Exit gate

100 repeated discovery/value refresh cycles pass with stable internal heap and valid concurrent snapshot reads.
