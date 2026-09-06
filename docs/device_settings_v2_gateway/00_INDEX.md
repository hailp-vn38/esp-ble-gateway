# ESP32 BLE Device Settings v2 — Gateway Implementation Pack


**Repo:** `hailp-vn38/esp-ble-gateway`  
**Baseline:** `dev-ws`  
**Target:** ESP32-S3  
**Feature:** Device Settings v2  
**Primary constraint:** bảo vệ internal SRAM; dữ liệu Settings lớn/long-lived phải dùng PSRAM-required.


## 1. Phạm vi

Bộ này **chỉ chứa công việc phía Gateway**: protocol/view codec, BLE discovery, PSRAM snapshots, transaction orchestration, reboot reconciliation, REST API, WebSocket, Device Detail UI và memory hardening.

Device implementation details không nằm trong bộ này; gateway chỉ dựa vào wire contract đã khóa.

## 2. Kiến trúc đã khóa

1. Device là source-of-truth.
2. Settings tách khỏi Features/MCP runtime control.
3. Schema và values là **hai immutable snapshots**.
4. Snapshot/pools lớn dùng `GW_MEM_EXTERNAL_REQUIRED`.
5. Không silent fallback large/long-lived Settings data vào internal SRAM.
6. Shared `gw_message_t` không được phình vì Settings strings/enum arrays.
7. Settings dùng specialized CBOR view/codec trên raw frame.
8. Queue/pending chỉ giữ compact metadata/pointer.
9. One active Settings operation per device.
10. Update serialized: `BEGIN -> SET sequential -> COMMIT -> CONFIRM`.
11. ACK-loss -> `OUTCOME_UNKNOWN`, reconcile bằng config_revision + values.
12. REST update async `202 + operation_id`.
13. WebSocket chỉ gửi state/revision, không gửi full settings/secrets.
14. GET nên streaming JSON hoặc allocator PSRAM; không big stack buffer.

## 3. Phase order

| Phase | File | Exit gate |
|---|---|---|
| G0 | `GATEWAY_PHASE_0_PROTOCOL_ALIGNMENT.md` | ✅ DONE (2026-09-06) |
| G1 | `GATEWAY_PHASE_1_SETTINGS_CORE_MEMORY_MODEL.md` | ✅ DONE (2026-09-06) |
| G2 | `GATEWAY_PHASE_2_BLE_DISCOVERY_SNAPSHOTS.md` | repeated discovery stable |
| G3 | `GATEWAY_PHASE_3_TRANSACTION_SERVICE.md` | atomic update orchestration pass |
| G4 | `GATEWAY_PHASE_4_REBOOT_RECONCILIATION.md` | ACK-loss/reconnect pass |
| G5 | `GATEWAY_PHASE_5_WEB_API.md` | REST/WS deterministic + secret-safe |
| G6 | `GATEWAY_PHASE_6_DEVICE_DETAIL_UI.md` | generic UI E2E pass |
| G7 | `GATEWAY_PHASE_7_MEMORY_SOAK_HARDENING.md` | SRAM/PSRAM + soak release gates pass |

Release checklist: `GATEWAY_TEST_MATRIX_AND_RELEASE_CHECKLIST.md`.

## 4. Hard memory policy

```text
INTERNAL SRAM
  compact state, pointer, index, locks, queue items, pending metadata

PSRAM REQUIRED
  schema snapshot, values snapshot, string pool, enum pool,
  transaction variable data, request copied strings,
  large HTTP/JSON workspace
```

If `EXTERNAL_REQUIRED` fails:

```text
Settings operation fails gracefully.
BLE/Wi-Fi/Features continue.
No fallback to internal SRAM for that large/long-lived object.
```

Targets:

| Item | Target |
|---|---:|
| Persistent Settings control metadata | `<= 64–128 B/device` |
| Queue item | `<= 32 B` |
| Pending metadata | `<= 64 B` |
| Discovery transient internal delta | target `<= 1 KiB` |
| Large/long-lived internal fallback | `0 B` |
| `gw_message_t` growth due Settings strings | `0 B` |

## 5. Gateway states

```text
UNKNOWN
UNSUPPORTED
DISCOVERING
READY
UPDATING
REBOOTING
OUTCOME_UNKNOWN
ERROR
```

Operation:

```text
QUEUED -> RUNNING -> WAITING_REBOOT -> VERIFYING -> SUCCEEDED
                     \-> OUTCOME_UNKNOWN -> VERIFYING -> SUCCEEDED/FAILED/CONFLICT
```

## 6. Definition of Done — Gateway

- [ ] Old devices remain fully functional with Settings unsupported.
- [ ] Snapshot allocations use PSRAM-required.
- [ ] Snapshot lifetime safe under concurrent HTTP/discovery.
- [ ] Multi-field save atomic at device protocol level.
- [ ] Stale revision surfaces as 409/conflict.
- [ ] ACK-loss reconciles correctly.
- [ ] REST/UI never expose secret plaintext.
- [ ] UI generated from schema, no product-specific setting ids.
- [ ] 100 discovery + 100 save/reboot cycles pass.
- [ ] Internal SRAM gates pass under Wi-Fi + BLE + WS load.
