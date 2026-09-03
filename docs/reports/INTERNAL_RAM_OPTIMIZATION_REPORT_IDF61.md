# Internal RAM optimization report — ESP32-S3 / ESP-IDF 6.1

Date: 2026-09-03  
Target: ESP32-S3, 16 MiB flash, 8 MiB Octal PSRAM  
Completed phases: RAM-01, RAM-02, RAM-03, RAM-04  
Qualification phase: RAM-06 LAN subset completed; RAM-05 OTA and remaining
RAM-06 hardware gates open.

## Before/after summary

| Metric | RAM-01 baseline | RAM-03/04 final | Change |
|---|---:|---:|---:|
| DIRAM static used | 164,019 B | 157,235 B | -6,784 B |
| `.bss` | 46,776 B | 39,992 B | -6,784 B |
| Internal largest runtime block | 36,864 B | 63,488 B | +26,624 B |
| PSRAM largest runtime block | 7,602,176 B | 7,733,248 B | +131,072 B |
| Application stack minimum | 1,272 B | 1,864 B | +592 B |

The static reduction comes from bounding ACK pending slots independently from
the 16-device registry (16 → 2) and moving the device-list HTTP workspace off
the HTTPD stack. cJSON uses PSRAM-preferred allocation with floor-protected
fallback.

## Evidence links

- [RAM-01 baseline](INTERNAL_RAM_BASELINE_IDF61.md)
- [RAM-04 stack report](STACK_WORKSPACE_RAM04_IDF61.md)
- [RAM-06 LAN qualification](RAM06_QUALIFICATION_IDF61.md)
- Production build tree: `build-ram02/`
- Test build tree: `test/build-ram-test/`

## Release boundary

The firmware is not release-ready until RAM-05 OTA pending-image validation,
9-link BLE, provisioning/power-cycle, TLS reconnect, heap-integrity and
24-hour soak evidence are collected. Missing evidence is tracked in the plan,
not treated as a pass.
