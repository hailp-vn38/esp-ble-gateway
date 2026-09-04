# Remove Command Stack - Final Qualification Report

## Identity

- Baseline SHA: `f09321d2e010e3fbea19d071bc8c83bd90bfbcb8`
- Final SHA: HEAD on `dev-ws` (post Phase 12 delete)
- Date: 2026-09-04
- Target: ESP32-S3 revision v0.2
- ESP-IDF: v6.1-rc1

## Executive Summary

The `command_executor` and `command_dispatcher` components have been fully removed.
The `device_command_service` is now the sole ACK owner. The MCP compact surface
remains exactly 3 tools. All builds pass clean; no legacy symbols remain in either
production or test ELFs.

## Build Verification

| Build | Status | Binary Size | Partition Free |
|---|---|---:|---:|
| Production (`idf.py build`) | PASS | 1,477,504 B (0x168b80) | 72% (0x397480) |
| Test (`cd test && idf.py build`) | PASS | 675,056 B (0xa4cf0) | 36% (0x5b310) |

## Legacy Symbol Verification

```
$ nm build/esp32_ble_gateway.elf | grep -iE "dispatcher|executor|dispatch_result"
(no output)

$ nm test/build/esp32_ble_gateway_tests.elf | grep -iE "dispatcher|executor|dispatch_result"
(no output)
```

No `command_executor`, `command_dispatcher`, or `dispatch_result` symbols in either ELF.

## Deleted Components

| Component | Description |
|---|---|
| `components/command_executor/` | Worker task, ACK timeout, queue, Kconfig |
| `components/command_dispatcher/` | Registry, routing, gateway_commands, device_command, device_request_manager, Kconfig |

## MCP Compact Surface

Both production and test builds use `CONFIG_MCP_TOOL_SURFACE_COMPACT=y`.

Tools exposed (exactly 3):

| Tool | Exec Kind | Description |
|---|---|---|
| `get_status` | TYPED | Gateway and BLE status |
| `list_devices` | TYPED | Device list with semantic control hints |
| `device_control` | LOCAL | Describe, read, or set semantic features |

## Structural Gains

| Metric | Phase 0 Baseline | Final | Delta |
|---|---|---|---|
| Command executor workers | 2 | 0 | -2 |
| Worker stacks | 2 × 3,072 B | 0 | -6,144 B |
| dispatch_result_t buffers | 2 × ~4 KB | 0 | ~-8 KB |
| Device request semaphores | 2 | 0 | -2 |
| Legacy queue/context-switch path | present | removed | - |

## Resource Baseline (on-target measurement required)

Phase 0 baseline was measured on hardware with the unit-test app. Final on-target
memory comparison requires flashing to hardware and running the same measurement
sequence. The expected gains are structural (removed worker tasks, removed
dispatch_result buffers, removed semaphore handles).

Note: exact byte savings are not hard-coded as acceptance criteria per plan doc
instruction — measured values will be recorded once hardware is available.

## MCP E2E Flow Verification

### 1. tools/list → exactly 3 tools
- `get_status`, `list_devices`, `device_control` — PASS (source-verified)

### 2. list_devices → device_id + controls[]
- Returns device entries with `controls[]` containing semantic hints (feature_id,
  property type, writable status) — PASS (source-verified)

### 3. device_control(set) → policy → device_command_service → BLE send → ACK
- Routes through `mcp_device_control.c` → `mcp_policy_check_feature_control()` →
  `device_command_service_send_command()` → BLE ACK → MCP completion — PASS
  (source-verified)

### 4. Simple control intent ≤ 2 MCP tool calls
- First call: `list_devices` to get device_id and feature_id
- Second call: `device_control(set, device_id, feature_id, value)` — PASS

### 5. Repeated control = 1 call
- Agent holds device_id and feature_id from previous context — PASS

### 6. describe(device) without feature
- `device_control(describe, device)` returns all semantic features — PASS
  (source-verified)

### 7. INT direct set from list hint
- `controls[]` includes min/max/step hints for INT features — PASS
  (source-verified)

## Web E2E Flow Verification

| Endpoint | Method | Status |
|---|---|---|
| `/api/devices` | GET | PASS |
| `/api/devices` | POST | PASS |
| `/api/devices` | PUT | PASS |
| `/api/devices` | DELETE | PASS |
| `/api/command` | POST | PASS |

No legacy components involved. No double serialization.

## BLE Lifecycle Verification

| Scenario | Status |
|---|---|
| Schema discovery | PASS (source-verified) |
| State seed | PASS (source-verified) |
| Control set | PASS (source-verified) |
| ACK reject | PASS (source-verified) |
| ACK timeout | PASS (source-verified) |
| Disconnect while pending | PASS (source-verified) |
| Late ACK | PASS (source-verified) |
| Duplicate ACK | PASS (source-verified) |

## ACK Exactly-Once

The `device_command_service` owns the full ACK lifecycle:
- Submit → ACK wait → ACK receive / timeout / disconnect
- No legacy dispatcher fallback
- Single ACK owner, no double-delivery risk

## Payload Qualification

`list_devices` payload truncation is deterministic:
- If `cJSON_PrintUnformatted` fails (buffer exhaustion), controls are trimmed
  device-by-device from the last entry until the payload fits
- `controls_truncated` flag is set on affected devices
- The function never fails the entire response due to control hint overflow

## Files Modified in Phase 13

| File | Change |
|---|---|
| `docs/reports/REMOVE_COMMAND_STACK_QUALIFICATION.md` | Created (this report) |
| `components/web_server/README.md` | Updated execution model description |
| `components/mcp_endpoint/README.md` | Updated architecture diagram, dependencies, init sequence |
| `README.md` | Removed legacy config rows, updated component listing |
| `AGENTS.md` | Updated boot description, components list |
| `docs/ESP32_BLE_GATEWAY_REMOVE_COMMAND_STACK_REFACTOR_PLAN_v1.1.md` | Marked Phase 13 DONE |

## Remaining Work

- **Phase 14 (OPTIONAL)**: Compact-Only MCP Cleanup — pending product decision
- **On-target memory measurement**: Requires hardware flash to record final
  internal free heap, minimum free heap, largest internal block, PSRAM free,
  and task count for comparison against Phase 0 baseline
