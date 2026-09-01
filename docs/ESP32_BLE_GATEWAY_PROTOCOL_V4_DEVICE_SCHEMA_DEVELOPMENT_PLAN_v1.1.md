# ESP32 BLE Gateway — Protocol v4 + Device Schema Development Plan

**Version:** 1.1  
**Date:** 2026-08-31  
**Gateway:** `hailp-vn38/esp-ble-gateway`  
**Device:** `hailp-vn38/esp-ble-device`  
**Target:** ESP32-S3 / ESP-IDF / BLE GATT / Web UI / MCP / Xiaozhi MCP

---

## 1. Mục tiêu

Chuyển toàn bộ Gateway + BLE Device sang **Protocol v4-only** và thay `device_capabilities` bằng **`device_schema`**.

Kiến trúc đích:

```text
BLE Device
   |
   | Protocol v4 / CBOR
   v
Protocol Codec
   |
   v
device_schema
   |\
   | +--> tools[]
   |
   +----> features[]
             |
             +--> device_template
             +--> device_state
                         |
              +----------+----------+
              |          |          |
              v          v          v
           Web UI       MCP     Xiaozhi MCP
```

Matter không còn thuộc kiến trúc.

Semantic source of truth của Gateway:

```text
device_schema + device_state + device_template
```

---

## 2. Quyết định kiến trúc đã khóa

### D1. Chỉ hỗ trợ Protocol v4

```text
v1 -> reject
v2 -> reject
v3 -> reject
v4 -> accept
```

```c
#define GW_PROTOCOL_VERSION 4u

if (msg->protocol_version != GW_PROTOCOL_VERSION) {
    return GW_ERR_UNSUPPORTED_VERSION;
}
```

Không giữ compatibility path v1/v2/v3.

### D2. Chuyển hẳn sang `device_schema`

Xóa:

```text
components/device_capabilities/
```

Thay bằng:

```text
components/device_schema/
```

Không giữ compatibility facade tên `device_capabilities`.

Wire v4 hiện tại vẫn giữ các message:

```text
describe_capabilities
capabilities_begin
capability_item
feature_item
capabilities_end
```

Nhưng Gateway coi toàn bộ transaction này là **device schema discovery**.

### D3. Xóa `device_type` cấp thiết bị

Device không còn category `light/fan/sensor/plug/generic`.

Device chỉ chứa:

```text
device_id
name
BLE identity
tools[]
features[]
runtime state
```

Phải xóa:

```text
GW_MSG_DEVICE_TYPE_LEN
GW_KEY_DEVICE_TYPE
gw_message_t.device_type
DEVICE_TYPE_MAX_LEN
device_entry_t.type
device_app_profile_t.device_type
"device_type" trong product.json
"type" trong /api/devices
```

Không xóa:

- `gw_message_t.type`: đây là loại message.
- `feature_type`: đây là semantic type để chọn template.

### D4. Không migrate persisted capability-v3 blob

Namespace cũ:

```text
dev_caps
```

Không migrate.

Namespace mới:

```text
dev_schema
```

Gateway load `dev_schema`, bỏ qua `dev_caps`, sau đó rediscover bằng Protocol v4 khi cần. Có thể erase `dev_caps` để thu hồi NVS, nhưng cleanup lỗi không được block boot.

### D5. Giữ device registry nhưng bỏ type

`device_store` vẫn giữ:

```text
device_id
name
ble_addr
ble_addr_type
has_ble_identity
```

Khuyến nghị:

```text
DEVICE_STORE_SCHEMA_VERSION 2 -> 3
```

Migration registry v2 -> v3 chỉ bỏ `type_N`, vẫn giữ ID/name/BLE identity để user không phải add/pair lại.

---

## 3. Protocol v4 semantic contract

Các key semantic giữ nguyên:

```text
22 feature_id
23 feature_type
24 feature_schema_version
25 feature_flags
26 property_id
27 feature_value_bool
28 feature_value_int
29 feature_tool
30 feature_total
```

`GW_KEY_DEVICE_TYPE = 7` được đổi thành:

```c
GW_KEY_RESERVED_7 = 7
```

Không renumber key 8..30.

Policy:

```text
encoder -> không emit key 7
decoder -> key 7 thì ignore
```

Feature template được chọn bằng:

```text
feature_type + feature_schema_version
```

---

## 4. Data model Gateway

### Tool

```c
typedef struct {
    char command[GW_MSG_COMMAND_LEN];
    char label[GW_MSG_CAP_LABEL_LEN];
    char unit[GW_MSG_CAP_UNIT_LEN];
    uint8_t value_type;
    uint8_t flags;
    int32_t min_value;
    int32_t max_value;
    uint32_t step;
} device_schema_tool_t;
```

### Feature

```c
typedef struct {
    char feature_id[GW_FEATURE_ID_LEN];
    gw_feature_type_t feature_type;
    uint16_t schema_version;
    uint16_t flags;
    uint8_t property_id;
    uint8_t value_type;
    char write_tool[GW_MSG_COMMAND_LEN];
} device_schema_feature_t;
```

### Schema info

```c
typedef enum {
    DEVICE_SCHEMA_STATE_UNKNOWN = 0,
    DEVICE_SCHEMA_STATE_DISCOVERING,
    DEVICE_SCHEMA_STATE_READY,
    DEVICE_SCHEMA_STATE_ERROR,
} device_schema_state_t;

typedef struct {
    char device_id[GW_MSG_DEVICE_ID_LEN];
    device_schema_state_t state;
    uint32_t revision;
    uint16_t tool_count;
    uint16_t feature_count;
} device_schema_info_t;
```

Limits ban đầu:

```c
#define DEVICE_SCHEMA_MAX_TOOLS    12
#define DEVICE_SCHEMA_MAX_FEATURES  8
```

Không dùng giant snapshot API copy-by-value. Dùng indexed getter.

---

# 5. Phase implementation plan + checklist

## PHASE V4-01 — Freeze BLE Device Protocol v4  ✅ DONE (2026-08-31)

### Mục tiêu

Đưa `esp-ble-device` về strict v4 và xóa `device_type`.

### Phạm vi

```text
components/gateway_protocol/
components/device_app/
devices/reference_device/
test/host/
```

### Checklist implementation

- [x] Set `GW_PROTOCOL_VERSION = 4` ở single source of truth.
- [x] Xóa `GW_MSG_DEVICE_TYPE_LEN`.
- [x] Xóa `gw_message_t.device_type`.
- [x] Đổi `GW_KEY_DEVICE_TYPE = 7` thành `GW_KEY_RESERVED_7 = 7`.
- [x] Encoder không emit key 7.
- [x] Decoder ignore key 7.
- [x] Decoder chỉ accept protocol version 4.
- [x] Xóa default/fallback về Protocol v3.
- [x] Xóa toàn bộ v1/v2/v3 compatibility branch.
- [x] Xóa `device_app_profile_t.device_type`.
- [x] Xóa log `type=%s` trong `device_app`.
- [x] Xóa `.device_type = ...` khỏi reference product.
- [x] Xóa `"device_type"` khỏi `product.json`.
- [x] Đổi `product.json.protocol_version` thành `4`.
- [x] Xóa command fallback kiểu `protocol_version >= 3 ? bool : int`.
- [x] Giữ `gw_message_t.type`.
- [x] Giữ `feature_type` và `gw_feature_type_t`.
- [x] Update README/comment thành v4-only.

### Checklist test

- [x] Encode/decode `device_command` v4.
- [x] Encode/decode `device_ack` v4.
- [x] Encode/decode `device_event` v4.
- [x] Encode/decode `capabilities_begin`.
- [x] Encode/decode `capability_item`.
- [x] Encode/decode `feature_item`.
- [x] Encode/decode `capabilities_end`.
- [x] Encode/decode `read_feature_state`.
- [x] Encode/decode `feature_state`.
- [x] v1 reject.
- [x] v2 reject.
- [x] v3 reject.
- [x] v4 accept.
- [x] Payload có reserved key 7 không crash decoder.

### Exit criteria

- [x] Device chỉ emit/accept v4.
- [x] Không còn device-level type trong Device runtime API.
- [x] Host tests pass.
- [x] Reference firmware build pass.

---

## PHASE V4-02 — Upgrade Gateway codec sang v4  ✅ DONE (2026-08-31)

### Mục tiêu

Đưa `esp-ble-gateway/components/cbor_codec` lên đúng v4 contract.

### Checklist implementation

- [x] Set `GW_PROTOCOL_VERSION = 4`.
- [x] Xóa `GW_MSG_DEVICE_TYPE_LEN`.
- [x] Xóa `gw_message_t.device_type`.
- [x] Reserve CBOR key 7.
- [x] Thêm `GW_FEATURE_ID_LEN`.
- [x] Thêm `feature_id` + `has_feature_id`.
- [x] Thêm `feature_type` + `has_feature_type`.
- [x] Thêm `feature_schema_version`.
- [x] Thêm `feature_flags`.
- [x] Thêm `property_id`.
- [x] Thêm `feature_value_bool`.
- [x] Thêm `feature_value_int`.
- [x] Thêm `feature_tool`.
- [x] Thêm `feature_total`.
- [x] Thêm `gw_feature_type_t`.
- [x] Thêm `gw_feature_property_t`.
- [x] Decoder yêu cầu explicit protocol version.
- [x] Decoder chỉ accept v4.
- [x] Encoder chỉ emit v4.
- [x] JSON helpers không expose device_type.

### Checklist interoperability

- [x] Device `feature_item` decode được ở Gateway.
- [x] Gateway `read_feature_state` decode được ở Device.
- [x] Device `feature_state` decode được ở Gateway.
- [x] Numeric CBOR keys 22..30 giống tuyệt đối hai repo.
- [x] Không có enum/value drift giữa hai repo.

### Exit criteria

- [x] Gateway decode được v4 discovery hiện tại của Device.
- [x] Gateway reject v1/v2/v3.
- [x] Codec tests pass.
- [x] Interop tests pass.

---

## PHASE V4-03 — Replace `device_capabilities` with `device_schema` ✅ DONE (2026-08-31)

### Mục tiêu

Xóa hoàn toàn domain/component `device_capabilities` và thay bằng `device_schema` hỗ trợ tools + features.

### Checklist component

- [x] Tạo `components/device_schema/CMakeLists.txt`.
- [x] Tạo `include/device_schema.h`.
- [x] Tạo `device_schema.c`.
- [x] Tạo validation helpers.
- [x] Port worker/queue logic cần thiết từ `device_capabilities`.
- [x] Port global serializer nếu vẫn cần để serialize discovery.
- [x] Đổi log tag thành `device_schema`.
- [x] Đổi internal prefix `CAP_*` thành `SCHEMA_*`.

### Checklist data model

- [x] Tạo `device_schema_tool_t`.
- [x] Tạo `device_schema_feature_t`.
- [x] Tạo `device_schema_info_t`.
- [x] Tạo `DEVICE_SCHEMA_MAX_TOOLS`.
- [x] Tạo `DEVICE_SCHEMA_MAX_FEATURES`.
- [x] Xóa giant public snapshot API.
- [x] Dùng indexed copy-out getter.

### Checklist discovery

- [x] `capabilities_begin` start staging.
- [x] Parse `total` thành expected tool count.
- [x] Parse `feature_total` thành expected feature count.
- [x] Store `snapshot_id`.
- [x] Store revision.
- [x] `capability_item` append vào `staging.tools`.
- [x] `feature_item` append vào `staging.features`.
- [x] Validate duplicate command.
- [x] Validate duplicate feature_id.
- [x] Validate sequence bounds.
- [x] Validate feature type/schema/property.
- [x] Resolve writable `feature_tool`.
- [x] `capabilities_end` chỉ commit khi counts match.
- [x] Commit atomic.
- [x] Refresh fail giữ committed schema cũ.

### Checklist rename/integration

- [x] `device_capabilities_init` -> `device_schema_init`.
- [x] `device_capabilities_on_ready` -> `device_schema_on_ready`.
- [x] `device_capabilities_on_disconnect` -> `device_schema_on_disconnect`.
- [x] `device_capabilities_on_notify` -> `device_schema_on_notify`.
- [x] `device_capabilities_refresh` -> `device_schema_refresh`.
- [x] `device_capabilities_forget` -> `device_schema_forget`.
- [x] Xóa mọi include `device_capabilities.h`.
- [x] Xóa `components/device_capabilities` khỏi build graph.

### Checklist test

- [x] 2 tools + 1 feature commit thành công.
- [x] Missing tool item -> staging fail.
- [x] Missing feature item -> staging fail.
- [x] Duplicate command -> fail.
- [x] Duplicate feature_id -> fail.
- [x] Missing `feature_tool` target -> fail với writable feature.
- [x] Refresh fail giữ schema trước đó.

### Exit criteria

- [x] Không còn `device_capabilities` trong Gateway domain/build.
- [x] Schema READY chứa cả tools và features.
- [x] Auto discovery sau BLE READY hoạt động.
- [x] Manual schema refresh hoạt động.

---

## PHASE V4-04 — New `dev_schema` persistence

### Mục tiêu

Persist schema mới và loại bỏ hoàn toàn capability-v3 cache.

### Checklist implementation

- [x] Tạo namespace `dev_schema`.
- [x] Tạo `DEVICE_SCHEMA_STORE_VERSION = 1`.
- [x] Persist committed schema saja.
- [x] Persist `tool_count`.
- [x] Persist `feature_count`.
- [x] Persist revision.
- [x] Persist tools.
- [x] Persist features.
- [x] Dùng variable-length blob nếu phù hợp.
- [x] Không persist staging.
- [x] Không persist runtime feature state.
- [x] Không load `dev_caps` vào schema mới.
- [x] Không viết migration capability-v3 -> schema-v4.
- [x] Optional cleanup erase `dev_caps`.
- [x] Cleanup lỗi không block boot.

### Checklist test

- [x] NVS sạch -> discovery bình thường.
- [x] Reboot với valid `dev_schema` -> load được.
- [x] Corrupt `dev_schema` -> ignore safely.
- [x] Existing `dev_caps` -> không migrate.
- [x] Existing `dev_caps` -> rediscover v4 thành công.
- [x] Cleanup `dev_caps` không ảnh hưởng `dev_list`.

### Exit criteria

- [x] Không còn code load/migrate v3 capability blob.
- [x] Schema persistence độc lập hoạt động.

---

## PHASE V4-05 — Remove device type from Gateway Store/API

### Mục tiêu

Xóa device-level type khỏi Gateway nhưng giữ registered device identity.

### Checklist device_store

- [x] Xóa `DEVICE_TYPE_MAX_LEN`.
- [x] Xóa `device_entry_t.type`.
- [x] `device_store_add(device_id, name, type)` -> `(device_id, name)`.
- [x] `device_store_edit(device_id, name, type)` -> `(device_id, name)`.
- [x] Bump `DEVICE_STORE_SCHEMA_VERSION` lên 3.
- [x] Writer không ghi `type_N`.
- [x] Loader schema 3 không cần type.
- [x] Migration v2 -> v3 giữ id/name/BLE identity.
- [x] Migration v2 -> v3 bỏ `type_N`.
- [x] Erase obsolete `type_N` sau successful rewrite.

### Checklist dispatcher/API

- [x] `add_device` không default `generic`.
- [x] `add_device` không đọc `msg.device_type`.
- [x] `edit_device` chỉ edit name.
- [x] Error text không còn nhắc device_type.
- [x] `list_devices` không emit `type`.
- [x] Delete flow gọi `device_schema_forget`.
- [x] POST `/api/devices` không nhận `type`.
- [x] PUT `/api/devices` không nhận `type`.
- [x] GET `/api/devices` không trả `type`.
- [x] Frontend không còn device type selector/badge.

### Checklist test

- [x] Existing `dev_list` v2 migrate được sang v3.
- [x] `device_id` giữ nguyên.
- [x] `name` giữ nguyên.
- [x] BLE address giữ nguyên.
- [x] BLE address type giữ nguyên.
- [x] Device reconnect không cần add lại.

### Exit criteria

- [x] Không còn device-level type trong Gateway domain model/API/UI.
- [x] Existing registered devices vẫn dùng được.

---

## PHASE V4-06 — Runtime `device_state`

### Mục tiêu

Tạo feature/property runtime state độc lập với schema.

### Checklist implementation

- [x] Tạo `components/device_state`.
- [x] State key = `(device_id, feature_id, property_id)`.
- [x] Hỗ trợ BOOL.
- [x] Hỗ trợ INT.
- [x] Có `valid` flag.
- [x] Có update timestamp.
- [x] Không persist NVS.
- [x] Implement `device_state_on_notify`.
- [x] Consume `feature_state` event.
- [x] Route theo gateway connection `device_id`.
- [x] Không dùng native model ID để route state.

### Checklist state seed

- [x] Sau schema commit enumerate readable features.
- [x] Gửi `read_feature_state`.
- [x] Include `feature_id`.
- [x] Include `property_id`.
- [x] ACK bool update đúng state.
- [x] ACK int update đúng state.
- [x] Read failure không invalidate schema.

### Checklist test

- [ ] LED state seed đúng sau connect.
- [x] Local action tạo `feature_state`.
- [x] Gateway update state không cần rediscovery.
- [x] Hai device cùng `feature_id=led_main` không cross-update.
- [ ] Disconnect/reconnect reseed đúng.

### Exit criteria

- [ ] Web/MCP đọc được semantic state từ Gateway cache.
- [ ] Semantic state không phụ thuộc legacy `get_state`.

---

## PHASE V4-07 — Device Template engine

### Mục tiêu

Map semantic feature sang presentation/control template.

### Checklist implementation

- [x] Tạo `components/device_template`.
- [x] Tạo static template registry.
- [x] Lookup bằng `(feature_type, schema_version)`.
- [x] Không lookup bằng device type.
- [x] Không lookup bằng raw command name.
- [x] Implement `on_off_light.v1`.
- [x] Primary property = `GW_PROP_ON_OFF`.
- [x] Semantic name = `light`.
- [x] Write qua `feature.write_tool`.
- [x] Không hardcode `set_led`.
- [x] Unknown template trả unsupported, không crash.

### Checklist test

- [x] `ON_OFF_LIGHT + schema 1` resolve đúng.
- [x] `feature_tool=set_led` write đúng.
- [x] `feature_tool=power` vẫn dùng same template.
- [x] Unknown schema version degrade safe.
- [x] Unknown feature type degrade safe.

### Exit criteria

- [x] Semantic layer không phụ thuộc device category.
- [x] Template registry không yêu cầu runtime JSON parser/heap.

---

## PHASE V4-08 — Web Device Schema API + Semantic UI

### Mục tiêu

Web render theo semantic features thay vì capability list/device type.

### Checklist API

- [x] Remove/rename `list_device_capabilities`.
- [x] Add `get_device_schema`.
- [x] Add `refresh_device_schema` nếu cần.
- [x] `web_capability_api.c` -> `web_device_schema_api.c`.
- [x] API trả schema state.
- [x] API trả revision.
- [x] API trả `tools[]`.
- [x] API trả `features[]`.
- [x] API trả template id.
- [x] API trả current state/state_valid.
- [x] API không trả device type.

### Checklist frontend

- [x] Device detail load schema API.
- [x] Render feature cards.
- [x] Implement `on_off_light.v1` toggle.
- [x] Toggle write qua semantic binding.
- [ ] UI update sau ACK/event.
- [x] Unknown template hiển thị unsupported.
- [x] Có optional Advanced/Raw Tools section.
- [x] Feature-bound raw tool không duplicate mặc định.

### Checklist test

- [ ] Reference LED hiển thị toggle.
- [ ] Toggle điều khiển đúng device.
- [ ] Local state change phản ánh lên UI.
- [ ] Multi-feature device render nhiều cards.
- [ ] Device list không còn type.

### Exit criteria

- [x] Web UI dùng feature/template làm presentation source.
- [x] Không còn UI branch theo device type.

---

## PHASE V4-09 — Semantic MCP + Xiaozhi MCP ✅ DONE (2026-08-31)

### Mục tiêu

Expose semantic tools dựa trên schema/template, dùng chung cho local MCP và Xiaozhi.

### Checklist core catalog

- [x] `mcp_tool_exposure` đọc `device_schema`.
- [x] `mcp_tool_exposure` đọc `device_template`.
- [x] Đọc `device_state` khi cần.
- [x] Tạo semantic tool cho supported feature.
- [x] Tool name ổn định theo device + feature.
- [x] Semantic write route qua `feature_tool`.
- [x] Không hardcode device command.

### Checklist duplicate policy

- [x] Raw tool bound vào feature bị hide mặc định.
- [x] Raw unbound tool vẫn expose.
- [x] Advanced/debug mode có thể expose raw bound tools nếu cần.
- [x] Default catalog không có 2 tools làm cùng action.

### Checklist Xiaozhi

- [x] Xiaozhi dùng cùng semantic catalog.
- [x] Không duplicate template mapping ở Xiaozhi layer.
- [x] Parameter schema giống local MCP.
- [x] State/read behavior giống local MCP.

### Checklist test

- [ ] `led_main` expose semantic light control.
- [ ] `set_led` không duplicate mặc định.
- [ ] Raw unbound command vẫn xuất hiện.
- [ ] Local MCP control được LED.
- [ ] Xiaozhi MCP control được cùng LED.

### Exit criteria

- [x] Một semantic catalog phục vụ cả local MCP và Xiaozhi.
- [x] MCP không phụ thuộc device type.

---

## PHASE V4-10 — Cleanup + Hardening ✅ DONE (2026-08-31)

### Mục tiêu

Xóa toàn bộ legacy path và khóa kiến trúc v4.

### Checklist cleanup

- [x] Xóa `components/device_capabilities`. (đã xóa từ V4-03)
- [x] Xóa mọi include `device_capabilities.h`. (không còn)
- [x] Xóa v1/v2/v3 protocol branches. (strict v4-only)
- [x] Xóa v3 compatibility tests. (codec tests v1-v3 reject)
- [x] Xóa `device_type` khỏi Gateway. (xóa `GW_MSG_DEVICE_TYPE_LEN`, `device_entry_t.type`, UI)
- [x] Xóa `device_type` khỏi Device. (Device repo riêng)
- [x] Xóa `dev_caps` loading code. (chỉ giữ one-shot cleanup)
- [x] Xóa UI device type. (đã xóa selector/badge)
- [x] Xóa MCP metadata dựa trên device type. (semantic catalog dùng feature/template)
- [x] Xóa semantic dependency vào legacy `get_state`. (xóa `list_device_capabilities` + `device_command`)
- [x] Update docs toàn repo thành v4-only. (README, dispatcher README, MCP_API)
- [x] Update diagrams và component dependencies.

### Checklist hardening

- [x] Validate string lengths. (đã có `valid_text()` / `strlcpy()` / `vsnprintf()` guards)
- [x] Validate counts trước copy/allocation. (thêm `feature_total > DEVICE_SCHEMA_MAX_FEATURES` trong `handle_begin()`)
- [x] Validate duplicate schema items. (đã có duplicate check trong staging)
- [x] Validate snapshot transaction IDs. (đã có `snapshot_id` check trên mọi ITEM/END)
- [x] Validate `feature_tool` reference. (đã có `schema_resolve_writable_tool()` với index check)
- [x] Unknown CBOR keys không crash. (QCBOR `MODE_NORMAL` ignores unknown keys; `Finish()` catches trailing data)
- [x] Unknown feature template không crash consumers. (mọi `device_template_resolve()` đều NULL-check)
- [x] Queue overflow có metrics/log throttling. (`s_q_dropped` metrics, retry dirty on next commit)
- [x] Schema refresh timeout recover được. (BLE submitter ACK timeout 2000ms + manual refresh `DISCONNECTED` state)
- [x] Disconnect giữa discovery rollback staging đúng. (clean staging, release serializer, preserve committed)
- [x] Delete device xóa schema + state + MCP exposure. (thêm `device_state_forget()` vào delete flow)

### Exit criteria

- [x] Source tree không còn legacy domain concepts. (`list_device_capabilities` / `device_command` removed)
- [ ] Full test suite pass.
- [ ] Hardware E2E pass.
- [ ] Memory/leak test pass.

---

# 6. Release-gate test matrix

| ID | Test | Expected |
|---|---|---|
| T01 | Protocol v1 | Reject |
| T02 | Protocol v2 | Reject |
| T03 | Protocol v3 | Reject |
| T04 | Protocol v4 | Accept |
| T05 | 2 tools + 1 feature discovery | Atomic commit READY |
| T06 | Missing tool item | Reject staging |
| T07 | Missing feature item | Reject staging |
| T08 | Duplicate command | Reject staging |
| T09 | Duplicate feature_id | Reject staging |
| T10 | Missing feature_tool target | Reject writable feature/schema |
| T11 | `read_feature_state` BOOL | State cache seeded |
| T12 | `feature_state` BOOL | Runtime cache updated |
| T13 | Two devices same native model | State isolated by gateway device_id |
| T14 | Multi-feature device | Multiple templates rendered |
| T15 | Unknown feature type/schema | Safe unsupported fallback |
| T16 | Existing `dev_caps` | No migration, rediscover v4 |
| T17 | Existing `dev_list` v2 | Preserve ID/name/BLE identity, drop type |
| T18 | Reboot with `dev_schema` | Schema restored |
| T19 | Corrupt schema NVS | Ignore safely / rediscover |
| T20 | Disconnect during discovery | Rollback staging |
| T21 | Refresh failure | Previous committed schema retained |
| T22 | Local device state change | Web/MCP state updates |
| T23 | Semantic MCP duplicate policy | No duplicate bound raw tool |
| T24 | 16 devices x 12 tools x 8 features | No stack overflow/leak |

---

# 7. Memory acceptance checklist

- [ ] Per-device records dùng PSRAM-preferred allocation.
- [ ] Staging schema dùng PSRAM-preferred allocation.
- [ ] Queue items không embed schema snapshot lớn.
- [ ] `gw_message_t` copies được bounded.
- [ ] HTTP handlers không đặt whole schema lên stack.
- [ ] MCP handlers không đặt whole schema lên stack.
- [ ] Đo worker stack high-water mark sau migration.
- [ ] So sánh heap trước/sau 100 refresh cycles.
- [ ] Không có monotonic PSRAM loss.
- [ ] Không có monotonic internal SRAM loss.
- [ ] Largest free internal block vẫn trong ngưỡng an toàn.

---

# 8. Reference LED expected schema

```json
{
  "device_id": "gateway-assigned-id",
  "revision": 2,
  "tools": [
    {
      "name": "set_led",
      "value_type": "boolean"
    },
    {
      "name": "get_state",
      "value_type": "none"
    }
  ],
  "features": [
    {
      "feature_id": "led_main",
      "feature_type": "on_off_light",
      "schema_version": 1,
      "property": "on_off",
      "write_tool": "set_led",
      "template": "on_off_light.v1"
    }
  ]
}
```

Không còn:

```json
"device_type": "light"
```

---

# 9. Exact rename map

| Old | New |
|---|---|
| `components/device_capabilities` | `components/device_schema` |
| `device_capabilities.h` | `device_schema.h` |
| `device_capability_t` | `device_schema_tool_t` |
| `device_capability_snapshot_t` | remove |
| `DEVICE_CAP_MAX_PER_DEVICE` | `DEVICE_SCHEMA_MAX_TOOLS` |
| `device_capabilities_init` | `device_schema_init` |
| `device_capabilities_on_ready` | `device_schema_on_ready` |
| `device_capabilities_on_disconnect` | `device_schema_on_disconnect` |
| `device_capabilities_on_notify` | `device_schema_on_notify` |
| `device_capabilities_refresh` | `device_schema_refresh` |
| `device_capabilities_forget` | `device_schema_forget` |
| `device_capabilities_get` | remove |
| `list_device_capabilities` | `get_device_schema` |
| `web_capability_api.c` | `web_device_schema_api.c` |
| `dev_caps` | `dev_schema` |
| `CAP_*` internal prefix | `SCHEMA_*` |
| `device_caps` log tag | `device_schema` |

---

# 10. Device-type removal search checklist

## Gateway

- [ ] `GW_MSG_DEVICE_TYPE_LEN`
- [ ] `GW_KEY_DEVICE_TYPE`
- [ ] `gw_message_t.device_type`
- [ ] `DEVICE_TYPE_MAX_LEN`
- [ ] `device_entry_t.type`
- [ ] `device_store_add(... type)`
- [ ] `device_store_edit(... type)`
- [ ] `type_N` NVS writer
- [ ] `"type"` trong `/api/devices`
- [ ] Device type form field
- [ ] Device type UI badge
- [ ] MCP metadata dựa trên device type
- [ ] Tests expecting `generic`

## Device

- [ ] `GW_MSG_DEVICE_TYPE_LEN`
- [ ] `GW_KEY_DEVICE_TYPE`
- [ ] `gw_message_t.device_type`
- [ ] `device_app_profile_t.device_type`
- [ ] `.device_type = ...`
- [ ] `"device_type"` trong product.json
- [ ] Device type logs
- [ ] Codec tests cho device_type

## Must remain

- [ ] `gw_message_t.type`
- [ ] `GW_MSG_TYPE_*`
- [ ] `feature_type`
- [ ] `gw_feature_type_t`

---

# 11. Definition of Done

- [ ] Gateway không còn component `device_capabilities`.
- [ ] Gateway domain model chính là `device_schema`.
- [ ] Gateway chỉ accept Protocol v4.
- [ ] BLE Device chỉ accept/emit Protocol v4.
- [ ] `device_type` bị xóa khỏi cả hai repository.
- [ ] `gw_message_t.type` vẫn hoạt động cho protocol routing.
- [ ] `feature_type` vẫn hoạt động cho template selection.
- [ ] `dev_caps` không được migrate.
- [ ] Device registry giữ BLE identity sau khi drop type.
- [ ] Schema discovery commit atomic tools + features.
- [ ] `read_feature_state` seed state đúng.
- [ ] `feature_state` update state đúng.
- [ ] Web UI render theo Device Template.
- [ ] MCP và Xiaozhi dùng cùng semantic catalog.
- [ ] Feature writes route qua `feature_tool`.
- [ ] Không hardcode command theo feature type.
- [ ] Unknown feature/template degrade safely.
- [ ] Schema records ưu tiên PSRAM.
- [ ] Không có giant snapshot copy lên task stack.
- [ ] Interop tests pass.
- [ ] Gateway unit tests pass.
- [ ] Device host tests pass.
- [ ] Hardware E2E pass.
- [ ] Memory/leak tests pass.

---

# 12. Kiến trúc cuối cùng

```text
Physical Device
    |
    | no device category
    |
    +-- Tool: set_led
    +-- Tool: get_state
    |
    +-- Feature: led_main
           |
           +-- type: ON_OFF_LIGHT
           +-- schema: 1
           +-- property: ON_OFF
           +-- write_tool: set_led
                  |
                  v
          Device Template
                  |
          +-------+-------+
          |       |       |
          v       v       v
         Web     MCP   Xiaozhi
```

Protocol v4 là BLE application protocol duy nhất được hỗ trợ.
