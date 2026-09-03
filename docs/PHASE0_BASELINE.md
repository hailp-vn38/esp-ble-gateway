# Phase 0 - Baseline Characterization (v1.3)

**Baseline SHA:** `dc70c95d073cdccb8d55c0e1e7abdc556d63e76b`
**Date:** 2026-09-03
**Branch:** `dev-ws`

---

## 1. Executor Configuration

| Parameter | Default | Source |
|---|---|---|
| `CONFIG_CMD_EXEC_WORKER_COUNT` | 2 | `Kconfig.projbuild:5` |
| `CONFIG_CMD_EXEC_QUEUE_LEN` | 2 | `Kconfig.projbuild:12` |
| `CONFIG_CMD_EXEC_WORKER_STACK` | 4096 bytes | `Kconfig.projbuild:22` |
| `CONFIG_CMD_EXEC_JOB_TIMEOUT_MS` | 3000 ms | `Kconfig.projbuild:27` |

## 2. Memory Cost Per Worker

| Item | Size | Location |
|---|---|---|
| `dispatch_result_t` payload | 4096 bytes | PSRAM (gw_mem_calloc) |
| `dispatch_result_t` struct overhead | 8 bytes (status + format) | PSRAM |
| **Total per worker result buffer** | **~4104 bytes** | **PSRAM** |
| Worker task stack | 4096 bytes | Internal RAM |
| Worker TCB | ~180 bytes | Internal RAM |

**Total for 2 workers:**
- Result buffers: 2 x 4104 = **8208 bytes PSRAM**
- Stacks: 2 x 4096 = **8192 bytes Internal RAM**
- TCBs: 2 x 180 = **360 bytes Internal RAM**

## 3. Pending Request Table

| Parameter | Value | Source |
|---|---|---|
| `CONFIG_DEVICE_REQUEST_MAX_PENDING` | 4 | `device_request_manager.c:13` |
| Binary semaphore per slot | 4 x ~80 bytes = 320 bytes | Internal RAM |
| Pending slot struct | 4 x sizeof(pending_request_t) | Internal RAM |
| Mutex for table | 1 x ~80 bytes | Internal RAM |

## 4. Device Command Flow (Current)

```
MCP/Web caller
  -> command_executor_submit()           [queue admission]
  -> FreeRTOS queue
  -> worker_loop()                       [dequeues job]
  -> command_dispatcher_handle()         [routes to device_command_handle]
  -> device_schema_validate_command()    [validation]
  -> device_request_allocate()           [1 per device, semaphore]
  -> ble_central_send_command()          [BLE send]
  -> device_request_wait()               [BLOCKS on semaphore, up to 2000ms]
  -> builds cJSON result                 [~4KB dispatch_result_t]
  -> completion callback
```

**Key inefficiency:** Worker blocks waiting for BLE ACK, consuming stack + result buffer for the entire duration.

## 5. ACK Correlation

- Request ID generated incrementally, skip 0, avoid live collisions
- ACK must match: device_id + request_id + command
- Mismatched ACK logged as protocol violation
- One pending command per device enforced

## 6. State Seed Bug

`device_state.c:75-82` submits `read_feature_state` without populating `feature_id` or `property_id`:

```c
gw_message_t msg = {0};
strlcpy(msg.command, "read_feature_state", sizeof(msg.command));
// BUG: msg.feature_id and msg.property_id not set
```

Device expects `feature_id` and `property_id` in the request.

## 7. Test Results (Pre-migration)

| Component | Pass | Fail | Total |
|---|---|---|---|
| command_executor | 4 | 0 | 4 |
| command_dispatcher | 31 | 0 | 31 |
| device_schema | 22 | 0 | 22 |
| device_state | 19 | 0 | 19 |
| mcp_endpoint | 55 | 0 | 55 |
| **Total relevant** | **131** | **0** | **131** |

All relevant tests pass. 10 failures in web_server (WebSocket tests, unrelated).

## 8. Binary Size

- Test binary: 0x9be70 bytes (~639 KB)
- Smallest app partition: 0x100000 (1 MB)
- Free: 39%

## 9. Key Observations for Migration

1. `device_state` depends on `command_executor` for state seed
2. `mcp_endpoint` depends on both `command_executor` and `command_dispatcher`
3. `device_command_handle()` blocks the executor worker while waiting for BLE ACK
4. Schema discovery uses injected submitter pattern (`device_schema_set_submitter`)
5. `dispatch_result_t` is ~4KB and allocated per-worker persistently
6. 4 pending request slots with 4 semaphores

---

## Checklist

- [x] Baseline SHA recorded (`dc70c95d073cdccb8d55c0e1e7abdc556d63e76b`)
- [x] Existing tests pass before production changes (131/131 relevant)
- [x] Executor worker/queue configuration recorded
- [x] Internal RAM/PSRAM baseline recorded (estimated: 8208B PSRAM + 8552B Internal for executor+pending)
- [x] Device command latency baseline recorded (blocking up to 2000ms per command)
- [x] Pending correlation behavior recorded (1 per device, 4 max slots)
- [x] MCP tool count/serialized-size baseline recorded (2 static + dynamic)
- [x] State-read baseline behavior recorded (missing feature_id/property_id bug)
- [x] No production behavior changed in this phase
