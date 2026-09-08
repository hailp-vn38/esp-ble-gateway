# Phase H1 — HIL, Fault Injection, Memory and Soak

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

Qualify Settings implementation for ESP32-S3 resource constraints and real BLE/Wi-Fi concurrency.

# 2. Hardware targets

Gateway:

```text
ESP32-S3
512 KB SRAM
up to 8 MB PSRAM
Wi-Fi + BLE coexistence
```

Device:

```text
ESP32 BLE peripheral running current D0-fixed baseline
```

# 3. HIL tests

## H1-HIL-001 cold boot

```text
Gateway clean boot
Device online
automatic capability/schema/values discovery
UI becomes READY
```

## H1-HIL-002 Device reboot

Gateway must recover without manual refresh.

## H1-HIL-003 disconnect during schema

Old committed schema remains valid.

## H1-HIL-004 disconnect during values

Old committed values remain valid.

## H1-HIL-005 two Devices

No builder/data mixing.

## H1-HIL-006 Wi-Fi/Web UI load

Generate HTTP/WS traffic during Settings discovery.

Pass:

```text
no watchdog
no BLE stream corruption
no HTTP starvation
```

## H1-HIL-007 transaction save/reboot

Full M2 path.

# 4. Fault injection

```text
truncated CBOR
trailing CBOR
invalid type
missing key40
huge total
duplicate ID
sequence gap
option overflow
string pool exhaustion
PSRAM allocation failure
queue full
command BUSY
command timeout
wrong-device notification
```

Every failure must:

```text
not crash
not leak
not commit partial snapshot
release ownership
emit bounded diagnostic log
```

# 5. Memory instrumentation

At test checkpoints record:

```text
heap_caps_get_free_size(MALLOC_CAP_INTERNAL)
heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)
heap_caps_get_free_size(MALLOC_CAP_SPIRAM)
heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)
uxTaskGetStackHighWaterMark(...)
```

# 6. 100 refresh soak

Loop:

```text
DESCRIBE
READ
```

100 cycles.

Pass:

```text
no monotonic memory leak
no growing queue depth
no stale pending command
no builder ownership leak
```

Suggested allowance:

```text
<=1 KB one-time settling delta
0 B/cycle continuing leak
```

# 7. 100 reconnect soak

Repeat Device disconnect/reconnect.

Pass:

```text
schema/value caches remain bounded
worker generation logic stable
no timer leak
```

# 8. 100 save/reboot soak

M2 only.

Pass:

```text
no transaction object leak
no pending-slot leak
no reconciliation timer leak
```

# 9. Maximum schema test

Create maximum supported profile:

```text
max setting count
max enum option count
max title/group/unit strings
max STRING lengths allowed by protocol
```

Pass:

```text
schema commit succeeds
PSRAM allocation within budget
task stack safe
```

# 10. Fragmentation test

Alternate schema revisions with different allocation sizes.

Check:

```text
largest free PSRAM block remains capable of allocating max schema
```

# 11. Logging under soak


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


Production soak:

```text
INFO
```

Protocol debugging:

```text
DEBUG temporarily
```

Avoid VERBOSE hexdump for long soak tests.

# 12. Final release gate

```text
[ ] all HIL tests pass
[ ] all fault tests safe
[ ] 100 refresh cycles no leak
[ ] 100 reconnect cycles no leak
[ ] 100 save/reboot cycles no leak
[ ] no WDT reset
[ ] no cross-device Settings corruption
[ ] no secret leakage
[ ] stack high-water marks acceptable
```
