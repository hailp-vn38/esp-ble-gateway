# Remove Command Stack - Phase 00 Baseline Report

## Identity

- Baseline SHA: `f09321d2e010e3fbea19d071bc8c83bd90bfbcb8`
- Characterized working branch: `dev-ws`
- Characterized HEAD before Phase 0 test changes: `83d4ee43c766efe80fbfa5f685272fcb4b85aa41`
- Date: 2026-09-04
- Target: ESP32-S3 revision v0.2
- Test port: `/dev/cu.usbmodem101`
- ESP-IDF environment: `/Users/lamphuchai/.espressif/v6.1-rc1/esp-idf`

The reviewed baseline SHA is an ancestor of the characterized HEAD. Phase 0 only
changes tests, test records, and documentation; production sources are unchanged.

## Behavioral baseline

- Compact `tools/list` contains exactly three tools, in order: `get_status`,
  `list_devices`, `device_control`.
- `get_status` and `list_devices` retain their current MCP tool response paths.
- `device_control` routes `describe`, `read`, and `set` through the compact
  semantic tool. The Phase 0 routing test uses a missing-device fixture and
  freezes the current tool-error contract without requiring a live peripheral.
- Web tests exercise all `/api/devices` methods over a real loopback HTTP server.
  GET returns the snapshot contract; malformed POST, PUT, and DELETE requests
  retain HTTP 400 plus `invalid_request`. `/api/command` retains the same typed
  request validation contract.
- Device command service tests freeze ACK success, device rejection, timeout,
  and disconnect completion behavior after schema validation.

## Delete ordering and degraded behavior

The current `delete_device` dispatcher order is:

1. revoke MCP exposure;
2. forget schema;
3. clear runtime feature state;
4. forget the BLE peer;
5. delete the persistent device record.

Current failure semantics are intentionally frozen for later migration:

- exposure revoke failure logs a warning and deletion continues;
- schema forget failure returns internal error and stops before state/BLE/store;
- runtime state forget has no returned failure;
- BLE peer forget failure logs `DEVICE_DELETE_DEGRADED` and continues;
- store delete failure logs `DEVICE_DELETE_DEGRADED` and the command still
  returns success.

## Resource baseline

Values below come from the flashed unit-test application. They are comparison
points for later phases, not production-firmware boot metrics.

| Metric | Baseline |
|---|---:|
| Free internal heap | 285,104 B |
| Minimum free internal heap | 284,256 B |
| Largest internal block | 241,664 B |
| PSRAM free | 0 B |
| Task count | 11 |
| Command executor workers | 2 |
| Minimum executor worker stack watermark | 2,948 B |
| `list_devices` JSON-RPC response | 91 B |

PSRAM is physically present (8 MiB reported by esptool), but
`CONFIG_SPIRAM` is disabled in `test/sdkconfig`; therefore the test-app PSRAM
baseline is correctly 0 B. The payload measurement uses an empty device-store
fixture and includes the complete JSON-RPC response.

## Verification

```text
cd test
idf.py fullclean
idf.py build
idf.py -p /dev/cu.usbmodem101 flash
```

- Clean test build: PASS
- Image size: `0xa2100` bytes; app partition free: `0x5df00` bytes (37%)
- Hardware Unity run: `353 Tests 0 Failures 0 Ignored`
- Compact tool count: exactly 3
- Production behavior changes: none
