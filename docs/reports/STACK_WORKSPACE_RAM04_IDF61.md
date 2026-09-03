# RAM-04 stack workspace report

Date: 2026-09-03  
Target: ESP32-S3 / ESP-IDF 6.1-rc1

## Changes

- `devices_get` no longer places a `dispatch_result_t` (~4 KiB) on the HTTPD
  stack; it uses a bounded PSRAM-preferred allocation and frees it after JSON
  parsing.
- HTTPD gateway/provisioning stacks remain internal (12 KiB / 8 KiB).
- BLE notify, identity and reconnect supervisor stacks remain internal.
- `httpd_config_t.max_open_sockets` is assigned directly to 7.

## Runtime evidence

The final idle STA snapshot reported 20 tasks, application minimum stack
high-water 1,864 B, unknown count 0, and command worker minimum 2,944 B.
No allocation failure, stack panic, watchdog or websocket error was observed.

## Call-chain budget

| Path | Large workspace | Placement |
|---|---|---|
| device list REST | `dispatch_result_t` | PSRAM-preferred bounded heap |
| MCP tools/call | persistent result buffer | PSRAM-preferred bounded heap |
| schema/exposure refresh | schema snapshot copy-out | bounded caller workspace/internal lock-safe data |
| BLE notify/ACK | wire/event buffers and task stack | internal |
| Wi-Fi scan/config | request body + worker stack | internal |

100-cycle canary/OTA/NVS/Wi-Fi stress remains part of RAM-06 qualification.
