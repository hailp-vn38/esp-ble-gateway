# Gateway Phase G0 — Protocol Alignment & Settings Extension Gate


**Repo:** `hailp-vn38/esp-ble-gateway`  
**Baseline:** `dev-ws`  
**Target:** ESP32-S3  
**Feature:** Device Settings v2  
**Primary constraint:** bảo vệ internal SRAM; dữ liệu Settings lớn/long-lived phải dùng PSRAM-required.


## Goal

Khóa wire contract và xóa drift Protocol v4 giữa gateway `dev-ws` và device baseline trước khi thêm Settings.

## Files

```text
components/cbor_codec/include/cbor_codec.h
components/cbor_codec/cbor_codec.c
components/device_command_service/...             # only protocol-facing structs if needed
protocol tests / golden vector tests
```

## Tasks

### G0.1 Baseline diff

Đối chiếu gateway header với device authoritative contract:

- protocol version;
- message enums;
- numeric CBOR keys;
- feature/property enums;
- optional extension behavior.

Không tiếp tục thêm key vào một phía riêng lẻ.

### G0.2 Settings specialized decoder/view

Không add:

```c
char setting_id[32];
char setting_value[64];
char enum_label[...];
```

vào `gw_message_t`.

Tạo API riêng, ví dụ:

```c
typedef struct {
    const uint8_t *buf;
    size_t len;
} gw_settings_frame_view_t;

esp_err_t gw_settings_view_parse(...);
esp_err_t gw_settings_encode_request(...);
```

View chỉ sống trong lifetime raw BLE frame; data cần giữ lâu phải deep-copy vào PSRAM-owned object.

### G0.3 Capability gate

Gateway chỉ request Settings khi device advertises support/revision. Old device path:

```text
capability absent -> settings state UNSUPPORTED -> do not send describe_settings
```

### G0.4 Version decision

Compatibility matrix quyết định giữ Protocol v4 additive extension hay bump v5.

## Tests

- [ ] Golden vectors match device pack byte-for-byte.
- [ ] Unknown additive keys handled according to contract.
- [ ] malformed/truncated CBOR returns bounded error.
- [ ] max frame accepted/rejected correctly.
- [ ] old device capability decode unchanged.
- [ ] `sizeof(gw_message_t)` unchanged by Settings work.

Compatibility:

- [ ] new gateway ↔ old device: Settings unsupported, features/control pass.
- [ ] old gateway ↔ new device: verified by cross-repo HIL.
- [ ] new ↔ new: full Settings protocol.

## Memory checklist

- [ ] Settings decoder does not allocate per parsed scalar.
- [ ] No shared queue event grows due to Settings strings.
- [ ] Temporary decode uses bounded stack/internal footprint.
- [ ] Persistent copies are allocated via Settings PSRAM allocator layer later in G1.

## Exit gate

Protocol version decision, golden vectors and old-device regression all pass.
