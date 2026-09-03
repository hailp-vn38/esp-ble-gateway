# Internal RAM baseline — ESP32-S3 / ESP-IDF 6.1

**Status:** RAM-01 completed with explicitly excluded stress scenarios  
**Date:** 2026-09-03  
**Branch:** `ram/01-observability-baseline`  
**Commit:** `b2f4f7709171dfec623bd5077f0f7c04adcae2a8`  
**Target:** ESP32-S3, 16 MiB flash, 8 MiB Octal PSRAM  
**IDF:** `v6.1-rc1`

## Build evidence

Build directory riêng: `build-ram-baseline`.

| Artifact | SHA-256 |
|---|---|
| `esp32_ble_gateway.elf` | `2e0933b66e8ed7993fe5a3df4ef1572c0f56bfcb6ca6f7e853a96b843f9f8337` |
| `esp32_ble_gateway.bin` | `14fc0fb515d8cbc98f25a1aeec8afe65df244040bbb71632898b933cab16a0d9` |

Static size from `idf.py size`:

| Region | Used | Total | Remain |
|---|---:|---:|---:|
| DIRAM | 164,019 B | 341,760 B | 177,741 B |
| `.bss` | 46,776 B | — | — |
| `.data` | 23,384 B | — | — |
| App binary | 1,477,541 B reported image | 5 MiB partition | 72% partition free |

Effective memory settings include `CONFIG_BT_NIMBLE_MAX_CONNECTIONS=9`,
`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=1024`, internal floor `65,536 B`, and
largest-block floor `32,768 B`.

## Hardware smoke evidence

Flashed via `/dev/cu.usbmodem101`; esptool verified bootloader, partition table,
OTA data and app image. The board reported ELF-matching firmware:
`v1.0.0-41-gb2f4f77-dirty`.

Endpoint: `http://192.168.1.114/api/status` returned `200 OK` after reboot.

| Metric | Value |
|---|---:|
| Device / connected / BLE links | 1 / 1 / 1 |
| Internal current free | 80,799 B |
| Internal minimum free | 39,916 B |
| Internal largest block | 36,864 B |
| PSRAM current free | 7,715,112 B |
| PSRAM minimum free | 7,705,124 B |
| PSRAM largest block | 7,602,176 B |
| External allocation success | 9 |
| External allocation failure | 0 |
| Internal fallback attempt/success/rejected | 0 / 0 / 0 |
| Command worker stack minimum | 2,948 B |
| Application task stack minimum / task | 1,272 B / `nimble_host` |
| Application task unknown / system tasks | 0 / 7 (system minimum 388 B) |
| BLE notify queue HWM / schema queue HWM | 0 / 1 |

The internal minimum-free value is below the release gate of 65,536 B. This is
an observation requiring later stress qualification, not a release claim.

## Scope and remaining work

This report records build, flash, test-app build/flash, and one STA runtime
smoke snapshot. Per request, provisioning stress and 0/3/6/9 BLE-link stress
were excluded; the connected 1-link path was observed. MCP `tools/list` could
be exercised after supplying the runtime bearer token: it returned 2 static
tools and 0 dynamic tools; 8/16/32 dynamic-tool fixtures are not configured.
An authenticated `tools/call get_status` also returned a valid JSON-RPC result.
No claim is made for soak or stress gates.

## RAM-03 bounded-pool evidence

`DEVICE_REQUEST_MAX_PENDING` is now independent from the 16-device registry
and defaults to 2, matching the two command-executor workers. Static usage
decreased by 6,784 B versus RAM-01 (`.bss`: 46,776 B → 39,992 B; DIRAM:
164,019 B → 157,235 B). The final production image was flashed and returned
HTTP 200 from `/api/status`.

Final idle STA snapshot: internal free 109,047 B, minimum 40,660 B, largest
block 63,488 B; PSRAM free 7,756,204 B. No allocation failures, fallback
events, or websocket errors were observed. Device-state storage remains
internal because it is accessed under a critical section; WebSocket ring and
BLE-sensitive buffers were not moved.

## Authenticated MCP follow-up

After production reflash, `POST /mcp` with `MCP-Protocol-Version: 2026-07-28`,
`Mcp-Method`, `Mcp-Name`, and the supplied bearer token passed `tools/list` and
`tools/call get_status`. The 0-link snapshot reported internal free 99,799 B,
minimum 44,824 B, largest block 51,200 B; PSRAM free 7,751,052 B and no
allocation failures.
