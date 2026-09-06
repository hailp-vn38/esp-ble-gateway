# Device Settings v2 — Gateway Test Matrix & Release Checklist


**Repo:** `hailp-vn38/esp-ble-gateway`  
**Baseline:** `dev-ws`  
**Target:** ESP32-S3  
**Feature:** Device Settings v2  
**Primary constraint:** bảo vệ internal SRAM; dữ liệu Settings lớn/long-lived phải dùng PSRAM-required.


## Matrix

| Area | Case | Expected |
|---|---|---|
| Protocol | old device | Settings unsupported; feature/control unchanged |
| Codec | malformed response | reject, no crash |
| Discovery | PSRAM fail | old snapshot preserved, no internal fallback |
| Snapshot | concurrent GET + swap | no UAF |
| Transaction | stale revision | conflict |
| Transaction | second same-device save | busy |
| Transaction | SET error | no COMMIT |
| Commit | ACK lost after persist | OUTCOME_UNKNOWN -> reconcile success |
| Reconnect | old revision | operation failure |
| Reconnect | unrelated newer revision | conflict |
| API | max GET | bounded internal SRAM |
| API | secret | no plaintext |
| UI | generic types | render/edit correctly |
| UI | stale revision | blind save prevented |
| Memory | external required fail | graceful failure |
| Soak | 100 discovery | no leak |
| Soak | 100 save/reboot | all ops resolve |

## PR checklist

### Memory
- [ ] Any new persistent allocation classified INTERNAL vs EXTERNAL_REQUIRED.
- [ ] No large fixed array added to global structs.
- [ ] Queue/pending `sizeof` recorded.
- [ ] No 8KB stack/local JSON buffer.
- [ ] Snapshot pools allocated PSRAM-required.

### Ownership
- [ ] Every pointer has owner/lifetime documented.
- [ ] HTTP snapshot acquire/release all exits.
- [ ] Queue payload freed exactly once.
- [ ] Completed operation TTL/bound known.

### Protocol
- [ ] Golden vectors match device.
- [ ] No `gw_message_t` string growth.
- [ ] Unsupported capability gate.

### API/UI
- [ ] async 202 operation.
- [ ] revision conflict handled.
- [ ] secret redacted.
- [ ] generic UI only.

## Final release condition

All G0–G7 exit gates pass against the Device Settings v2 reference firmware, including memory metrics on the target ESP32-S3 build.
