# ESP32 BLE Gateway — Protocol v4 + Device Schema Development Plan

**Version:** 1.0  
**Target Gateway:** `hailp-vn38/esp-ble-gateway`  
**Reference Device:** `hailp-vn38/esp-ble-device`

## 1. Mục tiêu

Chuyển ESP32 BLE Gateway hoàn toàn sang kiến trúc:

```text
BLE Device
   │
   │ Protocol v4 / CBOR
   ▼
Protocol Codec
   │
   ▼
device_schema
   ├── tools[]
   └── features[]
          │
          ├── device_template
          └── device_state
                   │
             ┌─────┼─────┐
             ▼     ▼     ▼
            Web   MCP  Xiaozhi
```

Matter không còn thuộc kiến trúc.

Nguồn semantic chính của gateway là:

```text
device_schema
+
device_state
+
device_template
```

---

# 2. Các quyết định bắt buộc

## D1. Chỉ hỗ trợ Protocol v4

```text
v1 -> reject
v2 -> reject
v3 -> reject
v4 -> accept
```

Validation:

```c
if (msg->protocol_version != GW_PROTOCOL_VERSION) {
    return GW_ERR_UNSUPPORTED_VERSION;
}
```

```c
#define GW_PROTOCOL_VERSION 4u
```

Xóa toàn bộ logic:

```c
protocol_version >= 3
```

hoặc:

```c
version >= 1 && version <= GW_PROTOCOL_VERSION
```

Không default protocol bị thiếu về v3.

---

# 3. Chuyển hoàn toàn sang `device_schema`

Xóa:

```text
components/device_capabilities/
```

Thay bằng:

```text
components/device_schema/
    CMakeLists.txt
    README.md
    include/
        device_schema.h
    device_schema.c
    device_schema_store.c
    device_schema_validate.c
```

Không giữ compatibility wrapper `device_capabilities`.

Các khái niệm mới:

```text
device schema
schema revision
schema discovery
schema refresh
tool
feature
property
```

Xóa public API:

```text
device_capability_snapshot_t
device_capabilities_get()
device_capabilities_refresh()
device_capabilities_forget()
device_capabilities_on_notify()
```

Thay bằng:

```text
device_schema_info_t
device_schema_get_info()
device_schema_get_tool()
device_schema_get_feature()
device_schema_find_feature()
device_schema_refresh()
device_schema_forget()
device_schema_on_notify()
```

---

# 4. Giữ nguyên tên wire của Protocol v4

Device hiện đã dùng:

```text
describe_capabilities
capabilities_begin
capability_item
feature_item
capabilities_end
```

Không đổi wire protocol trong phase này.

Gateway chỉ coi chúng là transport message của một:

```text
device_schema discovery transaction
```

Tức là:

```text
capability_item -> schema.tools[]
feature_item    -> schema.features[]
```

Tên `capability` chỉ còn tồn tại ở wire contract v4, không còn là domain model của gateway.

---

# 5. Xóa `device_type`

Device không còn được phân loại:

```text
light
fan
sensor
plug
generic
```

Device chỉ chứa:

```text
device_id
name
BLE identity
tools[]
features[]
state
```

Một device có thể đồng thời có:

```text
light feature
temperature feature
contact feature
fan feature
```

nên việc ép cả device thành một `type` không còn phù hợp.

## Xóa

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

## Không xóa `gw_message_t.type`

Phải giữ:

```c
char type[GW_MSG_TYPE_LEN];
```

vì đây là message kind:

```text
device_command
device_ack
device_event
capabilities_begin
capability_item
feature_item
capabilities_end
```

## Không xóa `feature_type`

Phải giữ:

```c
gw_feature_type_t feature_type;
```

vì:

```text
feature_type + feature_schema_version
```

là key dùng để chọn Device Template.

---

# 6. CBOR key 7

Không renumber CBOR keys.

Đổi:

```c
GW_KEY_DEVICE_TYPE = 7
```

thành:

```c
GW_KEY_RESERVED_7 = 7
```

Giữ:

```text
8..30
```

không thay đổi.

Encoder:

```text
không emit key 7
```

Decoder:

```text
nếu gặp key 7 -> ignore
```

Nhờ vậy numeric contract của Protocol v4 vẫn ổn định.

---

# 7. Protocol v4 semantic keys

Gateway phải bổ sung:

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

Giữ:

```c
typedef enum {
    GW_FEATURE_NONE = 0,
    GW_FEATURE_GENERIC_RELAY = 1,
    GW_FEATURE_ON_OFF_PLUGIN_UNIT = 10,
    GW_FEATURE_ON_OFF_LIGHT = 11,
    GW_FEATURE_DIMMABLE_LIGHT = 12,
    GW_FEATURE_FAN = 20,
    GW_FEATURE_TEMPERATURE_SENSOR = 30,
    GW_FEATURE_HUMIDITY_SENSOR = 31,
    GW_FEATURE_CONTACT_SENSOR = 40,
} gw_feature_type_t;
```

và:

```c
typedef enum {
    GW_PROP_NONE = 0,
    GW_PROP_ON_OFF = 1,
    GW_PROP_LEVEL = 2,
    GW_PROP_PERCENT_SETTING = 3,
    GW_PROP_PERCENT_CURRENT = 4,
    GW_PROP_TEMPERATURE = 5,
    GW_PROP_HUMIDITY = 6,
    GW_PROP_CONTACT = 7,
} gw_feature_property_t;
```

---

# 8. Thay đổi phía BLE Device

## `gateway_protocol.h`

```text
GW_PROTOCOL_VERSION = 4
```

Xóa:

```text
GW_MSG_DEVICE_TYPE_LEN
gw_message_t.device_type
GW_KEY_DEVICE_TYPE
```

Thêm:

```text
GW_KEY_RESERVED_7
```

Header/comment chỉ còn mô tả Protocol v4.

## `gateway_protocol.c`

Xóa:

```text
v1 compatibility
v2 compatibility
v3 compatibility
device_type encode/decode
```

Decoder chỉ nhận:

```text
protocol_version == 4
```

## `device_app_profile_t`

Từ:

```c
typedef struct {
    const char *model;
    const char *device_type;
    ...
} device_app_profile_t;
```

thành:

```c
typedef struct {
    const char *model;
    const char *hardware_version;
    const char *firmware_version;
    const char *ble_name_prefix;
    uint8_t protocol_version;
    uint32_t capability_revision;
    ...
} device_app_profile_t;
```

`model` là native identity, không phải device category.

## Reference product

Xóa:

```c
.device_type = "light",
```

Xóa legacy:

```c
bool new_state = request->protocol_version >= 3
                     ? request->bool_value != 0
                     : request->int_value != 0;
```

Dùng:

```c
bool new_state = request->bool_value != 0;
```

## `product.json`

Xóa:

```json
"device_type": "light"
```

Đổi:

```json
"protocol_version": 4
```

---

# 9. Gateway `cbor_codec`

Gateway hiện phải chuyển hẳn sang v4.

Thêm:

```c
#define GW_PROTOCOL_VERSION 4u
#define GW_FEATURE_ID_LEN 32u
```

`gw_message_t` bổ sung:

```c
char feature_id[GW_FEATURE_ID_LEN];
int has_feature_id;

uint8_t feature_type;
int has_feature_type;

uint16_t feature_schema_version;
int has_feature_schema_version;

uint16_t feature_flags;
int has_feature_flags;

uint8_t property_id;
int has_property_id;

bool feature_value_bool;
int has_feature_value_bool;

int32_t feature_value_int;
int has_feature_value_int;

char feature_tool[GW_MSG_COMMAND_LEN];
int has_feature_tool;

uint16_t feature_total;
int has_feature_total;
```

Xóa:

```text
GW_MSG_DEVICE_TYPE_LEN
gw_message_t.device_type
```

---

# 10. `device_schema` model

Giới hạn ban đầu:

```c
#define DEVICE_SCHEMA_MAX_TOOLS    12
#define DEVICE_SCHEMA_MAX_FEATURES  8
```

## Tool

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

## Feature

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

## Schema info

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

---

# 11. API của `device_schema`

Không copy nguyên snapshot lớn.

Dùng:

```c
esp_err_t device_schema_get_info(
    const char *device_id,
    device_schema_info_t *out_info);

esp_err_t device_schema_get_tool(
    const char *device_id,
    size_t index,
    device_schema_tool_t *out_tool);

esp_err_t device_schema_get_feature(
    const char *device_id,
    size_t index,
    device_schema_feature_t *out_feature);

esp_err_t device_schema_find_feature(
    const char *device_id,
    const char *feature_id,
    device_schema_feature_t *out_feature);

esp_err_t device_schema_refresh(
    const char *device_id);

esp_err_t device_schema_forget(
    const char *device_id);

bool device_schema_on_notify(
    const char *device_id,
    const gw_message_t *message);
```

Không tạo:

```c
device_schema_get(device_id, &huge_snapshot);
```

để tránh copy hàng KB lên task stack.

---

# 12. Schema discovery

Protocol:

```text
Gateway
   |
   | describe_capabilities
   ▼
Device
   |
   | capabilities_begin
   | capability_item x N
   | feature_item x M
   | capabilities_end
   | device_ack
```

Gateway:

```text
device_schema
    tools[N]
    features[M]
```

## Begin

Lưu:

```text
snapshot_id
tool_total
feature_total
revision
```

Khởi tạo staging.

## Tool validation

Kiểm tra:

```text
command hợp lệ
value type hợp lệ
min <= max
step > 0 đối với integer
sequence hợp lệ
không duplicate command
```

## Feature validation

Kiểm tra:

```text
feature_id != empty
feature_type != NONE
schema_version > 0
property_id != NONE
value_type hợp lệ
write_tool hợp lệ
```

`write_tool` được resolve ở cuối transaction.

## Atomic commit

Chỉ commit nếu:

```text
received_tools == expected_tools
received_features == expected_features
snapshot_id match
tools valid
features valid
writable feature_tool resolve được
```

Nếu fail:

```text
discard staging
giữ committed schema cũ
```

Không partial commit.

---

# 13. NVS của `device_schema`

Namespace mới:

```c
#define DEVICE_SCHEMA_NVS_NAMESPACE "dev_schema"
#define DEVICE_SCHEMA_STORE_VERSION 1
```

Không migrate:

```text
dev_caps
```

Startup:

```text
load dev_schema
ignore dev_caps
rediscover schema khi cần
```

Có thể erase `dev_caps` để lấy lại NVS space.

Không persist:

```text
staging
refresh state
request_id
BLE runtime
feature runtime state
```

---

# 14. Device registry sau khi bỏ type

`device_entry_t`:

```c
typedef struct {
    char device_id[DEVICE_ID_MAX_LEN];
    char name[DEVICE_NAME_MAX_LEN];

    uint8_t ble_addr[6];
    uint8_t ble_addr_type;
    bool has_ble_identity;
} device_entry_t;
```

API:

```c
device_store_add(device_id, name);
device_store_edit(device_id, new_name);
```

Xóa:

```text
DEVICE_TYPE_MAX_LEN
entry.type
```

## `dev_list`

Bump:

```text
schema 2 -> schema 3
```

Migration:

```text
id_N    -> giữ
name_N  -> giữ
type_N  -> bỏ
addr_N  -> giữ
atype_N -> giữ
```

Mục đích là user không phải add/pair lại device.

Đây không phải migration capability v3.

---

# 15. `device_state`

Tạo:

```text
components/device_state/
```

Key:

```text
gateway_device_id
+
feature_id
+
property_id
```

Ví dụ:

```text
dev_A / led_main / ON_OFF
dev_B / led_main / ON_OFF
```

Không dùng native model làm physical identity.

## Seed state

Sau khi schema READY:

```text
read_feature_state(feature_id, property_id)
```

để lấy state thật từ device.

## Realtime

Consume:

```text
device_event
command = feature_state
```

và update state cache.

State chưa cần persist NVS.

---

# 16. Notify routing

```c
static void on_device_notify(
    const char *device_id,
    const gw_message_t *msg)
{
    if (device_schema_on_notify(device_id, msg))
        return;

    if (device_state_on_notify(device_id, msg))
        return;

    command_dispatcher_on_device_notify(device_id, msg);
}
```

Ownership:

```text
capabilities_begin
capability_item
feature_item
capabilities_end
    -> device_schema

feature_state
    -> device_state

device_ack
generic device_event
    -> dispatcher/event system
```

---

# 17. Device Template

Tạo:

```text
components/device_template/
```

Lookup:

```text
feature_type + schema_version
```

Ví dụ:

```text
ON_OFF_LIGHT + 1
    -> on_off_light.v1
```

Descriptor:

```c
typedef struct {
    gw_feature_type_t feature_type;
    uint16_t schema_version;

    const char *template_id;
    const char *semantic_name;

    uint8_t primary_property;
    bool expose_semantic_mcp;
} device_template_t;
```

Registry static:

```c
static const device_template_t s_templates[] = {
    {
        .feature_type = GW_FEATURE_ON_OFF_LIGHT,
        .schema_version = 1,
        .template_id = "on_off_light.v1",
        .semantic_name = "light",
        .primary_property = GW_PROP_ON_OFF,
        .expose_semantic_mcp = true,
    },
};
```

Không cần JSON template parser trên ESP32.

Không cần heap cho template registry.

---

# 18. `feature_tool` binding

Ví dụ:

```text
feature_id     = led_main
feature_type   = ON_OFF_LIGHT
property       = ON_OFF
feature_tool   = set_led
```

Không được:

```text
ON_OFF_LIGHT -> hardcode set_led
```

Device khác có thể là:

```text
feature_tool = power
```

Template vẫn là:

```text
on_off_light.v1
```

Write flow:

```text
Web/MCP
   ↓
feature instance
   ↓
feature.write_tool
   ↓
raw device tool
```

---

# 19. Web API

Device API không còn:

```json
"type": "light"
```

Create device:

```json
{
  "device_id": "bedroom_01",
  "name": "Bedroom device",
  "ble_addr": "AA:BB:CC:DD:EE:FF",
  "ble_addr_type": 0
}
```

Đổi:

```text
list_device_capabilities
```

thành:

```text
get_device_schema
```

Đổi:

```text
web_capability_api.c
```

thành:

```text
web_device_schema_api.c
```

Response:

```json
{
  "device_id": "bedroom_01",
  "state": "ready",
  "revision": 2,
  "tools": [
    {
      "name": "set_led",
      "value_type": "boolean"
    }
  ],
  "features": [
    {
      "feature_id": "led_main",
      "feature_type": "on_off_light",
      "schema_version": 1,
      "template": "on_off_light.v1",
      "property": "on_off",
      "write_tool": "set_led",
      "state": true,
      "state_valid": true
    }
  ]
}
```

---

# 20. Web UI

Không render theo:

```text
device.type
```

Render theo:

```text
feature.template
```

Ví dụ:

```text
on_off_light.v1
    -> toggle
```

Một device có nhiều feature thì render nhiều semantic cards.

Có thể giữ:

```text
Advanced / Raw Tools
```

cho command không có template.

---

# 21. MCP và Xiaozhi

Catalog gồm:

```text
raw tools
semantic feature tools
```

Policy mặc định:

```text
raw tool đã bind vào feature
    -> không expose duplicate

semantic feature
    -> expose tool semantic

raw tool không thuộc feature
    -> expose bình thường
```

Ví dụ:

```text
set_led
    -> bound to led_main
    -> không expose trực tiếp mặc định

led_main
    -> expose semantic light control
```

Local MCP và Xiaozhi phải đọc chung:

```text
device_schema
+
device_template
+
device_state
```

Không xây hai mapping khác nhau.

---

# 22. Unknown feature

Nếu gateway chưa có template:

```text
feature_type = 99
schema_version = 1
```

thì:

```text
schema vẫn commit
feature stored
raw tool vẫn hoạt động
state vẫn lưu nếu value type hiểu được
template = unsupported
gateway không crash
```

Không reject cả device.

---

# 23. Memory policy

Internal SRAM:

```text
mutex
queue
worker control
completion metadata
```

PSRAM preferred:

```text
committed schemas
staging schemas
queued gw_message_t copies
```

Không copy nguyên schema lên:

```text
HTTP task stack
MCP task stack
worker task stack
```

Dùng indexed getters.

---

# 24. Rename map

| Old | New |
|---|---|
| `device_capabilities` | `device_schema` |
| `device_capabilities.h` | `device_schema.h` |
| `device_capability_t` | `device_schema_tool_t` |
| `device_capability_snapshot_t` | remove |
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
| `CAP_*` | `SCHEMA_*` |

---

# 25. Implementation order

## V4-01 — Device cleanup ✅ DONE (2026-08-31)

- [x] Strict v4.
- [x] Remove device type.
- [x] Reserve CBOR key 7.
- [x] Fix `product.json`.
- [x] Remove compatibility code.
- [x] Update tests.

## V4-02 — Gateway codec ✅ DONE (2026-08-31)

- [x] v4 fields.
- [x] strict decoder.
- [x] no device type.
- [x] CBOR interoperability tests.

## V4-03 — `device_schema` ✅ DONE (2026-08-31)

- [x] Replace component.
- [x] tools + features staging.
- [x] atomic commit.
- [x] new NVS.
- [x] delete old component.

## V4-04 — Gateway device-store cleanup

- Remove type.
- `dev_list` schema 3.
- retain device ID/name/BLE identity.

## V4-05 — `device_state`

- `read_feature_state`.
- feature state cache.
- realtime `feature_state`.

## V4-06 — `device_template`

- Static registry.
- `on_off_light.v1`.
- `feature_tool` binding.

## V4-07 — Web

- schema API.
- semantic UI.
- raw Advanced fallback.

## V4-08 — MCP/Xiaozhi

- semantic catalog.
- hide duplicate raw tools.
- shared exposure layer.

## V4-09 — Cleanup

Delete all remaining:

```text
device_capabilities
device_type
v3 compatibility
v3 capability cache loading
```

---

# 26. Release-gate tests

1. `v1/v2/v3` reject, `v4` accept.
2. Discovery `2 tools + 1 feature`.
3. Missing item prevents atomic commit.
4. Writable feature referencing missing tool is rejected.
5. `read_feature_state` seeds state.
6. `feature_state` updates state without polling.
7. Two identical models remain isolated by gateway `device_id`.
8. One device can contain multiple feature types.
9. Unknown feature template does not reject device.
10. Existing `dev_caps` is never migrated.
11. Existing device registry keeps BLE identity while dropping type.
12. 16 devices × 12 tools × 8 features passes memory/leak tests.

---

# 27. Reference LED result

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

Không tồn tại:

```json
"device_type": "light"
```

---

# 28. Definition of Done

Hoàn tất khi:

- Gateway không còn component `device_capabilities`.
- `device_schema` là source of truth.
- Chỉ Protocol v4 được chấp nhận.
- `device_type` bị xóa ở cả hai repo.
- `gw_message_t.type` vẫn dùng cho message routing.
- `feature_type` vẫn dùng cho semantic/template.
- `dev_caps` không migrate.
- Device registry giữ được BLE identity.
- Schema commit atomic tools + features.
- State được seed bằng `read_feature_state`.
- State realtime cập nhật bằng `feature_state`.
- Web render từ Device Template.
- MCP và Xiaozhi dùng cùng semantic catalog.
- Write sử dụng `feature_tool`.
- Unknown template degrade an toàn.
- Schema records ưu tiên PSRAM.
- Tất cả interoperability và hardware tests pass.

---

# 29. Kiến trúc cuối

```text
Physical Device
    |
    | không có device category
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
                  ▼
          Device Template
                  |
          +-------+-------+
          |       |       |
          ▼       ▼       ▼
         Web     MCP   Xiaozhi
```

Gateway semantic source of truth:

```text
device_schema
+
device_state
+
device_template
```

Protocol v4 là BLE application protocol duy nhất được hỗ trợ.