# ESP32 BLE Gateway — Device Detail UI Redesign Implementation Plan

**Project:** `esp-ble-gateway`  
**Repository:** `https://github.com/hailp-vn38/esp-ble-gateway`  
**Target view:** `Device Detail`  
**Primary frontend files:** `components/web_server/www_src/dashboard/...`  
**Document status:** Implementation specification  
**Version:** 1.0

---

# 1. Mục tiêu

Thiết kế lại toàn bộ Device Detail view để:

- ưu tiên trạng thái thiết bị và khả năng điều khiển;
- tách manual controls khỏi MCP exposure;
- giảm thông tin lặp;
- đưa destructive actions xuống cuối trang;
- làm rõ capability lifecycle;
- cải thiện responsive layout;
- đưa toàn bộ Device Detail vào i18n;
- giữ UI dễ mở rộng khi device có nhiều capability.

---

# 2. Vấn đề của UI hiện tại

Device Detail hiện có:

```text
Header
    Edit
    Remove

Left:
    Device information

Right:
    Device Commands
        Capability state
        MCP tools
        Manual controls
        Legacy command
```

Các vấn đề:

1. Manual control và MCP configuration bị trộn chung.
2. `MCP tools` nằm bên trong `Device Commands`, sai hierarchy.
3. `Remove` nằm cạnh `Edit` dù là destructive action.
4. Device Information chiếm 1/3 layout dù chỉ là metadata.
5. `Capability state: ready` nằm quá sâu và khó nhìn.
6. Command controls dùng grid 2 cột mặc định, dễ chật.
7. MCP exposure dùng chips, không rõ ON/OFF semantics.
8. Legacy custom command hiện quá gần normal flow.
9. UI chưa có i18n đầy đủ.
10. Device ID và BLE address có thể bị lặp.

---

# 3. UX hierarchy mới

Device Detail nên theo thứ tự:

```text
IDENTITY
    ↓
STATUS
    ↓
MANUAL CONTROLS
    ↓
MCP EXPOSURE
    ↓
METADATA
    ↓
ADVANCED
    ↓
DEVICE MANAGEMENT
```

---

# 4. Layout tổng thể mới

```text
← Thiết bị

┌──────────────────────────────────────────────────────────────┐
│ [BT] TEST                                    ● Đang kết nối │
│      Generic · AC:27:6E:CC:F2:26                            │
│                                              [ Chỉnh sửa ]   │
└──────────────────────────────────────────────────────────────┘

┌────────────────┐ ┌────────────────┐ ┌────────────────┐
│ Kết nối        │ │ Loại thiết bị  │ │ Capabilities   │
│ ● Online       │ │ Generic        │ │ ● Sẵn sàng     │
└────────────────┘ └────────────────┘ └────────────────┘

┌──────────────────────────────────────────────────────────────┐
│ Điều khiển thiết bị                         [ ↻ Làm mới ]   │
│ Các chức năng được thiết bị công bố                          │
├──────────────────────────────────────────────────────────────┤
│ LED state                                                    │
│ Đọc trạng thái LED                              [ Thực thi ] │
│                                                              │
│ LED power                                                    │
│ Điều khiển nguồn LED                           [ Bật ] [Tắt] │
└──────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────┐
│ MCP Tools                                      2 / 32 tools │
│ Chọn command được cung cấp cho MCP clients                   │
├──────────────────────────────────────────────────────────────┤
│ LED state       test_led_state                   [ ON ]      │
│ LED power       test_led_power                   [ ON ]      │
└──────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────┐
│ Thông tin thiết bị                                           │
│ Device ID        AC:27:6E:CC:F2:26                           │
│ BLE address      AC:27:6E:CC:F2:26                           │
└──────────────────────────────────────────────────────────────┘

▸ Công cụ nâng cao

┌──────────────────────────────────────────────────────────────┐
│ Quản lý thiết bị                                             │
│ Xóa thiết bị khỏi Gateway                    [ Xóa thiết bị ]│
└──────────────────────────────────────────────────────────────┘
```

---

# 5. Header redesign

Giữ:

```text
Back to Devices
Device icon
Device name
Type
Identifier
Status
Edit
```

Loại khỏi header:

```text
Remove
```

Header:

```text
[BT] TEST                               ● Đã kết nối
     Generic
     AC:27:6E:CC:F2:26

                                      [ Chỉnh sửa ]
```

---

# 6. Device status badge

Mapping:

```text
online
    → green
    → Đã kết nối

offline
    → gray
    → Ngoại tuyến
```

Không chỉ dùng màu.

Luôn có text.

---

# 7. Summary cards

Tạo 3 compact cards:

```text
Connection
Device Type
Capabilities
```

Desktop:

```text
grid-cols-3
```

Tablet/mobile:

```text
responsive wrap
```

---

# 8. Connection card

Ví dụ:

```text
Kết nối

● Online
```

Nếu offline:

```text
Kết nối

● Ngoại tuyến
```

---

# 9. Device Type card

```text
Loại thiết bị

Generic
```

Không cần lặp device type trong nhiều vị trí.

Header có thể vẫn giữ type như secondary metadata.

---

# 10. Capabilities card

Mapping:

```text
ready
    → green
    → Sẵn sàng

loading / discovering
    → blue
    → Đang đọc

stale
    → amber
    → Cần làm mới

error
    → red
    → Không khả dụng

unknown
    → gray
    → Chưa xác định
```

Không hiển thị lại một badge:

```text
Capability state: ready
```

bên trong controls.

---

# 11. Device Controls phải là card chính

Title:

```text
Điều khiển thiết bị
```

Subtitle:

```text
Các chức năng được thiết bị công bố.
```

Action:

```text
↻ Làm mới
```

---

# 12. Refresh capabilities behavior

Khi click:

```text
Refresh
    ↓
queue capability refresh
    ↓
show busy
    ↓
reload capabilities
```

UI:

```text
[ ↻ Đang làm mới... ]
```

disable trong lúc request.

Sau thành công:

```text
Capabilities đã được cập nhật
```

---

# 13. Command rendering principles

Không dùng default:

```text
grid-cols-2
```

cho toàn bộ controls.

Khuyến nghị:

```text
1-column list
```

Mỗi capability là một row/card.

---

# 14. Action command

Ví dụ:

```text
LED state
Đọc/truy vấn trạng thái LED

                                  [ Thực thi ]
```

Không dùng full-width button nếu không cần.

---

# 15. Boolean command

Ví dụ:

```text
LED power
Điều khiển nguồn LED

                               [ Bật ] [ Tắt ]
```

Visual:

```text
Bật
    → primary/brand

Tắt
    → neutral / outline
```

Trừ khi command destructive.

---

# 16. Integer command

Ví dụ:

```text
Brightness
Điều chỉnh độ sáng

[ 50             ] %        [ Áp dụng ]

Range: 0–100
```

Hiển thị:

```text
min
max
step
unit
```

nếu capability có.

---

# 17. Destructive command

Nếu:

```text
capability.destructive == true
```

render:

```text
Factory reset                    ⚠ Nguy hiểm

Xóa toàn bộ cấu hình của thiết bị.

                                  [ Thực thi... ]
```

Button:

```text
red outline / destructive
```

Confirm:

```text
Run factory_reset?
```

Nên dùng label thân thiện nếu có.

---

# 18. Offline behavior

Nếu device offline:

Manual controls:

```text
disabled
```

Hiển thị:

```text
Thiết bị hiện đang ngoại tuyến.

Các command không thể thực thi cho tới khi thiết bị kết nối lại.
```

MCP exposure có thể vẫn editable nếu backend cho phép vì đó là persisted configuration.

---

# 19. Empty capabilities state

Nếu không có commands:

```text
Không tìm thấy capability

Gateway chưa nhận được danh sách command từ thiết bị.

[ Làm mới capabilities ]
```

---

# 20. Capability error state

Nếu API fail:

```text
Không thể tải capabilities

<error message>

[ Thử lại ]
```

Legacy command không nên tự động biến thành main UI ngay lập tức.

---

# 21. MCP Tools phải là card riêng

Không đặt trong Device Controls.

Card:

```text
MCP Tools                                  2 / 32

Chọn command được cung cấp cho MCP clients.
```

---

# 22. MCP exposure rows

Không dùng chips.

Mỗi command:

```text
LED state

test_led_state

Cho phép MCP client gọi command này.

                                          [ ON ]
```

---

# 23. MCP switch semantics

```text
ON
    → command exposed

OFF
    → not exposed
```

Switch rõ hơn chip màu xanh.

---

# 24. MCP capacity

Current:

```text
2 / 32 tool slots
```

New:

```text
2 / 32 tools đang sử dụng
```

Color:

```text
normal
    → gray

near full
    → amber

full
    → red
```

Suggested:

```text
>= 80%
    → amber

100%
    → red
```

---

# 25. MCP destructive command

Nếu destructive:

```text
Factory reset                           ⚠ Nguy hiểm
factory_reset

Cho phép MCP client gọi command này.

                                          [ OFF ]
```

Nếu user bật:

```text
confirm
```

Giữ backend requirement:

```text
confirm_destructive=true
```

---

# 26. MCP exposure loading state

Per-row hoặc toàn card:

```text
Đang tải MCP tools...
```

Khi toggle:

```text
disable only that row/switch
```

Không block toàn Device Detail.

---

# 27. MCP error state

Nếu load exposure fail:

```text
Không thể tải MCP tools

<error>

Kiểm tra MCP admin token trong Gateway Settings.
```

Không render raw chip error style.

---

# 28. Device Information xuống dưới

Card metadata:

```text
Thông tin thiết bị

Device ID
AC:27:6E:CC:F2:26

BLE address
AC:27:6E:CC:F2:26
```

---

# 29. Duplicate Device ID / BLE address

Hiện JS dùng:

```text
dev.id
dev.mac
```

nên về contract chúng có thể khác.

Do đó không tự động merge chỉ dựa trên một screenshot.

Optional frontend behavior:

```text
if dev.id === dev.mac:
    show one Identifier row
else:
    show Device ID + BLE address
```

Đây là lựa chọn UI tốt hơn.

---

# 30. Advanced section

Legacy custom command nên chuyển thành collapsible:

```text
▸ Công cụ nâng cao
```

Expand:

```text
Gửi command thủ công

Command
[ toggle ]

Value type
[ none / boolean / integer ]

Value
[ ... ]

[ Gửi command ]
```

---

# 31. Advanced warning

Hiển thị:

```text
Chỉ dùng cho debug hoặc thiết bị chưa hỗ trợ capability discovery.
```

Normal user không cần thấy section này mở sẵn.

---

# 32. Device Management xuống cuối

Không để Remove ở header.

Section:

```text
Quản lý thiết bị

Xóa thiết bị khỏi Gateway

Gateway sẽ xóa cấu hình thiết bị và ngừng tự động kết nối lại.

                                      [ Xóa thiết bị ]
```

---

# 33. Remove confirmation

Confirmation nên có device name:

```text
Xóa "TEST" khỏi Gateway?
```

Nếu device có MCP exposure:

```text
Các MCP tools của thiết bị này cũng sẽ không còn khả dụng.
```

---

# 34. Header Edit action

`Edit` vẫn ở header vì là action thường xuyên và không destructive.

Edit modal có thể giữ flow hiện tại:

```text
name
type
save
```

Chỉ cần đưa text vào i18n.

---

# 35. Responsive desktop

```text
Header

Summary 3 columns

Device Controls
full-width

MCP Tools
full-width

Device Information

Advanced

Device Management
```

---

# 36. Responsive tablet

```text
Summary:
    2 + wrap

All main cards:
    full width
```

---

# 37. Responsive mobile

```text
All sections:
    1 column
```

Controls:

```text
boolean buttons
    stack or 2 columns depending width

integer input + button
    stack if needed
```

MCP rows:

```text
label
tool name
switch on next row if width small
```

---

# 38. Why vertical flow is preferred

Device capability count is dynamic:

```text
2
10
20+
```

Current fixed:

```text
1/3 info
2/3 commands
```

scales poorly.

Vertical flow scales better:

```text
identity
↓
status
↓
controls
↓
MCP
↓
metadata
```

---

# 39. HTML changes

Primary:

```text
components/web_server/www_src/dashboard/views/device_detail.html
```

Recommended new sections:

```text
detail-header

detail-summary

device-controls-card

mcp-tools-card

device-info-card

device-advanced-section

device-management-card
```

---

# 40. JS refactor — devices.js

Current `loadCapabilities()` does too much.

Split into:

```text
loadCapabilities()

renderCapabilityState()

renderCapabilities()

renderCapabilityControl()

renderBooleanControl()

renderIntegerControl()

renderActionControl()

renderCapabilityEmptyState()

renderCapabilityError()
```

---

# 41. Command row renderer

Recommended abstraction:

```text
createCapabilityRow(capability)
```

Then attach control-specific content.

This keeps consistent:

```text
label
description
destructive badge
control
```

---

# 42. Busy state

During command execution:

Disable only relevant control.

Example:

```text
LED power
[ Bật ] [ Tắt ]
```

Click Bật:

```text
[ Đang gửi... ] [ Tắt disabled ]
```

or disable both until completion.

Do not disable unrelated commands.

---

# 43. Command result feedback

Keep toast:

```text
success
error
```

Optional future:

Show last command result inline.

Not required for first redesign.

---

# 44. JS refactor — mcp_exposure.js

Replace:

```text
renderChip()
```

with:

```text
renderExposureRow()
```

Row state:

```text
enabled
disabled
busy
destructive
```

---

# 45. MCP exposure row structure

```text
name
tool_name
description
destructive badge
switch
```

Avoid relying on:

```text
chip selected color
```

as sole state indicator.

---

# 46. i18n

Device Detail currently contains hard-coded English labels.

Add full EN/VI translation coverage.

Suggested namespace:

```text
device_detail.*
```

---

# 47. i18n keys

Suggested:

```text
device_detail.back
device_detail.edit

device_detail.online
device_detail.offline

device_detail.connection
device_detail.device_type
device_detail.capabilities

device_detail.capability_ready
device_detail.capability_loading
device_detail.capability_stale
device_detail.capability_error
device_detail.capability_unknown

device_detail.controls
device_detail.controls_desc
device_detail.refresh
device_detail.refreshing

device_detail.run
device_detail.on
device_detail.off
device_detail.apply

device_detail.mcp_tools
device_detail.mcp_tools_desc
device_detail.mcp_enabled
device_detail.mcp_disabled
device_detail.mcp_capacity
device_detail.destructive

device_detail.info
device_detail.device_id
device_detail.ble_address
device_detail.identifier

device_detail.advanced
device_detail.custom_command
device_detail.custom_command_warning
device_detail.send

device_detail.management
device_detail.remove
device_detail.remove_confirm
```

---

# 48. Vietnamese wording

Recommended:

```text
Device Controls
    → Điều khiển thiết bị

Capabilities
    → Capabilities

MCP Tools
    → MCP Tools

Refresh
    → Làm mới

Run
    → Thực thi

Device Management
    → Quản lý thiết bị

Remove device
    → Xóa thiết bị
```

Có thể giữ `Capabilities` và `MCP Tools` dạng technical term.

---

# 49. English wording

Recommended:

```text
Device Controls
Available functions reported by this device

MCP Tools
Choose commands exposed to MCP clients

Device Information
Identity and BLE metadata

Device Management
Remove this device from the gateway
```

---

# 50. Security / safety

For destructive device commands:

```text
manual execution
    → confirm

MCP exposure
    → confirm before enabling
```

Do not remove existing confirmation behavior.

---

# 51. Accessibility

Status:

```text
color + text
```

Switches:

```text
aria-checked
label
```

Buttons:

```text
visible text
```

Capability controls must be keyboard accessible.

---

# 52. Loading states

When opening detail:

```text
summary
    → immediate from selected device

capabilities
    → loading skeleton / state

MCP exposures
    → loading state
```

Avoid flashing legacy custom command before API response.

---

# 53. Device offline scenario

Expected:

```text
header:
    Offline

summary:
    Connection = Offline

controls:
    disabled

MCP:
    available if backend supports persisted config

refresh capabilities:
    may be disabled or show expected error
```

---

# 54. Device online + ready scenario

Expected:

```text
green Online
Capabilities ready

manual controls active

MCP rows active
```

---

# 55. Device capability refresh scenario

Expected:

```text
Refresh clicked

button busy

capability status:
    Loading / Discovering

new controls rendered

MCP exposure list reloaded if command registry changed
```

Important:

After capability refresh, MCP exposure UI should reload because available commands may have changed.

---

# 56. Capability state change and MCP sync

Recommended frontend sequence:

```text
refreshCapabilities()
    ↓
wait/reload capabilities
    ↓
render new commands
    ↓
mcpTools.loadDevice(device.id)
```

Do not leave old MCP command list after capabilities change.

---

# 57. MCP exposure + Xiaozhi relationship

Changing MCP exposure changes available MCP tools.

Device Detail should not directly reconnect Xiaozhi automatically in this UI redesign unless product behavior explicitly requires it.

Recommended:

```text
exposure change
    → save successfully
    → toast:
       "MCP tools updated"
```

If Xiaozhi requires reconnect for rediscovery, optional future UX:

```text
"MCP tools changed. Reconnect Xiaozhi from Settings to refresh."
```

Keep cross-view coupling minimal.

---

# 58. Generated file rule

Do not edit:

```text
components/web_server/www/dashboard.html
```

directly.

Edit only:

```text
www_src
```

then rebuild dashboard.

---

# 59. Files to modify

Primary HTML:

```text
components/web_server/www_src/dashboard/views/device_detail.html
```

Primary device JS:

```text
components/web_server/www_src/dashboard/js/features/devices.js
```

MCP exposure JS:

```text
components/web_server/www_src/dashboard/js/features/mcp_exposure.js
```

Translations:

```text
components/web_server/www_src/dashboard/js/core/i18n.js
```

Optional UI helper:

```text
components/web_server/www_src/dashboard/js/core/ui.js
```

only if shared renderer/helper is necessary.

---

# 60. Phase 0 — Structure

- [ ] Remove old 1/3 + 2/3 layout.
- [ ] Create vertical hierarchy.
- [ ] Move Remove action to bottom.
- [ ] Define summary cards.
- [ ] Separate Device Controls and MCP Tools.

Acceptance:

```text
new layout approved
```

---

# 61. Phase 1 — Header + Summary

- [ ] New header.
- [ ] Status badge.
- [ ] Edit action.
- [ ] Connection card.
- [ ] Device Type card.
- [ ] Capability card.

Acceptance:

```text
device state readable at a glance
```

---

# 62. Phase 2 — Device Controls

- [ ] Full-width controls card.
- [ ] Refresh behavior.
- [ ] 1-column command list.
- [ ] Boolean renderer.
- [ ] Integer renderer.
- [ ] Action renderer.
- [ ] Destructive renderer.
- [ ] Offline disabled state.
- [ ] Empty/error states.

Acceptance:

```text
manual controls are clear and independent
```

---

# 63. Phase 3 — MCP Tools

- [ ] Separate card.
- [ ] Replace chips with rows/switches.
- [ ] Capacity indicator.
- [ ] Destructive warning.
- [ ] Busy state.
- [ ] Error state.
- [ ] Reload after capability refresh.

Acceptance:

```text
MCP exposure state is explicit
```

---

# 64. Phase 4 — Metadata + Advanced

- [ ] Device info card.
- [ ] Optional duplicate ID collapse.
- [ ] Advanced collapsible section.
- [ ] Move custom command inside Advanced.

Acceptance:

```text
debug controls no longer dominate normal UX
```

---

# 65. Phase 5 — Device Management

- [ ] Full-width management section.
- [ ] Remove button.
- [ ] Device-name confirmation.
- [ ] MCP exposure warning if relevant.

Acceptance:

```text
destructive action separated from normal controls
```

---

# 66. Phase 6 — JS refactor

- [ ] Split capability render methods.
- [ ] Per-command busy states.
- [ ] Clean error handling.
- [ ] Avoid duplicate API rendering.
- [ ] MCP reload after capability refresh.

Acceptance:

```text
devices.js no longer has one oversized capability renderer
```

---

# 67. Phase 7 — i18n

- [ ] English keys.
- [ ] Vietnamese keys.
- [ ] Header.
- [ ] Status.
- [ ] Controls.
- [ ] MCP.
- [ ] Management.
- [ ] Confirm dialogs.

Acceptance:

```text
no hard-coded English visible in Device Detail
```

---

# 68. Phase 8 — Responsive QA

Test:

```text
1440
1280
1024
768
390
375
```

Check:

- [ ] no overflow;
- [ ] long device name;
- [ ] long tool name;
- [ ] long command label;
- [ ] MAC/ID wrapping;
- [ ] switches usable;
- [ ] command controls usable on mobile.

---

# 69. Functional regression

## Header

- [ ] open detail.
- [ ] online/offline state.
- [ ] edit.
- [ ] back navigation.

## Controls

- [ ] action command.
- [ ] boolean command.
- [ ] integer command.
- [ ] destructive command.
- [ ] command error.
- [ ] device offline.

## Capabilities

- [ ] loading.
- [ ] ready.
- [ ] empty.
- [ ] error.
- [ ] refresh.

## MCP

- [ ] exposure load.
- [ ] enable.
- [ ] disable.
- [ ] destructive confirm.
- [ ] capacity full.
- [ ] API error.

## Advanced

- [ ] collapsed by default.
- [ ] custom command send.

## Management

- [ ] remove confirmation.
- [ ] remove success.
- [ ] navigation after remove.

---

# 70. Definition of Done

Device Detail redesign hoàn tất khi:

- [ ] Header chỉ chứa normal actions.
- [ ] Remove không còn nằm cạnh Edit.
- [ ] Summary cards hiển thị connection/type/capability state.
- [ ] Device Controls là section độc lập.
- [ ] MCP Tools là section độc lập.
- [ ] Capability controls dùng vertical list.
- [ ] Boolean/integer/action controls có UI phù hợp.
- [ ] Destructive commands hiển thị warning rõ.
- [ ] MCP exposure dùng switches/rows thay chips.
- [ ] MCP capacity rõ ràng.
- [ ] Device Information chuyển xuống dưới.
- [ ] Legacy command nằm trong Advanced.
- [ ] Device Management nằm cuối trang.
- [ ] Offline behavior rõ ràng.
- [ ] Capability refresh reload MCP exposure.
- [ ] EN/VI i18n hoàn chỉnh.
- [ ] Desktop/tablet/mobile ổn định.
- [ ] Generated dashboard không bị sửa trực tiếp.
- [ ] Existing backend APIs tiếp tục tương thích.

---

# 71. Final target structure

```text
DEVICE DETAIL

┌─ Header ───────────────────────────────────────────┐
│ identity + online state + edit                    │
└────────────────────────────────────────────────────┘

┌─ Summary ──────────────────────────────────────────┐
│ Connection     Device Type     Capabilities       │
└────────────────────────────────────────────────────┘

┌─ Device Controls ─────────────────────────────────┐
│ manual execution of device capabilities           │
└────────────────────────────────────────────────────┘

┌─ MCP Tools ───────────────────────────────────────┐
│ exposure configuration                            │
└────────────────────────────────────────────────────┘

┌─ Device Information ──────────────────────────────┐
│ IDs / BLE metadata                                │
└────────────────────────────────────────────────────┘

▸ Advanced Tools

┌─ Device Management ───────────────────────────────┐
│ destructive removal                              │
└────────────────────────────────────────────────────┘
```

Mục tiêu cuối cùng là user có thể trả lời ngay:

```text
Thiết bị có online không?

Gateway đã đọc capability chưa?

Tôi điều khiển device ở đâu?

Command nào đang được expose qua MCP?

Thông tin định danh ở đâu?

Xóa device ở đâu?
```

mà không bị trộn manual control, MCP configuration và destructive actions trong cùng một khu vực.
