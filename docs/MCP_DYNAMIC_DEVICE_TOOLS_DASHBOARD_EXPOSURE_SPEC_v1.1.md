# MCP Dynamic Device Tools & Dashboard Exposure Development Spec

**Version:** 1.2  
**Status:** Implementation Ready — reviewed baseline  
**Date:** 2026-08-28  
**Project:** ESP32 BLE Gateway  
**Target:** ESP32-S3 / ESP-IDF  
**Depends on:** MCP dual-era refactor v1.1 completed successfully  
**Repository:** `hailp-vn38/esp-ble-gateway`

---

## Revision 1.2 — các thay đổi sau code review đối chiếu hiện trạng

- cập nhật HTTP route budget theo hiện trạng thực tế: dual-era POST/GET/DELETE `/mcp` **đã land** trong code (17 handler web + 3 handler MCP = 20/21 hiện tại, không còn là 19 "chưa gồm dual-era"); `mcp_endpoint_register()` bắt buộc check return của GET/DELETE registration (hiện đang bỏ qua — silent degradation nếu tràn budget);
- đổi tên config TTL thành `CONFIG_MCP_TOOLS_CACHE_TTL_MS`, áp dụng cho **cả static và dynamic** `tools/list`, thay thế define hardcode `MCP_TOOLS_CACHE_TTL_MS 60000` trong `mcp_endpoint_internal.h`; default giữ 60000 để không đổi behavior hiện có;
- yêu cầu `device_capability_snapshot_t` expose thêm `has_committed` (hiện public API không phân biệt "chưa từng commit" với "committed nhưng state kẹt DISCOVERING sau refresh fail"); exposure không được gate theo `state == READY`;
- cảnh báo không reuse `normalize_arguments()` cho dynamic tool (helper hiện copy `device_id` từ agent arguments và map `value` → `int_value`);
- bổ sung file list còn thiếu: `test/CMakeLists.txt` (TEST_COMPONENTS), `main/main.c` (init order), `components/mcp_endpoint/mcp_endpoint.c` (register error handling);
- ghi chú init-order/provisioning-mode cho `mcp_tool_exposure_init()`;
- ghi chú listener firing point sau persist attempt trong `handle_end()`.

---

## Revision 1.1 — các thay đổi bắt buộc sau review

Bản v1.1 cập nhật các vấn đề kiến trúc và security được phát hiện khi đối chiếu v1.0 với implementation hiện tại của gateway:

- tách **enabled tool capacity** khỏi **exposure record capacity**;
- làm tool naming deterministic, không phụ thuộc catalog/collision state hiện tại;
- không expose full `list_device_capabilities` cho Agent trong production dynamic mode;
- thêm admin authorization boundary cho Dashboard exposure API;
- cập nhật HTTP route budget sau MCP dual-era + exposure routes;
- thay fingerprint 32-bit bằng SHA-256 truncated 128-bit trên canonical semantic fields;
- định nghĩa rõ fail-closed semantics khi NVS persist thất bại;
- chốt admin mutation đi trực tiếp qua typed `mcp_tool_exposure` service, không thêm field MCP vào `gw_message_t`;
- coi peripheral-provided label/unit là untrusted metadata và không đưa trực tiếp vào model-facing prose;
- định nghĩa callback capability-commit ngoài capability mutex và chỉ enqueue reconcile;
- chốt phase 1 không pagination và không list-change transport;
- cập nhật test matrix, file-change list và Definition of Done.

---

# 1. Mục tiêu

Tài liệu này định nghĩa kiến trúc và kế hoạch triển khai chức năng:

> User chọn các command của từng BLE device trong Dashboard để expose thành MCP tools riêng. AI Agent chỉ nhìn thấy và chỉ có thể gọi các command đã được user cho phép.

Mục tiêu cuối cùng:

```text
BLE Device
    |
    | describe_capabilities
    v
device_capabilities
    |
    | persisted capability snapshot
    v
Dashboard
    |
    | user enables selected commands
    v
mcp_tool_exposure
    |
    | persistent user policy
    v
Dynamic MCP Tool Catalog
    |
    | tools/list
    v
AI Agent
    |
    | tools/call
    v
MCP Binding Resolver
    |
    | fixed device_id + command
    v
command_executor
    |
    v
command_dispatcher
    |
    v
BLE Device
```

Phạm vi tài liệu này **không** bao gồm:

- MCP handshake/session refactor;
- MCP 2025/2026 negotiation;
- SSE;
- `subscriptions/listen`;
- OAuth;
- remote cloud synchronization;
- multi-user authorization;
- user roles;
- device firmware capability protocol redesign.

Các phần trên phải được xem là dependency hoặc phase tương lai.

---

# 2. Nguyên tắc kiến trúc

## 2.1 Device capability và MCP exposure là hai khái niệm khác nhau

Phải giữ boundary rõ:

```text
device_capabilities
    = thiết bị CÓ THỂ làm gì

mcp_tool_exposure
    = user CHO PHÉP AI làm gì
```

Không thêm trực tiếp:

```c
bool expose_to_mcp;
```

vào `device_capability_t`.

Lý do:

- capability là dữ liệu do peripheral quảng bá;
- MCP exposure là policy do user sở hữu;
- device không được tự mở rộng quyền của AI;
- refresh capability không được tự động enable tool;
- UI policy phải persist độc lập với BLE runtime.

---

## 2.2 User approval là nguồn quyền chính

Một command chỉ được expose nếu đồng thời:

```text
device exists
AND
capability snapshot exists
AND
command exists in capability snapshot
AND
user explicitly enabled it
AND
exposure state == ENABLED
AND
firmware hard-policy (nếu có) cho phép
```

Không được expose command chỉ vì peripheral quảng bá command đó.

---

## 2.3 MCP tool phải bind cứng với device + command

Không để Agent tự gửi:

```json
{
  "device_id": "...",
  "command": "..."
}
```

đối với dynamic device tool.

Ví dụ public tool:

```text
living_room_light.set_power
```

phải bind nội bộ:

```text
device_id = living_room_light
command   = set_power
```

Agent chỉ truyền argument thật sự cần cho command:

```json
{
  "value": true
}
```

Lợi ích:

- giảm attack surface;
- giảm hallucination;
- giảm lỗi chọn device;
- schema đơn giản hơn;
- tên tool thể hiện intent;
- policy được enforce trước dispatch.

---

# 3. Hiện trạng project liên quan

## 3.1 MCP registry hiện tại

`components/mcp_endpoint/mcp_registry.c` hiện expose static tools:

```text
get_status
list_devices
list_device_capabilities
device_command
```

Target production sau dynamic exposure:

```text
Static Gateway Tools
    get_status
    list_devices

Dynamic Device Tools
    living_room_light.set_power
    living_room_light.set_brightness
    bedroom_fan.set_speed
```

Hai thay đổi bắt buộc:

1. `device_command` không còn public sau migration; nó chỉ là internal execution primitive.
2. `list_device_capabilities` **không được expose public trong production dynamic mode**, vì tool này hiện trả toàn bộ command peripheral quảng bá và sẽ làm Agent nhìn thấy cả command user đã disable. Dashboard vẫn đọc full capability qua REST/admin path.

Nếu cần debug compatibility, có thể giữ compile-time flag riêng:

```text
CONFIG_MCP_EXPOSE_FULL_CAPABILITY_TOOL=n
```

Default production phải là `n`. Nếu flag được bật cho debug, tài liệu/README phải ghi rõ nó bypass mục tiêu “Agent chỉ nhìn thấy approved command metadata”.

## 3.2 Device capabilities đã đủ metadata để sinh schema

Current `device_capability_t` đã có:

```c
char command[];
char label[];
char unit[];

device_cap_value_type_t value_type;

uint8_t flags;

int32_t min_value;
int32_t max_value;
uint32_t step;
```

Các type hiện tại:

```text
DEVICE_CAP_VALUE_NONE
DEVICE_CAP_VALUE_BOOL
DEVICE_CAP_VALUE_INT
```

Flags:

```text
DEVICE_CAP_FLAG_IDEMPOTENT
DEVICE_CAP_FLAG_DESTRUCTIVE
```

Do đó không cần tạo một schema language mới cho MCP.

---

## 3.3 Capability snapshot đã persist

Capability snapshot hiện được lưu NVS và được load lại sau reboot.

Điều này cho phép:

- MCP tools tồn tại khi BLE device tạm offline;
- `tools/list` không nhảy liên tục theo trạng thái kết nối;
- agent catalog ổn định;
- reconnect không gây tool add/remove churn.

Dynamic tool catalog phải dựa trên:

```text
persisted capability snapshot
+
persisted user exposure policy
```

Không dựa trực tiếp vào:

```text
BLE connected == true
```

---

# 4. Kiến trúc component mới

Đề xuất component:

```text
components/
└── mcp_tool_exposure/
    ├── CMakeLists.txt
    ├── Kconfig.projbuild
    ├── include/
    │   └── mcp_tool_exposure.h
    ├── mcp_tool_exposure.c
    ├── mcp_tool_exposure_store.c
    ├── mcp_tool_name.c
    ├── mcp_tool_catalog.c
    └── test/
        ├── test_mcp_tool_exposure.c
        ├── test_mcp_tool_name.c
        ├── test_mcp_tool_catalog.c
        └── test_mcp_tool_exposure_store.c
```

Có thể gộp file ở implementation đầu tiên, nhưng API boundary phải giữ như tài liệu.

---

# 5. Dependency direction

Allowed:

```text
web_server
    |
    +--> web_admin_auth
    |       |
    |       v
    +--> mcp_tool_exposure

mcp_endpoint
    |
    v
mcp_tool_exposure

mcp_tool_exposure
    |
    +--> device_capabilities
    +--> device_store
    +--> NVS
    +--> SHA-256 implementation

command_dispatcher/gateway_commands
    |
    +--> mcp_tool_exposure   // chỉ cho revoke khi device delete
```

Không cho:

```text
device_capabilities --> mcp_endpoint
device_capabilities --> web_server
mcp_tool_exposure --> web_server
mcp_tool_exposure --> esp_http_server
web_server --> NVS exposure blob trực tiếp
```

`mcp_tool_exposure` phải độc lập HTTP transport.

### Quyết định v1.1: exposure admin mutation không đi qua `gw_message_t`

Exposure configuration là **gateway control-plane configuration**, không phải BLE/device command. Vì vậy:

```text
Dashboard PUT /api/mcp/exposures
        |
        v
web_admin_auth
        |
        v
mcp_tool_exposure_admin_apply(...)
        |
        v
NVS + RAM catalog
```

Không thêm field:

```c
bool mcp_enabled;
bool has_mcp_enabled;
```

vào `gw_message_t`. `gw_message_t` tiếp tục là shared gateway/CBOR/device message contract.

Exception duy nhất ở `gateway_commands.c` là lifecycle hook khi xóa device: command handler phải revoke exposure của device để dynamic tools biến mất ngay lập tức.

---

# 6. Public API đề xuất

```c
#ifndef MCP_TOOL_EXPOSURE_H
#define MCP_TOOL_EXPOSURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "device_capabilities.h"

#define MCP_DYNAMIC_TOOL_NAME_MAX 128
#define MCP_CAPABILITY_DIGEST_LEN 16

typedef enum {
    MCP_EXPOSURE_ENABLED = 0,
    MCP_EXPOSURE_NEEDS_REVIEW,
    MCP_EXPOSURE_ORPHANED,
} mcp_exposure_state_t;

typedef enum {
    MCP_EXPOSURE_REASON_NONE = 0,
    MCP_EXPOSURE_REASON_CAPABILITY_CHANGED,
    MCP_EXPOSURE_REASON_COMMAND_MISSING,
    MCP_EXPOSURE_REASON_DEVICE_MISSING,
    MCP_EXPOSURE_REASON_POLICY_BLOCKED,
    MCP_EXPOSURE_REASON_PERSIST_DIRTY,
} mcp_exposure_reason_t;

typedef struct {
    char device_id[GW_MSG_DEVICE_ID_LEN];
    char command[GW_MSG_COMMAND_LEN];
    char tool_name[MCP_DYNAMIC_TOOL_NAME_MAX + 1];

    mcp_exposure_state_t state;
    mcp_exposure_reason_t reason;

    uint8_t capability_digest[MCP_CAPABILITY_DIGEST_LEN];
} mcp_tool_exposure_t;

typedef struct {
    char tool_name[MCP_DYNAMIC_TOOL_NAME_MAX + 1];
    char device_id[GW_MSG_DEVICE_ID_LEN];
    char command[GW_MSG_COMMAND_LEN];
    device_capability_t capability;
} mcp_tool_binding_t;

typedef struct {
    bool confirm_destructive;
} mcp_exposure_enable_options_t;

esp_err_t mcp_tool_exposure_init(void);

/* Privilege grant: publish ENABLED only after persistence succeeds. */
esp_err_t mcp_tool_exposure_enable(
    const char *device_id,
    const char *command,
    const mcp_exposure_enable_options_t *options);

/* Privilege reduction: hide immediately in RAM, then persist. */
esp_err_t mcp_tool_exposure_disable(
    const char *device_id,
    const char *command);

esp_err_t mcp_tool_exposure_get(
    const char *device_id,
    const char *command,
    mcp_tool_exposure_t *out);

esp_err_t mcp_tool_exposure_snapshot(
    mcp_tool_exposure_t *out,
    size_t capacity,
    size_t *out_count);

esp_err_t mcp_tool_catalog_find(
    const char *tool_name,
    mcp_tool_binding_t *out_binding);

esp_err_t mcp_tool_catalog_snapshot(
    mcp_tool_binding_t *out,
    size_t capacity,
    size_t *out_count,
    uint32_t *out_revision);

/* Callback path only enqueues work; reconciliation occurs in exposure worker. */
esp_err_t mcp_tool_exposure_reconcile_device_async(
    const char *device_id,
    uint32_t capability_revision);

/* Device deletion must revoke in RAM immediately, then persist best-effort. */
esp_err_t mcp_tool_exposure_forget_device(
    const char *device_id);

uint32_t mcp_tool_catalog_revision(void);

#endif
```

Tên API có thể điều chỉnh, nhưng semantics về **grant**, **revoke**, **digest**, **async reconcile** và ownership phải giữ.

---

# 7. Data model

## 7.1 Disabled command không cần persisted authorization record

Phase 1 giữ rule:

```text
absence of exposure record == disabled
```

Khi user disable:

```text
record bị revoke khỏi executable catalog ngay
-> record có thể bị xóa khỏi persisted policy sau khi save thành công
```

Stable identity **không phụ thuộc persisted disabled record**. Tool name phải được tạo deterministic từ immutable naming algorithm v1 (§10).

Persisted states:

```text
ENABLED
NEEDS_REVIEW
ORPHANED
```

## 7.2 Enabled capacity và record capacity là hai giới hạn khác nhau

Không dùng một `CONFIG_MCP_DYNAMIC_TOOL_MAX` cho cả hai.

Bắt buộc tách:

```text
CONFIG_MCP_DYNAMIC_TOOL_MAX_ENABLED
    = số dynamic tool tối đa xuất hiện trong tools/list

CONFIG_MCP_EXPOSURE_RECORD_MAX
    = số policy/history record tối đa giữ trong exposure store
```

Default đề xuất:

```text
MAX_ENABLED = 32
MAX_RECORDS = 96
```

Project hiện có tối đa 16 device và 12 capability/device, tức theoretical capability surface = 192 command. Record max không bắt buộc bằng 192 ở default, nhưng phải configurable tới 192.

Dashboard phải hiển thị cả hai:

```text
Enabled tools:   18 / 32
Policy records:  26 / 96
```

Nếu record store đầy dù enabled count còn chỗ, enable mới phải fail với machine error `mcp_exposure_record_capacity_exceeded`. User có thể disable/remove orphaned records bằng cùng admin mutation API.

## 7.3 Ý nghĩa state

### ENABLED

Tool:

- xuất hiện trong `tools/list`;
- có thể resolve;
- có thể execute;
- semantic digest khớp current capability;
- hard policy vẫn cho phép.

### NEEDS_REVIEW

Tool:

- không xuất hiện trong `tools/list`;
- không execute;
- vẫn xuất hiện trong Dashboard;
- user phải review và enable lại.

Dùng khi semantic/safety contract thay đổi, ví dụ:

- `value_type`;
- destructive/idempotent flags;
- integer min/max/step;
- unit semantics;
- firmware hard policy bắt đầu block.

Display-only `label` không tự động trigger review.

### ORPHANED

Tool:

- không xuất hiện trong MCP;
- không execute;
- Dashboard báo command không còn tồn tại.

Dùng khi device vẫn tồn tại nhưng capability snapshot không còn command.

Nếu device bị delete hoàn toàn, exposure phải bị revoke khỏi RAM ngay và persisted record phải được xóa/best-effort cleanup.

---

# 8. NVS persistence

## 8.1 Namespace

Đề xuất:

```text
namespace: mcp_exp
key:       catalog
```

## 8.2 Versioned catalog blob

```c
#define MCP_EXP_STORE_SCHEMA_VERSION 2

typedef struct {
    uint8_t schema_version;
    uint8_t reserved0;
    uint16_t count;
    uint32_t catalog_revision;

    mcp_exposure_persisted_record_t records[
        CONFIG_MCP_EXPOSURE_RECORD_MAX
    ];
} mcp_exposure_store_blob_t;
```

Implementation nên ghi variable-length blob theo `count`, không bắt buộc ghi toàn bộ max array.

Không dùng NVS key theo `device_id` vì key length limit, migration và transactional update khó kiểm soát.

## 8.3 Persisted record phải compact

Tool name **không cần persist** trong schema v2 vì naming algorithm v1 deterministic và là compatibility contract.

```c
typedef struct {
    char device_id[GW_MSG_DEVICE_ID_LEN];
    char command[GW_MSG_COMMAND_LEN];

    uint8_t state;
    uint8_t reason;
    uint8_t naming_version;
    uint8_t reserved;

    uint8_t capability_digest[MCP_CAPABILITY_DIGEST_LEN];
} mcp_exposure_persisted_record_t;
```

Không persist:

```text
tool description/title
label
min/max/step
flags/value_type
connection state
```

Source-of-truth capability vẫn là `device_capabilities`.

## 8.4 Naming version là compatibility contract

```text
naming_version = 1
```

Algorithm v1 không được silently thay đổi trong firmware sau này. Nếu cần algorithm v2, migration phải giữ tên v1 cho record cũ hoặc có explicit migration version.

## 8.5 Flash wear

Exposure write chỉ xảy ra khi:

- user enable/disable;
- capability reconcile đổi authorization state;
- device delete;
- dirty revoke retry.

Không ghi mỗi tool call.

## 8.6 Corruption policy

Nếu catalog blob corrupt/schema unsupported:

```text
load no executable dynamic grants
log diagnostic
Dashboard reports exposure store recovery required
```

Không cố đoán/repair bằng cách auto-enable từ capability snapshot.

---

# 9. Catalog revision

Duy trì:

```c
uint32_t s_catalog_revision;
```

Increment khi visible MCP catalog thay đổi.

Ví dụ:

```text
enable tool            -> revision++
disable tool           -> revision++
ENABLED -> REVIEW      -> revision++
REVIEW -> ENABLED      -> revision++
device delete          -> revision++
metadata change that
alters generated schema -> revision++
```

Không increment khi:

```text
device disconnect
device reconnect
BLE RSSI change
command execution result
```

Revision phải persist hoặc được re-derived.

Khuyến nghị persist trong exposure catalog.

---

# 10. Tool naming

## 10.1 Tool identity phải stable và deterministic

Không dùng `device.name`. Tool identity derive từ immutable logical `device_id` + command.

Base format:

```text
<device-token>.<command>
```

## 10.2 Character policy

Generator chỉ emit:

```text
A-Z a-z 0-9 _ - .
```

Command đã được capability layer validate theo tập tương thích.

## 10.3 Naming algorithm v1

### Fast path

Nếu original `device_id`:

- chỉ chứa allowed chars;
- không empty;
- `<device_id>.<command>` <= 128 bytes;

thì dùng trực tiếp:

```text
living_room_light.set_power
```

### Sanitized path

Nếu phải sanitize hoặc truncate, **luôn** append deterministic SHA-256-derived suffix; không đợi tới khi phát hiện collision trong current catalog.

```text
<slug>_<hash16>.<command>
```

Trong đó `hash16` là 16 hex characters từ SHA-256(original `device_id`).

Ví dụ:

```text
original: living room/light
slug:     living_room_light
name:     living_room_light_7d0c3a9e81b1c452.set_power
```

Nếu total >128, truncate `slug` để chừa đủ chỗ cho `_hash16.<command>`.

## 10.4 Collision semantics

Naming **không phụ thuộc**:

- NVS insertion order;
- current enabled tools;
- device connection order;
- việc một tool khác đang disabled.

Sau generation vẫn phải check uniqueness. Nếu cực hiếm SHA-derived suffix collision xảy ra, fail closed và trả `mcp_tool_name_collision`; không tự chọn suffix dựa trên mutable catalog order.

## 10.5 Disable/re-enable phải giữ cùng tool name

Vì algorithm deterministic:

```text
enable -> disable -> reboot -> enable
```

phải tạo lại cùng tool name ngay cả khi record disabled không được persist.

Unit test bắt buộc cover case hai `device_id` khác nhau canonicalize về cùng slug.

---

# 11. Capability semantic digest

## 11.1 Mục đích

Digest là safety guard để phát hiện command execution contract thay đổi. Nó không phải bearer authorization token, nhưng là **security-relevant state**, vì mismatch phải revoke tool khỏi executable catalog.

## 11.2 Algorithm bắt buộc

Dùng SHA-256 trên canonical field stream và lưu 128-bit đầu:

```c
uint8_t digest[MCP_CAPABILITY_DIGEST_LEN]; // 16 bytes
```

Không dùng CRC32/FNV32 cho authorization reconciliation.

## 11.3 Canonical semantic fields

Bắt buộc hash theo fixed order:

```text
command
value_type
flags
min_value
max_value
step
unit
```

`label` không tham gia semantic digest vì label là display metadata.

`DEVICE_CAP_FLAG_DESTRUCTIVE` thay đổi luôn làm digest đổi.

Unit được giữ trong digest dù không đưa raw unit trực tiếp vào model-facing prose, vì thay đổi unit có thể thay đổi physical semantics của integer value.

## 11.4 Không hash raw struct memory

Sai:

```c
sha256(&capability, sizeof(capability))
```

Phải encode canonical bytes:

```text
command UTF-8 bytes + NUL
value_type u8
flags u8
min LE32
max LE32
step LE32
unit UTF-8 bytes + NUL
```

Mọi field phải zero/normalize rõ trước hash.

## 11.5 Comparison

Digest compare dùng constant-length compare. Nếu mismatch:

```text
ENABLED -> NEEDS_REVIEW
remove from runtime catalog immediately
catalog_revision++
persist downgrade best-effort
```

---

# 12. Reconciliation

## 12.1 Khi capability refresh thành công

Sau committed snapshot đổi:

```text
device_capabilities
        |
        v
mcp_tool_exposure_reconcile_device(device_id)
```

Không gọi reconcile trực tiếp từ NimBLE callback.

Nó phải chạy ở task/context an toàn.

---

## 12.2 Existing command unchanged

Nếu:

```text
digest_new == digest_saved
```

giữ:

```text
ENABLED
```

Không tăng catalog revision.

---

## 12.3 Existing command changed

Nếu:

```text
digest_new != digest_saved
```

set:

```text
state  = NEEDS_REVIEW
reason = CAPABILITY_CHANGED
```

Tool lập tức biến mất khỏi visible catalog.

Increment catalog revision.

Persist state.

---

## 12.4 Command disappeared

Nếu command không còn trong snapshot:

```text
state  = ORPHANED
reason = COMMAND_MISSING
```

Tool biến mất.

Exposure record vẫn giữ để Dashboard giải thích.

---

## 12.5 Command quay lại

Không tự enable.

Nếu ORPHANED command quay lại:

```text
ORPHANED -> NEEDS_REVIEW
```

User phải xác nhận lại.

---

# 13. Device disconnect behavior

Transient disconnect không được thay đổi exposure.

```text
connected
   |
disconnected
   |
connected
```

Tool catalog phải stable.

Nếu Agent call tool khi device offline:

```text
tools/call
   |
   v
binding resolves
   |
   v
command execution
   |
   v
device unavailable
```

Trả MCP tool error:

```json
{
  "content": [
    {
      "type": "text",
      "text": "Device is currently unavailable"
    }
  ],
  "isError": true
}
```

Không trả protocol-level unknown tool.

---

# 14. Device delete behavior

Device deletion phải revoke MCP authorization **trước khi delete flow có thể hoàn tất**.

Target runtime order:

```text
delete_device
    |
    +--> MCP exposure revoke in RAM immediately
    |       +--> persist cleanup best-effort / dirty retry
    |
    +--> capability forget
    +--> BLE peer forget
    +--> device store delete
```

Nếu exposure persist fail:

- dynamic tools của device vẫn phải biến mất khỏi RAM catalog ngay;
- delete flow có thể tiếp tục theo device lifecycle policy;
- dirty state được retry;
- boot reconciliation phải thấy device không còn trong `device_store` và không load executable grant.

Không bao giờ giữ tool executable chỉ vì cleanup NVS thất bại.

---

# 15. Boot reconciliation

Trong:

```text
mcp_tool_exposure_init()
```

flow:

```text
load persisted exposure catalog
        |
        v
for each exposure
        |
        +--> device exists?
        |       no -> remove / invalidate
        |
        +--> capability snapshot exists?
        |       no -> NEEDS_REVIEW or ORPHANED
        |
        +--> command exists?
        |       no -> ORPHANED
        |
        +--> semantic digest matches?
                no -> NEEDS_REVIEW
                yes -> ENABLED
```

Do current capability component load persisted snapshot as READY, boot catalog có thể được revalidated mà không cần BLE connection.

Lưu ý boot mode (theo AGENTS.md): `mcp_tool_exposure_init()` chỉ được gọi trong **STA mode sau khi có IP** (cùng chỗ với `device_capabilities_init()`/`command_dispatcher_init()` trong `main/main.c`), không gọi ở provisioning mode. Boot reconcile khả thi vì `device_store_init()` và `device_capabilities_init()` (load NVS snapshot) chạy trước web/MCP registration.

---

# 16. Dynamic tool catalog

## 16.1 Catalog build source

Visible catalog production:

```text
Static MCP tools
    get_status
    list_devices

+

ENABLED dynamic bindings only
```

Không lấy trực tiếp từ all device capabilities.

`list_device_capabilities` không nằm trong production MCP static catalog khi dynamic exposure bật. Dashboard vẫn đọc full capability qua `/api/capabilities`.

## 16.2 Deterministic order

1. static tools theo fixed table order;
2. dynamic tools sort lexicographically by `tool_name`.

Không sort theo NVS order/connection time/last updated.

## 16.3 Capacity

Runtime dynamic catalog chỉ chứa tối đa:

```text
CONFIG_MCP_DYNAMIC_TOOL_MAX_ENABLED = 32
```

Persistent exposure policy có capacity riêng:

```text
CONFIG_MCP_EXPOSURE_RECORD_MAX = 96
```

Đây là hai failure class khác nhau:

```text
enabled limit full -> mcp_tool_capacity_exceeded
record store full  -> mcp_exposure_record_capacity_exceeded
```

## 16.4 Phase 1 không pagination

Do enabled dynamic tools bị hard-cap, phase 1 trả toàn bộ list trong một response và **không emit `nextCursor`**.

Nếu client gửi non-empty `cursor` khi server không có pagination state:

```text
JSON-RPC -32602 invalid cursor
```

Không tạo custom pagination field.

Nếu benchmark 32 tools vượt response/heap budget, giảm default max trước; pagination là phase riêng.

---

# 17. Kconfig

Đề xuất:

```text
menu "MCP Dynamic Tools"

config MCP_DYNAMIC_TOOLS
    bool "Enable dashboard-managed dynamic MCP tools"
    default y

config MCP_DYNAMIC_TOOL_MAX_ENABLED
    int "Maximum enabled dynamic MCP tools"
    range 8 64
    default 32

config MCP_EXPOSURE_RECORD_MAX
    int "Maximum persisted MCP exposure records"
    range 32 192
    default 96

config MCP_KEEP_GENERIC_DEVICE_COMMAND
    bool "Keep generic device_command visible"
    default n
    help
        Migration/debug compatibility only.
        Production target should keep this disabled.

config MCP_EXPOSE_FULL_CAPABILITY_TOOL
    bool "Expose full list_device_capabilities tool over MCP"
    default n
    help
        Debug only. When enabled, agents can inspect commands that are not
        approved for dynamic exposure. Production target should keep n.

config MCP_DYNAMIC_ALLOW_DESTRUCTIVE
    bool "Allow destructive dynamic tools to be enabled"
    default n
    help
        Even when enabled, the admin API still requires explicit
        confirm_destructive=true per grant.

endmenu
```

### Quyết định v1.2: TTL config chung cho static + dynamic

```text
config MCP_TOOLS_CACHE_TTL_MS
    int "tools/list cache TTL in milliseconds (MCP 2026)"
    range 0 60000
    default 60000
```

- Thuộc menu `MCP Endpoint` (mcp_endpoint), không thuộc menu Dynamic Tools, vì TTL áp dụng cho **cả** static và dynamic `tools/list` — cùng một code path `mcp_tools_list()`.
- Thay thế define hardcode `MCP_TOOLS_CACHE_TTL_MS 60000` trong `mcp_endpoint_internal.h`.
- Default **60000** (giữ nguyên behavior hiện có), không phải 10000 như v1.1 đề xuất — thay đổi TTL là quyết định tuning riêng, không trộn vào feature này.

Dashboard admin authorization config nên thuộc `web_server`/`web_admin_auth`, không thuộc MCP protocol Kconfig.

---

# 18. Tool schema generation

## 18.1 NONE

Capability:

```text
value_type = NONE
```

Schema:

```json
{
  "type": "object",
  "properties": {},
  "additionalProperties": false
}
```

Call:

```json
{
  "name": "front_door.lock",
  "arguments": {}
}
```

---

## 18.2 BOOL

Capability:

```text
set_power
BOOL
```

Schema:

```json
{
  "type": "object",
  "properties": {
    "value": {
      "type": "boolean"
    }
  },
  "required": ["value"],
  "additionalProperties": false
}
```

---

## 18.3 INT

Capability:

```text
set_brightness
INT
min  = 0
max  = 100
step = 1
unit = "%"
```

Schema:

```json
{
  "type": "object",
  "properties": {
    "value": {
      "type": "integer",
      "minimum": 0,
      "maximum": 100,
      "multipleOf": 1,
      "description": "Brightness (%)"
    }
  },
  "required": ["value"],
  "additionalProperties": false
}
```

---

## 18.4 Step semantics

Chỉ add:

```json
"multipleOf": step
```

nếu:

```text
step > 0
```

Current capability validator yêu cầu integer capability có step > 0.

Execution side vẫn phải validate bằng `device_capabilities_validate_command()`.

JSON Schema không thay thế runtime validation.

---

# 19. Tool metadata and untrusted peripheral text

## 19.1 Model-facing description phải dùng trusted template

Không đưa trực tiếp peripheral-provided `label`, raw `unit`, hoặc mutable display name vào prose gửi cho model.

Phase 1 description:

```text
Execute command '<command>' on device '<device_id>'.
```

Ví dụ:

```text
Execute command 'set_brightness' on device 'living_room_light'.
```

Đây là deterministic, bounded, và không biến BLE metadata thành prompt-injection surface.

## 19.2 Dashboard có thể dùng friendly metadata

Dashboard UI được phép hiển thị:

```text
device name
capability label
unit
```

nhưng phải HTML-escape và giới hạn length. Đây là presentation data, không phải model instruction.

## 19.3 Tool title

Phase 1 có thể omit MCP `title` cho dynamic tools để tránh mutable display metadata làm catalog churn. Nếu sau này thêm title, phải generate từ sanitized/bounded trusted data và không dùng làm identity.

## 19.4 Annotations

Map:

```text
readOnlyHint = false
DEVICE_CAP_FLAG_IDEMPOTENT  -> idempotentHint
DEVICE_CAP_FLAG_DESTRUCTIVE -> destructiveHint
```

Dynamic device command luôn được coi là side-effect capable; không advertise read-only chỉ vì command tên giống getter.

---

# 20. Destructive command policy

## 20.1 Default hard policy

Default:

```text
CONFIG_MCP_DYNAMIC_ALLOW_DESTRUCTIVE=n
```

Khi `n`, Dashboard vẫn có thể hiển thị destructive capability nhưng server từ chối enable.

## 20.2 Khi firmware cho phép destructive dynamic tools

Để grant một destructive command, cả ba điều kiện phải đúng:

```text
admin endpoint authenticated
CONFIG_MCP_DYNAMIC_ALLOW_DESTRUCTIVE=y
request.confirm_destructive == true
```

Thiếu bất kỳ điều kiện nào -> không grant.

Body ví dụ:

```json
{
  "device_id": "front_door",
  "command": "unlock",
  "enabled": true,
  "confirm_destructive": true
}
```

UI phải hiển thị warning rõ và yêu cầu explicit confirmation.

## 20.3 Không auto-reapprove

Nếu destructive flag hoặc semantic digest thay đổi:

```text
ENABLED -> NEEDS_REVIEW
```

Không tự động restore ENABLED sau refresh/reboot.

---

# 21. Runtime authorization pipeline

Target execution:

```text
tools/call
   |
   v
find tool in dynamic catalog
   |
   | not found
   +--> normal MCP unknown tool
   |
   v
read binding
   |
   v
revalidate exposure == ENABLED
   |
   v
read current capability
   |
   v
verify command exists + semantic digest current
   |
   v
validate arguments
   |
   v
check firmware hard policy
   |
   v
build gw_message_t
   |
   v
command_executor_submit
   |
   v
command_dispatcher
```

Không trust cached binding blindly nếu catalog có thể update concurrently.

---

# 22. Concurrency

## 22.1 Required synchronization

Exposure/catalog state có thể được access từ:

- HTTP Dashboard request;
- MCP HTTP request;
- capability reconcile worker;
- device deletion;
- boot initialization.

Phải có mutex.

Ví dụ:

```c
static SemaphoreHandle_t s_exposure_mutex;
```

Không giữ mutex trong:

- NVS long operation nếu có thể snapshot trước;
- command execution;
- BLE wait;
- JSON serialization lớn.

---

## 22.2 Snapshot pattern

Cho `tools/list`:

```text
lock
copy binding snapshot
copy catalog_revision
unlock

build cJSON outside lock
```

Cho `tools/call`:

```text
lock
resolve binding copy
unlock

validate current capability
execute
```

---

# 23. Catalog rebuild strategy

Không cần lưu cJSON catalog lâu dài.

Preferred:

```text
persist exposure records
+
generate tools/list JSON on request
```

Có thể cache lightweight binding snapshot trong RAM.

Không cache cJSON trees across requests trừ khi đo được lợi ích rõ.

Lý do:

- ownership phức tạp;
- heap fragmentation;
- invalidation khó;
- max 32 tools vẫn manageable.

---

# 24. MCP 2026 behavior

Dynamic catalog tương thích MCP 2026 stateless.

`tools/list`:

- build từ current catalog snapshot;
- deterministic;
- include cache hints.

Đề xuất:

```text
ttlMs = CONFIG_MCP_DYNAMIC_TOOLS_CACHE_TTL_MS
cacheScope = private
```

Default:

```text
10000 ms
```

Khi dynamic tools disabled có thể dùng TTL dài hơn.

---

# 25. MCP 2025 behavior

MCP 2025 không có `ttlMs` list result theo mô hình 2026.

Phase 1:

```text
tools.listChanged = false
```

vì gateway compatibility design không có long-lived SSE/list notification path.

Sau Dashboard update:

- new `tools/list` sees new catalog;
- existing 2025 client có thể cần reconnect hoặc explicitly re-list tools.

Dashboard phải mô tả điều này.

Không khai báo `listChanged=true` nếu gateway không thể deliver notification.

---

# 26. MCP 2026 list-change future phase

MCP 2026 chuyển list-change delivery sang client-opened:

```text
subscriptions/listen
```

Phase 1 không triển khai.

Future phase có thể:

```text
catalog_revision change
        |
        v
publish tools list_changed
        |
        v
active subscriptions/listen stream
```

Không mở scope này trong implementation đầu tiên.

---

# 27. Dashboard API and admin security boundary

## 27.1 Exposure API là authorization control plane

Các route exposure phải được bảo vệ bằng **admin credential riêng**, không reuse MCP bearer token. MCP client không được có quyền mutate MCP exposure chỉ vì nó có quyền gọi MCP.

Đề xuất helper:

```text
components/web_server/web_admin_auth.c
components/web_server/web_admin_auth.h
```

Config/NVS source có thể theo pattern:

```text
CONFIG_WEB_ADMIN_AUTH_TOKEN
namespace: web_admin
key: token
```

Nếu admin token chưa cấu hình:

```text
GET/PUT exposure admin routes -> fail closed
error.code = admin_auth_not_configured
```

Không có dev-mode “empty token means allow” cho mutation exposure.

Bearer token đi qua HTTP plaintext trên LAN nên không phải TLS-equivalent security. Không expose các route này ra Internet; future HTTPS là phase riêng.

## 27.2 Request security checks

Exposure GET/PUT phải:

- validate Bearer admin token bằng constant-time compare;
- validate `Host`;
- nếu có `Origin`, yêu cầu same-origin/allowlisted origin;
- không bật permissive CORS;
- response `Cache-Control: no-store`;
- không log token;
- reject body oversize bằng existing web body helpers.

Dashboard nên giữ token trong memory/session storage, không bake token vào firmware HTML/JS asset.

## 27.3 GET exposure state của một device

```http
GET /api/mcp/exposures?device_id=<id>
Authorization: Bearer <admin-token>
```

Response:

```json
{
  "device_id": "living_room_light",
  "catalog_revision": 12,
  "capacity": {
    "enabled": 2,
    "max_enabled": 32,
    "records": 4,
    "max_records": 96
  },
  "commands": [
    {
      "command": "set_power",
      "label": "Power",
      "value_type": "boolean",
      "enabled": true,
      "state": "enabled",
      "tool_name": "living_room_light.set_power",
      "destructive": false,
      "idempotent": true
    }
  ]
}
```

API merge capability snapshot + exposure records; disabled command không cần persisted record.

## 27.4 PUT single hoặc bulk trên cùng URI

Để tiết kiệm route handlers, chỉ dùng:

```http
PUT /api/mcp/exposures
Authorization: Bearer <admin-token>
Content-Type: application/json
```

Single:

```json
{
  "device_id": "living_room_light",
  "command": "set_power",
  "enabled": true
}
```

Bulk:

```json
{
  "device_id": "living_room_light",
  "commands": [
    {"command": "set_power", "enabled": true},
    {"command": "set_brightness", "enabled": true}
  ]
}
```

Không tạo `/api/mcp/exposures/device` ở phase 1.

## 27.5 Admin service call

Sau parse/auth, `web_server` gọi typed service:

```text
web_gateway_api
    -> mcp_tool_exposure_enable/disable/apply_bulk
```

Không gọi NVS trực tiếp và không serialize exposure mutation vào `gw_message_t`.

## 27.6 HTTP route budget

Hiện trạng **thực tế sau dual-era** (v1.2 cập nhật): web_server component đăng ký 17 handler gateway mode (assets 5 + gateway API 7 + system API 2 + BLE API 3), `mcp_endpoint` đăng ký 3 handler POST/GET/DELETE `/mcp` — tổng **20 handler đang dùng trên budget 21** (`WEB_GATEWAY_MAX_URI_HANDLERS 21` trong `web_server.c`). Comment cũ trong code ghi "19" đã stale.

Dynamic exposure thêm GET+PUT cùng `/api/mcp/exposures` (thêm 2):

```text
20 current (đã gồm dual-era MCP POST/GET/DELETE)
+2 exposure GET/PUT
=22
```

Bắt buộc update:

```c
#define WEB_GATEWAY_MAX_URI_HANDLERS 28
```

`28` để có headroom nhỏ cho future admin route. Test startup phải fail nếu route registration vượt budget; không silently bỏ route.

Bắt buộc sửa kèm (v1.2): `mcp_endpoint_register()` hiện **không check** return của GET/DELETE `/mcp` registration — nếu budget tràn, POST thành công còn GET/DELETE âm thầm 404 thay vì 405. Phải check và fail loud như POST.

---

# 28. Dashboard mutation contract

v1.1 chốt một contract duy nhất:

```text
Dashboard
    |
    v
web_server
    |
    v
web_admin_auth
    |
    v
mcp_tool_exposure typed admin service
    |
    +--> RAM authorization state
    +--> NVS persistence
```

`web_server` không ghi NVS trực tiếp.

Exposure configuration **không bắt buộc đi qua `command_dispatcher`**, vì nó không phải BLE/device command và current gateway đã có precedent service-oriented admin operations như capability refresh.

`command_dispatcher` vẫn là execution path cho command tới gateway/device; control-plane policy store có typed service riêng.

Security boundary nằm ở `web_admin_auth`, không nằm ở MCP registry.

---

# 29. Integration với command dispatcher / device lifecycle

Không thêm command:

```text
set_mcp_exposure
get_mcp_exposures
```

vào `gw_message_t`/CBOR protocol.

`command_dispatcher` chỉ cần integration hook cho **device delete**, vì delete đang được thực hiện trong `gateway_commands.c`.

Target delete order về authorization:

```text
1. mcp_tool_exposure_forget_device(device_id)
   - revoke/hide all dynamic tools in RAM immediately
   - persist cleanup best-effort / dirty retry

2. device_capabilities_forget(device_id)

3. BLE peer forget

4. device_store_delete(device_id)
```

Nếu exposure persistence fail ở bước 1, device deletion không được làm tool executable trở lại. Boot reconciliation sẽ thấy device không còn trong `device_store` và tiếp tục fail closed.

Nếu product muốn capability-forget vẫn là first irreversible storage step, có thể đổi thứ tự storage operations, nhưng **runtime MCP revoke phải luôn xảy ra trước khi device delete flow có thể hoàn tất**.

---

# 30. Dashboard UI

## 30.1 Device Detail

Section:

```text
AI / MCP Commands
```

Mỗi capability:

```text
Power
set_power
Boolean
[ Enabled for AI ]

MCP tool:
living_room_light.set_power
```

---

## 30.2 Integer command

Display:

```text
Brightness
set_brightness

Type: Integer
Range: 0 - 100
Step: 1
Unit: %

[ Enabled for AI ]

Tool:
living_room_light.set_brightness
```

---

## 30.3 Destructive

Display:

```text
Factory Reset
factory_reset

DESTRUCTIVE

[ ] Enabled for AI

Warning:
This command can cause destructive or irreversible effects.
```

Nếu user bật, yêu cầu explicit confirmation modal.

Không dùng pre-checked destructive toggle.

---

## 30.4 Needs Review

Display:

```text
Brightness
MCP exposure paused

Capability changed:
Old: integer 0..100
New: integer 0..255

[ Review and re-enable ]
```

Nếu không lưu old capability details thì UI chỉ cần:

```text
Capability changed. Review required.
```

Không cần persist full old schema chỉ để diff.

---

## 30.5 Global MCP page

Đề xuất:

```text
Settings
  -> AI / MCP
```

Hiển thị:

```text
Dynamic tools: Enabled
Enabled tools: 18 / 32
Catalog revision: 42

Living Room Light
  ON  set_power
  ON  set_brightness
  OFF factory_reset

Bedroom Fan
  ON  set_power
  ON  set_speed

Front Door
  ON  lock
  OFF unlock
```

Có filter:

```text
Enabled only
Needs review
Destructive
By device
```

---

# 31. Generic `device_command` migration

## Phase A

Trong development:

```text
CONFIG_MCP_KEEP_GENERIC_DEVICE_COMMAND=y
```

Dynamic tools chạy song song.

Mục tiêu:

- integration test;
- compare paths;
- rollback dễ.

---

## Phase B

Production default:

```text
CONFIG_MCP_KEEP_GENERIC_DEVICE_COMMAND=n
```

`device_command` biến mất khỏi `tools/list`.

Execution function bên trong vẫn giữ để reuse.

---

## Phase C

Sau khi stable:

- xóa legacy generic public schema;
- giữ internal helper;
- remove Kconfig compatibility flag nếu không cần.

---

# 32. Tool resolution

Resolver order:

```text
1. static registry
2. dynamic catalog
3. unknown tool
```

Pseudo-code:

```c
const mcp_tool_desc_t *static_tool =
    mcp_registry_find_static(name);

if (static_tool != NULL) {
    return resolve_static(...);
}

mcp_tool_binding_t binding;
if (mcp_tool_catalog_find(name, &binding) == ESP_OK) {
    return resolve_dynamic(&binding, arguments, ...);
}

return unknown_tool;
```

---

# 33. Dynamic argument normalization

> **Cảnh báo v1.2:** không reuse `normalize_arguments()` hiện có trong `mcp_tools.c` cho dynamic tool. Helper này (a) copy `device_id` từ agent-supplied arguments — sẽ phá vỡ hard binding §2.3/§34 nếu agent truyền `device_id`; (b) map legacy `"value"` → `int_value` cho `message_type=device_command` — sẽ coerce sai BOOL argument. Dynamic path phải build `gw_message_t` trực tiếp từ binding.

## NONE

Reject any unknown property.

Expected:

```json
{}
```

Build:

```c
msg.type = "device_command";
msg.device_id = binding.device_id;
msg.command = binding.command;
```

---

## BOOL

Expected:

```json
{"value": true}
```

Build:

```c
msg.has_bool_value = true;
msg.bool_value = true;
```

Không accept:

```json
{"bool_value": true}
```

trên public dynamic tool nếu schema public dùng `value`.

Public schema nên đơn giản.

---

## INT

Expected:

```json
{"value": 80}
```

Build:

```c
msg.has_int_value = true;
msg.int_value = 80;
```

Sau đó gọi:

```c
device_capabilities_validate_command(...)
```

để enforce range/step/type.

---

# 34. Defense in depth

Một tools/call dynamic phải kiểm tra lại:

```text
tool exists
exposure enabled
device exists
capability command exists
semantic digest matches
argument valid
hard policy allows
```

Không chỉ rely vào `tools/list` trước đó.

Agent có thể call stale tool name.

Stale call phải fail closed.

---

# 35. Stale agent call

Ví dụ:

1. agent cached `living_room_light.set_power`;
2. user disable tool;
3. agent call tool cũ.

Gateway phải:

```text
catalog lookup -> not found
```

Trả unknown/not available theo MCP tool-call behavior.

Không execute vì tool từng tồn tại.

---

# 36. Capability changed during call

Race:

```text
resolve binding
        |
capability refresh commits
        |
execute
```

Preferred:

- resolve binding copy;
- immediately revalidate current capability semantic digest before building message;
- capability service itself vẫn validate command trước dispatch.

Nếu semantic digest mismatch:

```text
tool error:
"Tool capability changed and requires review."
```

Không execute.

---

# 37. Firmware hard policy

Current Kconfig allowlist có thể được chuyển vai trò.

Không nên có hai equal runtime sources:

```text
Dashboard exposure
vs
Kconfig allowlist
```

Target semantics:

```text
Dashboard exposure
    = runtime user authorization

Kconfig policy
    = optional hard ceiling
```

Ví dụ firmware build:

```text
set_power,set_brightness,set_speed
```

thì user không thể expose:

```text
factory_reset
```

dù device advertise nó.

Nếu hard ceiling empty, quyết định phải rõ:

### Recommended production semantics

Nếu dynamic exposure feature enabled:

```text
empty hard ceiling = no additional firmware restriction
```

hoặc tạo separate flag:

```text
CONFIG_MCP_ENFORCE_DEVICE_COMMAND_HARD_ALLOWLIST
```

Không reuse semantics cũ "empty = deny all" vì sẽ làm Dashboard enable nhưng không chạy.

Migration phải explicit.

---

# 38. Proposed policy API

Thay:

```c
mcp_device_command_allowed(command)
```

bằng tư duy:

```c
bool mcp_hard_policy_allows_device_command(
    const char *command);
```

Execution:

```text
dynamic exposure authorizes
AND
hard policy allows
```

Nếu hard policy disabled:

```text
true
```

---

# 39. Error semantics

## User disabled tool

Tool không có trong catalog:

```text
unknown tool / tool not found
```

Không leak hidden binding details.

---

## Device offline

Tool exists nhưng execution failed:

```text
CallToolResult
isError = true
"Device is currently unavailable."
```

---

## Capability changed

Nếu call xảy ra trước catalog refresh:

```text
isError = true
"Tool capability changed and requires user review."
```

Exposure chuyển NEEDS_REVIEW nếu chưa chuyển.

---

## Argument out of range

Tool error:

```text
isError = true
"Value must be between 0 and 100 in steps of 1."
```

Không dùng JSON-RPC protocol error cho device/application outcome.

---

# 40. Security invariants

Bắt buộc test các invariants:

```text
Peripheral cannot self-enable MCP exposure.
```

```text
Dashboard cannot mutate exposure without valid admin credential.
```

```text
MCP bearer token alone cannot mutate exposure policy.
```

```text
Dashboard cannot expose command absent from committed capability snapshot.
```

```text
Agent cannot override bound device_id or command.
```

```text
Disabled / NEEDS_REVIEW / ORPHANED tool never dispatches.
```

```text
Changed destructive flag or semantic digest revokes immediately.
```

```text
Persist failure during revoke never restores executable privilege.
```

```text
Deleted device leaves no executable MCP tool, including after reboot.
```

```text
BLE reconnect does not change authorization.
```

```text
Peripheral label/text cannot inject arbitrary model-facing instructions.
```

---

# 41. Memory budget

Có hai static capacities:

```text
CONFIG_MCP_DYNAMIC_TOOL_MAX_ENABLED = 32
CONFIG_MCP_EXPOSURE_RECORD_MAX      = 96
```

Runtime binding memory chỉ cần scale theo enabled tool count. Persistent policy RAM/cache có thể scale theo record count.

Persisted record schema v2 không lưu `tool_name`, giúp giảm RAM/NVS footprint đáng kể.

Benchmark bắt buộc:

```text
heap before exposure init
heap after 32 enabled bindings
heap after 96 policy records
largest free block
tools/list peak allocation
SHA-256 reconcile stack/heap impact
```

Không hardcode assumption “vài trăm byte/tool” mà không đo trên target build.

---

# 42. JSON response size

`tools/list` phase 1 chứa:

```text
2 production static tools
+ tối đa CONFIG_MCP_DYNAMIC_TOOL_MAX_ENABLED dynamic tools
```

Benchmark:

```text
2 static + 8 dynamic
2 static + 16 dynamic
2 static + 32 dynamic
```

Measure:

```text
serialized JSON bytes
heap peak
HTTP response time
largest free block after request
```

Phase 1 không pagination. Nếu 32 tools vượt budget, giảm default max trước khi cân nhắc cursor pagination đúng MCP.

---

# 43. Tool descriptions và token budget

Description phải ngắn, deterministic và không chứa raw peripheral text.

Good:

```text
Execute command 'set_brightness' on device 'living_room_light'.
```

Không dùng:

```text
<label from BLE> ... arbitrary prose ...
```

Schema đã chứa type/min/max/step. Dashboard giữ friendly label/unit riêng cho human UI.

---

# 44. `tools/list` cache and list-change policy

MCP 2026 dynamic catalog phase 1:

```text
ttlMs = 10000
cacheScope = private
```

TTL là project policy, không phải protocol requirement.

Phase 1:

```text
2025 tools capability listChanged = false
2026 no subscriptions/listen implementation
no unsolicited list_changed transport
```

Dashboard sau mutation có thể báo:

```text
MCP catalog updated.
Modern clients may refresh within configured cache TTL.
Legacy clients may need to reconnect or re-list tools.
```

Catalog revision chỉ dùng Dashboard/diagnostics, không thêm custom field vào standard `tools/list`.

Phase 1 không emit `nextCursor`; xem §16.4.

---

# 45. Optional catalog ETag-style diagnostics

Không thêm custom MCP wire fields.

Nhưng Dashboard/admin API có thể expose:

```json
{
  "catalog_revision": 42
}
```

Dùng để debug.

Không add:

```text
catalog_revision
```

vào standard `tools/list` nếu spec không có field tương ứng.

---

# 46. Integration với `mcp_registry.c`

Refactor static registry:

```c
int mcp_registry_build_static_tools(...);
const mcp_tool_desc_t *mcp_registry_find_static(...);
```

Dynamic catalog builder ở component mới.

`tools/list`:

```text
build result
  |
  +--> append static
  +--> snapshot dynamic
  +--> append generated descriptors
```

---

# 47. Generated descriptor ownership

Builder phải rõ ownership.

Example:

```c
esp_err_t mcp_dynamic_tool_build_json(
    const mcp_tool_binding_t *binding,
    cJSON **out_tool);
```

Contract:

- success: caller owns `*out_tool`;
- failure: `*out_tool == NULL`;
- no borrowed cJSON from persistent catalog.

---

# 48. Current device command allowlist migration

Current `CONFIG_MCP_DEVICE_COMMAND_ALLOWLIST` cần migration plan.

## Option recommended

Rename future:

```text
CONFIG_MCP_DEVICE_COMMAND_HARD_ALLOWLIST
```

Semantics:

```text
empty = no hard restriction
non-empty = command must be listed
```

Dashboard exposure vẫn required.

Migration release:

- accept old config for one release;
- log deprecation;
- map old non-empty value to new hard ceiling;
- old empty deny-all semantics không được silently giữ khi dynamic exposure enabled.

---

# 49. Dashboard exposure read API merge logic

GET API phải merge:

```text
capability snapshot
+
exposure records
```

Pseudo-code:

```text
for each current capability:
    find exposure(device_id, command)

    if none:
        enabled = false
        state = disabled

    if enabled:
        state = exposure.state

then add orphaned exposure records
that have no current capability
```

Nhờ vậy UI vẫn thấy command đã biến mất.

---

# 50. Bulk transaction and fail-closed behavior

Bulk update không thể dùng rollback đơn giản cho mọi trường hợp, vì **revoke phải fail closed**.

Phải classify mutation:

```text
privilege reducing:
    ENABLED -> disabled
    ENABLED -> NEEDS_REVIEW
    ENABLED -> ORPHANED

privilege granting:
    disabled/NEEDS_REVIEW/ORPHANED -> ENABLED
```

Algorithm:

```text
1. validate complete request
2. apply all revocations to RAM catalog immediately
3. build candidate persisted catalog containing intended final state
4. persist candidate
5a. persist success:
       publish grants to RAM
       catalog_revision reflects final state
5b. persist fail:
       keep revocations effective
       do NOT publish grants
       mark store dirty / schedule retry
       return failure to Dashboard
```

Không bao giờ rollback một security downgrade thành executable grant chỉ vì flash write thất bại.

Nếu request chỉ chứa grants, persist-first rồi publish RAM đảm bảo failed save không tạo transient privilege.

---

# 51. Event integration

Exposure component cần event khi:

```text
capability committed changed
device deleted
```

Device rename không cần catalog event trong v1.1 vì model-facing tool identity/description không dùng mutable display name.

Không polling.

Preferred capability integration:

```text
mcp_tool_exposure_init()
    -> device_capabilities_register_commit_listener(listener, ctx)
```

Dependency vẫn là exposure -> capability API; capability component chỉ giữ generic callback pointer và không include MCP headers.

---

# 52. Capability commit listener contract

Public callback API đề xuất:

```c
typedef void (*device_capability_commit_listener_t)(
    const char *device_id,
    uint32_t revision,
    void *context);

esp_err_t device_capabilities_register_commit_listener(
    device_capability_commit_listener_t listener,
    void *context);
```

Bắt buộc:

- invoke listener **sau khi capability mutex đã unlock**;
- chỉ invoke sau committed snapshot đã được publish in-memory;
- listener không được chạy dưới NVS/capability lock;
- listener phải non-blocking;
- exposure listener chỉ copy `device_id/revision` vào queue;
- NVS reconcile chạy trong exposure worker.

Current `handle_end()` đã tách phần lock/unlock quanh committed snapshot; implementation phải đặt callback ở safe point ngoài lock sau commit decision. Cụ thể (v1.2): listener invoke **sau persist attempt**, trong capability worker task (context an toàn), cho mọi commit thành công (kể cả `changed == false`), và listener chỉ enqueue — không block.

## 52.1 Gap v1.2: expose `has_committed` trong public snapshot

`device_capability_snapshot_t` hiện không phân biệt "chưa từng commit" với "đã commit nhưng state kẹt DISCOVERING": `start_discovery()` set `committed.state = DISCOVERING` kể cả khi `has_committed=true`, và khi refresh fail với snapshot đã commit, state **kẹt DISCOVERING vĩnh viễn** (`handle_completion` chỉ set ERROR khi `!has_committed`).

Do đó:

- thêm `bool has_committed;` vào `device_capability_snapshot_t`, populate trong `device_capabilities_get()`;
- exposure **không được gate theo `state == READY`** (mẫu hiện có trong `mcp_policy.c` sẽ deny sai sau một lần refresh fail) — gate theo `has_committed && count >= 1`;

---

# 53. Exposure worker

Dùng một small queue/task để serialize:

```text
CAPABILITY_COMMITTED(device_id, revision)
DEVICE_REVOKE(device_id)
DIRTY_PERSIST_RETRY
```

Worker responsibilities:

- snapshot current capability bằng public copy-out API;
- compute semantic digest;
- reconcile ENABLED/NEEDS_REVIEW/ORPHANED;
- update runtime catalog revision;
- persist state theo fail-closed rules;
- coalesce duplicate reconcile events cùng device nếu queue pressure.

Không giữ capability mutex trong khi lấy exposure mutex và ngược lại. Mọi cross-component data phải copy-out trước khi lock component kế tiếp.

---

# 54. Dashboard save UX

Toggles có hai mô hình:

### Immediate save
click toggle -> API write ngay.

Ưu:
- đơn giản state.

Nhược:
- nhiều NVS writes.

### Save button
user chọn nhiều -> `Save MCP Tools`.

Ưu:
- một transaction;
- ít flash writes;
- dễ validate capacity.

Khuyến nghị:

```text
Save button / bulk update
```

cho device detail.

---

# 55. Capacity behavior

Hai capacity errors:

### Enabled dynamic tool full

```json
{
  "success": false,
  "error": {"code": "mcp_tool_capacity_exceeded"}
}
```

UI:

```text
Maximum 32 MCP device tools can be enabled.
Disable another tool first.
```

### Exposure record store full

```json
{
  "success": false,
  "error": {"code": "mcp_exposure_record_capacity_exceeded"}
}
```

UI phải hiển thị record usage và cho user remove/disable stale ORPHANED/NEEDS_REVIEW records bằng cùng PUT API.

HTTP recommended: `409 Conflict` cho cả hai policy-capacity cases; không dùng `507` trừ khi lỗi storage resource thực sự ở lower layer.

---

# 56. Duplicate command behavior

Capability snapshot validation phải đảm bảo command unique per device.

Exposure enable phải reject nếu:

- duplicate ambiguous capability;
- corrupted snapshot;
- tool-name collision unresolved.

Fail closed.

---

# 57. Device capability state handling

Enable chỉ được phép khi capability snapshot có committed command.

Nếu state:

```text
UNKNOWN
DISCOVERING
ERROR
UNSUPPORTED
```

nhưng committed persisted snapshot vẫn tồn tại thì current component semantics cần được đọc đúng.

Target policy:

- use committed persisted snapshot when valid (xem §52.1: gate theo `has_committed`, không theo `state == READY`);
- do not require BLE READY;
- if no committed snapshot, reject enable.

Admin error:

```text
capabilities_not_ready
```

---

# 58. Unsupported capability devices

Nếu device không support capability discovery:

Dashboard:

```text
MCP command exposure unavailable.
Device does not publish capabilities.
```

Không fallback tự động sang arbitrary command text.

Nếu cần manual command definition, đó là future feature riêng.

---

# 59. Tool catalog and device rename

Device display name change:

```text
tool_name unchanged
model-facing description unchanged
semantic digest unchanged
catalog revision unchanged
```

Lý do: v1.1 model-facing metadata dùng stable `device_id` + command, không dùng display name.

Dashboard friendly text update theo `device_store` độc lập với MCP catalog. Điều này loại bỏ requirement phải có device-rename event chỉ để refresh tool descriptors.

---

# 60. Capability label/unit changes

### Label change

`label` là display-only, không nằm trong semantic digest và không được đưa raw vào model-facing description.

```text
label change -> Dashboard text update only
no NEEDS_REVIEW
no MCP catalog revision change
```

### Unit change

`unit` nằm trong semantic digest vì có thể thay đổi physical meaning của integer value.

```text
unit change -> semantic digest mismatch -> NEEDS_REVIEW
```

Raw unit vẫn không được chèn trực tiếp vào arbitrary model prose ở phase 1.

---

# 61. Tool output

Dynamic device tool giữ current device command result behavior.

Success:

```json
{
  "content": [
    {
      "type": "text",
      "text": "Command completed"
    }
  ],
  "isError": false
}
```

Nếu device trả structured status có thể map theo MCP dual-era rules.

Không cần dynamic outputSchema trong phase 1 vì command capability protocol hiện chỉ mô tả input.

---

# 62. Audit/log

Project định hướng bỏ persistent logging.

Không thêm flash audit log.

Có thể:

- ESP_LOGI event config change;
- current UI state;
- optional RAM diagnostics.

Không log sensitive bearer token.

Không log full command arguments nếu sau này có sensitive values.

---

# 63. Tests - exposure store and fail-closed persistence

Bắt buộc:

```text
init empty
enable persists before publish
disable removes executable binding immediately
reload after reset
schema version mismatch
corrupt blob => no grants loaded
enabled capacity independent from record capacity
record capacity full
duplicate exposure
grant NVS failure => grant not published
revoke NVS failure => remains revoked in RAM + dirty retry
mixed bulk persist failure => revokes stay effective, grants not published
device delete revoke survives persistence failure
```

---

# 64. Tests - naming

Bắt buộc:

```text
valid device_id + command
invalid chars sanitized
empty sanitized token
length exactly 128
length >128
collision
hash suffix deterministic
disable/re-enable/reboot generates same tool_name
device display rename no effect
```

---

# 65. Tests - semantic digest

Bắt buộc:

```text
same capability -> same digest
type changes -> changes
min changes -> changes
max changes -> changes
step changes -> changes
idempotent changes -> changes
destructive changes -> changes
unit changes -> changes
label-only changes -> no digest change
padding does not affect digest
```

---

# 66. Tests - reconcile

Bắt buộc:

```text
enabled + unchanged -> enabled
enabled + changed -> needs_review
enabled + command missing -> orphaned
orphaned + command returns -> needs_review
device missing on boot -> removed/inactive
device delete -> exposure removed
disconnect -> no catalog change
reconnect -> no catalog change
```

---

# 67. Tests - catalog

Bắt buộc:

```text
production static tools = get_status + list_devices
full list_device_capabilities absent by default
generic device_command absent in production config
only ENABLED dynamic tools visible
NEEDS_REVIEW hidden
ORPHANED hidden
static + dynamic order deterministic
catalog revision increments on executable catalog change
catalog revision unchanged on disconnect
label-only change does not churn catalog
max enabled count
tool-name uniqueness
non-empty cursor rejected when pagination disabled
no nextCursor emitted
```

---

# 68. Tests - schema generation

NONE:

```text
empty object
additionalProperties false
```

BOOL:

```text
value boolean required
```

INT:

```text
value integer required
min/max/multipleOf correct
```

Annotations:

```text
idempotent mapping
destructive mapping
```

---

# 69. Tests - tools/call security

Bắt buộc:

```text
agent cannot supply device_id override
agent cannot supply command override
unknown property rejected
disabled tool rejected
NEEDS_REVIEW tool rejected
ORPHANED tool rejected
stale cached tool rejected
semantic digest changed before call rejected
device offline returns tool error
invalid integer range returns tool error
invalid step returns tool error
hard policy deny prevents dispatch
revoke persistence failure still prevents dispatch
```

---

# 70. Tests - Dashboard API and admin auth

Bắt buộc:

```text
GET without admin token rejected
GET with invalid admin token rejected
PUT without admin token rejected
PUT with MCP bearer but no admin token rejected
valid admin token accepted
invalid Origin rejected
permissive CORS not present
Cache-Control no-store present
GET merges capability + exposure
GET includes orphaned record
GET returns enabled/max_enabled + records/max_records
enable valid command
disable
enable unknown command rejected
enable no committed capabilities rejected
bulk mixed grant/revoke semantics follow fail-closed policy
enabled capacity exceeded
record capacity exceeded
destructive flag shown
destructive enable rejected when Kconfig disabled
destructive enable requires confirm_destructive when Kconfig enabled
needs_review state shown
```

Route registration test:

```text
web_server starts with dual-era MCP + exposure GET/PUT
max_uri_handlers target = 28
all expected routes registered
```

---

# 71. Tests - persistence + reboot

Scenario A — normal offline restore:

```text
1. device capability cached
2. user enables tool
3. reboot gateway
4. device stays offline
5. capability loads from NVS
6. exposure loads from NVS
7. semantic digest matches
8. tool appears in tools/list
9. tools/call returns device unavailable, not unknown tool
```

Scenario B — failed revoke persistence must not resurrect deleted device tool:

```text
1. enabled tool exists
2. device delete revokes runtime tool
3. simulate exposure NVS cleanup failure
4. device_store deletion succeeds
5. reboot
6. stale persisted exposure is found
7. boot reconcile sees device missing
8. tool MUST NOT appear in tools/list
```

---

# 72. Stress tests

Test:

```text
32 dynamic tools
100 repeated tools/list
1000 catalog lookups
rapid enable/disable cycles
concurrent tools/list + exposure update
concurrent tools/call + reconcile
```

Check:

```text
heap stable
no use-after-free
no duplicate tool
no deadlock
no stale binding execute
```

---

# 73. Manual verification

## Step 1

Connect device and get capabilities.

Verify Dashboard lists commands.

## Step 2

Enable:

```text
set_power
set_brightness
```

## Step 3

Call `tools/list`.

Verify:

```text
<device>.set_power
<device>.set_brightness
```

exists.

Verify disabled command absent.

## Step 4

Call BOOL tool:

```json
{"value": true}
```

Device receives expected command.

## Step 5

Disable tool.

Re-call stale tool name.

Must not dispatch.

## Step 6

Disconnect BLE.

Tool remains in `tools/list`.

Call returns device unavailable tool error.

## Step 7

Reconnect.

Same tool name still works.

## Step 8

Change device capability range.

Refresh capability.

Tool moves to NEEDS_REVIEW and disappears from MCP.

## Step 9

User reviews and re-enables.

Tool returns with updated schema.

---

# 74. Migration steps

## Phase 1 - component foundation

Implement:

```text
mcp_tool_exposure
NVS schema v2
deterministic naming v1
SHA-256 semantic digest
separate enabled/record capacities
catalog revision
fail-closed grant/revoke semantics
unit tests
```

No UI yet.

## Phase 2 - MCP catalog integration

Implement:

```text
production static catalog = get_status + list_devices
dynamic tools/list
dynamic schema builder
dynamic tools/call resolver
no pagination
no public generic device_command by final production config
```

Keep generic `device_command` temporarily during development only.

## Phase 3 - Dashboard admin security + API

Implement:

```text
web_admin_auth
GET /api/mcp/exposures
PUT /api/mcp/exposures single/bulk
no direct NVS write from web_server
route budget -> 28
```

## Phase 4 - Dashboard UI

Implement:

```text
admin token/session UX
per-device MCP command section
global MCP tools page
destructive warning + explicit confirm
needs-review/orphaned UX
enabled + record capacity indicators
```

## Phase 5 - lifecycle integration

Implement:

```text
capability commit listener outside capability lock
exposure reconcile worker
device delete immediate revoke
boot reconcile
offline behavior
dirty persist retry
```

## Phase 6 - remove broad MCP introspection/execution surfaces

Production target:

```text
CONFIG_MCP_KEEP_GENERIC_DEVICE_COMMAND=n
CONFIG_MCP_EXPOSE_FULL_CAPABILITY_TOOL=n
```

Run full MCP dual-era conformance + integration suite.

## Phase 7 - hardening

Benchmark:

```text
heap
tools/list size
NVS blob size/write behavior
concurrency
32 enabled tools + 96 policy records
admin auth negative tests
reboot after failed revoke persistence
```

---

# 75. Files expected to change

New:

```text
components/mcp_tool_exposure/*
components/web_server/web_admin_auth.c
components/web_server/web_admin_auth.h
```

Modify:

```text
components/mcp_endpoint/mcp_registry.c
components/mcp_endpoint/mcp_tools.c
components/mcp_endpoint/mcp_endpoint.c           // register error handling (v1.2)
components/mcp_endpoint/mcp_endpoint_internal.h  // TTL define -> Kconfig (v1.2)
components/mcp_endpoint/Kconfig.projbuild
components/mcp_endpoint/CMakeLists.txt           // REQUIRES mcp_tool_exposure
components/mcp_endpoint/test/*

components/web_server/web_gateway_api.c
components/web_server/web_server.c
components/web_server/CMakeLists.txt             // REQUIRES mcp_tool_exposure
components/web_server/Kconfig.projbuild        // hoặc Kconfig owner tương ứng
components/web_server/README.md
components/web_server/test/*
components/web_server/www/*

components/device_capabilities/include/device_capabilities.h   // has_committed + commit listener (v1.2)
components/device_capabilities/device_capabilities.c
components/device_capabilities/test/*

components/command_dispatcher/gateway_commands.c
components/command_dispatcher/CMakeLists.txt     // REQUIRES mcp_tool_exposure
components/command_dispatcher/test/*           // device-delete revoke integration

main/main.c                                      // mcp_tool_exposure_init() init order (v1.2)
main/CMakeLists.txt                              // REQUIRES mcp_tool_exposure

test/CMakeLists.txt                              // TEST_COMPONENTS thêm mcp_tool_exposure (v1.2)
```

Không cần sửa `components/cbor_codec/*` chỉ để cấu hình exposure. `gw_message_t` không nhận MCP-specific admin fields.

---

# 76. Code ownership

`device_capabilities` owns:

```text
what device can do
capability persistence
capability validation
generic commit-listener callback point
```

`mcp_tool_exposure` owns:

```text
what user allows AI to do
persistent exposure records
stable naming algorithm
semantic digest
runtime dynamic catalog
reconciliation
fail-closed grant/revoke state
```

`mcp_endpoint` owns:

```text
MCP transport
tools/list wire format
tools/call wire format
static + dynamic catalog presentation
```

`web_admin_auth` owns:

```text
admin bearer validation
Host/Origin policy for admin routes
constant-time credential comparison
```

`web_server` owns:

```text
Dashboard assets
HTTP exposure API
input parsing
response formatting
calling typed exposure service after auth
```

`command_dispatcher/gateway_commands` owns:

```text
device execution routing
device-delete lifecycle hook that revokes MCP exposure
```

`gw_message_t` / CBOR protocol does **not** own exposure policy.

---

# 77. Failure policy summary

| Failure | Behavior |
|---|---|
| Exposure NVS load corrupt | fail closed; no dynamic grants loaded |
| Grant persist fails | do not publish ENABLED in RAM |
| Revoke/downgrade persist fails | keep revoked/hidden in RAM; mark dirty; retry |
| Capability missing | cannot enable |
| Semantic digest changed | immediate NEEDS_REVIEW |
| Command disappeared | immediate ORPHANED |
| Device disconnected | tool stays visible; call returns tool error if unavailable |
| Device deleted | immediate runtime revoke; persisted cleanup best-effort |
| Agent uses stale disabled tool | reject; never dispatch |
| Tool-name collision after deterministic hash | fail closed; report collision |
| Enabled capacity full | reject grant |
| Record capacity full | reject new record/grant; allow revocation cleanup |
| Hard policy denies | reject enable/call |
| Admin auth missing/invalid | exposure GET/PUT denied |
| Destructive enable lacks explicit confirm | reject grant |
| BLE execution fails | MCP tool execution error (`isError:true`) |

---

# 78. Definition of Done

Feature chỉ được coi là hoàn thành khi:

- [ ] User có thể xem capability commands trong Dashboard sau admin authentication.
- [ ] User có thể enable/disable từng command cho MCP.
- [ ] MCP bearer token không đủ để mutate Dashboard exposure API.
- [ ] Admin token chưa cấu hình => exposure mutation fail closed.
- [ ] Exposure policy persist qua reboot.
- [ ] Naming algorithm v1 deterministic và stable qua disable/re-enable/reboot.
- [ ] Hai device ID canonicalize cùng slug vẫn tạo deterministic unique names.
- [ ] Enabled tool capacity và exposure record capacity được enforce độc lập.
- [ ] Production `tools/list` chỉ chứa `get_status`, `list_devices`, và approved dynamic tools.
- [ ] Production không expose full `list_device_capabilities`.
- [ ] Production không expose generic `device_command`.
- [ ] Agent không thể override `device_id` hoặc `command`.
- [ ] Input schema được sinh đúng từ capability.
- [ ] INT min/max/step và BOOL type được enforce runtime.
- [ ] SHA-256 truncated 128-bit semantic digest được dùng; không hash raw struct.
- [ ] Destructive command mặc định hard-disabled.
- [ ] Destructive enable yêu cầu admin auth + firmware allow + explicit confirmation.
- [ ] Destructive/semantic/unit change trigger immediate NEEDS_REVIEW.
- [ ] Label-only change không trigger review.
- [ ] Command removed trigger ORPHANED.
- [ ] Device disconnect không remove tool.
- [ ] Device delete revoke tất cả tool ngay cả khi exposure NVS save fail.
- [ ] Reboot sau failed delete/revoke persistence vẫn không resurrect tool của device đã xóa.
- [ ] Stale cached tool không thể dispatch sau khi disabled/revoked.
- [ ] Capability commit listener chạy ngoài capability mutex và chỉ enqueue reconcile.
- [ ] Không có cross-component nested lock.
- [ ] Catalog order deterministic.
- [ ] Catalog revision hoạt động đúng cho executable descriptor changes.
- [ ] MCP 2025 không quảng bá listChanged nếu không deliver được.
- [ ] MCP 2026 tools/list có cache hints đúng.
- [ ] Phase 1 không emit `nextCursor`; invalid cursor được xử lý rõ.
- [ ] Gateway web route budget đủ cho dual-era MCP + exposure GET/PUT (`max_uri_handlers=28` target).
- [ ] Full MCP dual-era tests vẫn pass.
- [ ] Heap/stress test 32 enabled tools + 96 policy records pass.
- [ ] Không có NVS write per tool call.
- [ ] Không có direct exposure NVS write từ web_server.
- [ ] Peripheral label/display text không được đưa raw thành model-facing instruction.
- [ ] README và architecture docs được cập nhật.

---

# 79. Recommended final architecture

```text
                         BLE Device
                             |
                     describe_capabilities
                             |
                             v
                   device_capabilities
                    persisted snapshot
                             |
                    commit listener
                   (outside cap lock)
                             |
                             v
                  exposure worker queue
                             |
                             v
                   mcp_tool_exposure
                 policy + digest + catalog
                      |             ^
                      |             |
         Dashboard    |             | tools/list
             |        |             |
             v        |             |
       web_admin_auth |             |
             |        |             |
             +------> admin service |
                      |             |
                      v             |
                  NVS policy        |
                                    |
                               AI Agent
                                    |
                                tools/call
                                    |
                                    v
                          dynamic binding resolver
                           fixed device_id + command
                                    |
                                    v
                              command_executor
                                    |
                                    v
                           command_dispatcher
                                    |
                                    v
                               BLE Device

Device delete:
command_dispatcher -> immediate exposure revoke -> remaining delete lifecycle
```

Authorization boundary:

```text
Capability Truth
      AND
User Persisted Grant
      AND
Semantic Digest Match
      AND
Firmware Hard Policy
      =
Executable Dynamic MCP Tool
```

---

# 80. Recommended implementation decision summary

Baseline v1.1:

```text
Dynamic tool per selected command.
No public generic device_id + command arguments.
Exposure policy separate from device capability.
Admin exposure API uses separate admin credential, not MCP token.
Exposure admin mutation uses typed service; no MCP fields in gw_message_t.
Persist exposure in NVS schema v2.
Disabled == no persisted grant record.
Naming algorithm v1 is deterministic and immutable.
Sanitized tool names always carry SHA-derived suffix.
Semantic digest = SHA-256 truncated 128-bit over canonical fields.
Grant = persist first, publish second.
Revoke = hide first, persist second; never rollback privilege.
Production static MCP tools = get_status + list_devices.
Full list_device_capabilities is Dashboard/admin-only by default.
Default enabled dynamic limit = 32.
Default exposure record limit = 96.
MCP 2026 cache TTL default = 10 seconds.
No pagination/list-change streaming in phase 1.
Destructive dynamic tools hard-disabled by default.
Peripheral label/display text is not raw model-facing prose.
Generic device_command is migration-only and disabled in production.
```

---

# 81. Sources and implementation basis

Project basis checked for v1.1:

- `components/device_capabilities/include/device_capabilities.h`
- `components/device_capabilities/device_capabilities.c`
- `components/device_store/include/device_store.h`
- `components/device_store/device_store.c`
- `components/cbor_codec/include/cbor_codec.h`
- `components/mcp_endpoint/mcp_registry.c`
- `components/mcp_endpoint/mcp_tools.c`
- `components/web_server/web_gateway_api.c`
- `components/web_server/web_server.c`
- `components/web_server/README.md`
- `components/command_dispatcher/gateway_commands.c`
- MCP dual-era update plan v1.1

Current code constraints reflected in this revision:

- `DEVICE_STORE_MAX_DEVICES = 16`;
- `DEVICE_CAP_MAX_PER_DEVICE = 12`;
- `gw_message_t` is shared CBOR/gateway protocol state and should not receive MCP exposure-policy fields;
- capability snapshots are persisted and can remain valid while BLE is offline;
- current web server route budget is 21 with 19 handlers accounted before dual-era GET/DELETE and exposure routes;
- current Dashboard REST has no general admin authentication layer, so exposure mutation needs an explicit new boundary.

MCP behavior inherited from dual-era v1.1:

- 2026 stateless request model;
- 2025 compatibility lifecycle;
- `tools/list` cache hints for modern era;
- no list-change streaming/subscriptions in this phase;
- tool execution errors remain `CallToolResult isError:true`;
- protocol errors remain separate from device/application outcomes.

---

# 82. Final recommendation

Không phát triển tính năng này bằng cách cho Dashboard sửa registry hoặc cho peripheral tự quảng bá thành executable MCP tool.

Target v1.1:

```text
Capability Truth
      +
User-controlled Admin Grant
      +
Current Semantic Digest Match
      +
Firmware Hard Policy
      =
Dynamic MCP Catalog
```

Ba guardrail quan trọng nhất của v1.1:

1. **Tool identity deterministic:** disable/re-enable, reboot hoặc collision context không được làm tên tool thay đổi.
2. **Authorization fail closed:** mọi revoke/downgrade phải có hiệu lực trong RAM ngay cả khi NVS lỗi; storage failure không được resurrect privilege.
3. **Dashboard exposure là security control plane:** mutation phải có admin credential riêng và không reuse MCP bearer token.

Sau khi các guardrail này được triển khai cùng dual-era MCP v1.1, Agent sẽ chỉ thấy các hành động mà user thực sự approve, với schema được sinh từ capability thật và binding cố định tới đúng device/command.

Tài liệu v1.1 này là baseline để giao cho dev/AI agent triển khai.
