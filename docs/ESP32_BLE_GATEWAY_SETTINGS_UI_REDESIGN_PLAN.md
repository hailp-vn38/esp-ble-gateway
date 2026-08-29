# ESP32 BLE Gateway — Settings UI Redesign Implementation Plan

**Project:** `esp-ble-gateway`  
**Repository:** `https://github.com/hailp-vn38/esp-ble-gateway`  
**Target view:** `Settings`  
**Primary frontend files:** `components/web_server/www_src/dashboard/...`  
**Document status:** Implementation specification  
**Version:** 1.0

---

# 1. Mục tiêu

Thiết kế lại toàn bộ Settings view để:

- giảm mật độ thông tin;
- tách rõ read-only status, configuration và destructive actions;
- làm rõ lifecycle của Xiaozhi;
- tránh hiển thị WebSocket endpoint input khi endpoint đã được cấu hình;
- không trả secret endpoint đầy đủ về browser;
- cải thiện responsive layout;
- giữ UI dễ bảo trì, phù hợp kiến trúc modular hiện tại của Web UI.

---

# 2. Vấn đề của UI hiện tại

Settings hiện sử dụng layout 2 cột:

```text
Column 1:
    System Information
    Danger Zone

Column 2:
    Network Status
    MCP Access
    Xiaozhi
```

Các vấn đề:

1. Xiaozhi có quá nhiều thông tin nhưng bị đặt trong cột 50%.
2. Trạng thái, cấu hình và action bị trộn trong cùng một block.
3. Saved endpoint và endpoint input cùng hiển thị.
4. User có cảm giác phải nhập endpoint lại dù endpoint đã lưu.
5. `Save`, `Reconnect`, `Clear` đang nằm cùng một action row dù semantics khác nhau.
6. Danger Zone nằm quá cao trong trang.
7. System Information dùng một card lớn cho chỉ vài metric.
8. Visual hierarchy chưa rõ.
9. Mobile/tablet dễ bị dồn nội dung và vỡ dòng.

---

# 3. Nguyên tắc UX mới

Settings phải đi theo thứ tự:

```text
STATUS
    ↓
ACCESS / CONFIGURATION
    ↓
SERVICE-SPECIFIC SETTINGS
    ↓
SYSTEM ACTIONS
```

Không dùng cùng một button cho nhiều nghĩa.

Mỗi action phải có semantics rõ ràng:

```text
toggle
    → enable / disable

Thay đổi
    → thay endpoint

Kết nối lại
    → reconnect runtime session

Xóa cấu hình
    → destructive remove configuration

Khởi động lại
    → apply reboot-required changes
```

---

# 4. Layout tổng thể mới

Desktop:

```text
┌──────────────────────────────────────────────────────────────┐
│ Cài đặt Gateway                                [Tiếng Việt ▼]│
│ Quản lý kết nối, quyền truy cập và cấu hình hệ thống         │
└──────────────────────────────────────────────────────────────┘


┌────────────────┐ ┌────────────────┐ ┌────────────────┐
│ Firmware       │ │ Thời gian chạy │ │ Bộ nhớ trống   │
│ f291e18-dirty  │ │ 0h 22m         │ │ 7.4 MB         │
│ IDF v6.1-rc1   │ │                │ │                │
└────────────────┘ └────────────────┘ └────────────────┘


┌──────────────────────────────┐ ┌──────────────────────────────┐
│ Trạng thái mạng              │ │ Truy cập MCP                 │
│                              │ │                              │
│ ● Đã kết nối                 │ │ ● Không có token            │
│ Wi-Fi      Anh Tu            │ │                              │
│ IP         192.168.1.114     │ │ Bearer token cho MCP client │
│ Tín hiệu   -73 dBm           │ │                              │
│ MAC        9C:13:...         │ │ [ Tạo token ]                │
└──────────────────────────────┘ └──────────────────────────────┘


┌──────────────────────────────────────────────────────────────┐
│ ☁ Cầu nối MCP trực tiếp Xiaozhi                ● Đã kết nối │
│ Kết nối gateway trực tiếp tới Xiaozhi MCP WebSocket          │
├──────────────────────────────────────────────────────────────┤
│                                                              │
│ Bật cầu nối                                      [ ON ]      │
│ Bật/tắt yêu cầu khởi động lại gateway.                       │
│                                                              │
│ Endpoint WebSocket                                           │
│ wss://api.xiaozhi.me/mcp/?...****            [ Thay đổi ]   │
│                                                              │
│ Giao thức                                      2024-11-05    │
│                                                              │
│ [ ↻ Kết nối lại ]                                           │
│                                                              │
│ ───────────────────────────────────────────────────────────  │
│ Xóa cấu hình                                     [ Xóa ]     │
└──────────────────────────────────────────────────────────────┘


┌──────────────────────────────────────────────────────────────┐
│ ⚠ Hành động hệ thống                                        │
│                                                              │
│ Khởi động lại Gateway                                       │
│ Tất cả kết nối BLE sẽ tạm thời bị ngắt.                      │
│                                            [ Khởi động lại ] │
└──────────────────────────────────────────────────────────────┘
```

---

# 5. Header Settings

## Current

Subtitle hiện mang nghĩa:

```text
View system and network status.
```

Nội dung Settings hiện đã nhiều hơn system/network.

## New text

### Vietnamese

```text
Cài đặt Gateway

Quản lý kết nối, quyền truy cập và cấu hình hệ thống.
```

### English

```text
Gateway Settings

Manage connectivity, access, and system configuration.
```

Language selector vẫn ở góc phải.

---

# 6. System Information → Summary Cards

Không dùng một card lớn chứa:

```text
Firmware
Uptime
Free Heap
```

Chuyển sang 3 compact cards.

## Firmware

```text
Firmware

f291e18-dirty
IDF v6.1-rc1
```

## Uptime

```text
Thời gian chạy

0h 22m
```

## Memory

```text
Bộ nhớ trống

7.4 MB
```

Responsive:

```text
desktop:
grid-cols-3

tablet:
grid-cols-2 / responsive wrap

mobile:
grid-cols-1
```

---

# 7. Network Status

Giữ các field:

```text
SSID
IP
MAC
RSSI
```

Nhưng thêm connection state ở đầu card.

Ví dụ:

```text
Trạng thái mạng

● Đã kết nối

Wi-Fi       Anh Tu
IP          192.168.1.114
Tín hiệu    -73 dBm
MAC         9C:13:9E:AA:FA:BC
```

Có thể map RSSI phía frontend:

```text
>= -55 dBm
    → Tốt

-56 .. -67 dBm
    → Trung bình

< -67 dBm
    → Yếu
```

Không cần thay backend để thực hiện presentation này.

---

# 8. MCP Access

MCP card phải hiển thị status trước.

## Không có token

```text
Truy cập MCP

● Không có token

MCP endpoint hiện không yêu cầu Bearer token.

[ Tạo token ]
```

## Có token

```text
Truy cập MCP

● Đã bảo vệ bằng token

Token
••••••••••4A7C

[ Làm mới token ]        Thu hồi
```

`Revoke` phải là destructive secondary action.

Không nên có prominence ngang với `Rotate`.

---

# 9. Token newly generated state

Khi token vừa được tạo:

```text
┌──────────────────────────────────────────────┐
│ Token mới                                    │
│                                              │
│ eyJ...                              [ Copy ] │
│                                              │
│ Token chỉ được hiển thị một lần.             │
└──────────────────────────────────────────────┘
```

Giữ flow hiện tại:

```text
generate
→ browser nhận token
→ user copy
→ reload sẽ không hiện lại full token
```

---

# 10. Xiaozhi phải là full-width card

Không đặt Xiaozhi trong cột 50%.

Lý do:

- có lifecycle state;
- có runtime state;
- có desired enabled state;
- có restart-required state;
- có endpoint secret;
- có reconnect action;
- có protocol/error metadata;
- có destructive configuration action.

Desktop:

```text
col-span-full
```

---

# 11. Xiaozhi header

Header:

```text
☁ Cầu nối MCP trực tiếp Xiaozhi        ● Đã kết nối

Kết nối gateway trực tiếp tới endpoint MCP WebSocket của Xiaozhi.
```

Status badge nằm trong header.

Không để status text đứng một mình ở góc và bị wrap như UI hiện tại.

---

# 12. Xiaozhi status mapping

```text
connected
    → green
    → Đã kết nối

connecting
    → blue
    → Đang kết nối

handshaking
    → blue
    → Đang bắt tay MCP

wait_network
    → amber
    → Đang chờ mạng

backoff
    → amber
    → Đang thử lại

disabled
    → gray
    → Đã tắt

error
    → red
    → Lỗi
```

Optional spinner:

```text
connecting
handshaking
```

---

# 13. Xiaozhi enable switch

Render:

```text
Bật cầu nối                                  [ ON ]

Bật/tắt cầu nối yêu cầu khởi động lại gateway.
```

Switch phải persist ngay.

Không dùng một nút Save chung cho:

```text
enabled
+
endpoint
```

---

# 14. Toggle behavior

Frontend:

```text
toggle change
    ↓
PUT /api/settings/xiaozhi
{
    "enabled": true | false
}
    ↓
receive new xiaozhi state
    ↓
render restart_required
```

Tạo function:

```text
toggleXiaozhiEnabled()
```

Thay vì phụ thuộc vào:

```text
saveXiaozhi()
```

---

# 15. Restart-required state

Nếu:

```text
restart_required=true
```

render banner:

```text
┌──────────────────────────────────────────────────┐
│ ⚠ Cần khởi động lại                              │
│                                                  │
│ Trạng thái bật/tắt mới chưa được áp dụng.        │
│                                                  │
│                         [ Khởi động lại gateway ] │
└──────────────────────────────────────────────────┘
```

Trong state này:

```text
Reconnect
    → disabled
```

vì desired state chưa khớp runtime state.

---

# 16. Endpoint UX — nguyên tắc bắt buộc

Nếu endpoint đã cấu hình:

```text
KHÔNG hiển thị input mặc định.
```

Không render cùng lúc:

```text
Saved endpoint
+
WebSocket endpoint input
```

---

# 17. Endpoint UI state machine

Có 3 state.

## State A — chưa có endpoint

```text
Endpoint WebSocket

[ wss://api.xiaozhi.me/mcp/?token=................ ]

Endpoint chứa thông tin xác thực.

[ Lưu endpoint ]
```

Input hiển thị vì chưa có endpoint.

---

# 18. State B — đã có endpoint

```text
Endpoint WebSocket

wss://api.xiaozhi.me/mcp/?...****          [ Thay đổi ]
```

Không render input.

Không trả full secret endpoint về browser.

---

# 19. State C — đang thay endpoint

```text
Endpoint hiện tại

wss://api.xiaozhi.me/mcp/?...****

Endpoint mới

[________________________________________]

Endpoint mới sẽ thay thế endpoint hiện tại.

[ Lưu thay đổi ]   [ Hủy ]
```

Input phải trống.

Không prefill:

```text
wss://...token=SECRET
```

---

# 20. Security rule cho endpoint

Backend chỉ trả:

```text
endpoint_display
```

đã masked.

Browser không cần endpoint secret cũ.

Điều này phải được giữ.

UI action nên mang nghĩa:

```text
Thay đổi endpoint
```

không phải:

```text
Edit exact stored URL
```

---

# 21. Recommended naming

Vietnamese:

```text
Thay đổi
```

English:

```text
Change
```

Thay vì:

```text
Edit
```

vì user không sửa trực tiếp secret cũ.

---

# 22. Endpoint editor containers

HTML:

```html
<div id="xiaozhi-endpoint-view">
    ...
</div>

<div id="xiaozhi-endpoint-editor" class="hidden">
    ...
</div>
```

Không tạo/xóa toàn bộ form bằng innerHTML nếu không cần thiết.

---

# 23. Endpoint frontend state

Thêm:

```js
xiaozhiEndpointEditing: false
```

Optional:

```js
xiaozhiBusy: false
```

Render:

```text
if endpoint not configured:
    editor create mode

else if xiaozhiEndpointEditing:
    editor replace mode

else:
    masked endpoint view mode
```

---

# 24. Endpoint functions

Tách:

```text
beginXiaozhiEndpointEdit()

cancelXiaozhiEndpointEdit()

saveXiaozhiEndpoint()
```

Không dùng `saveXiaozhi()` để làm cả toggle + endpoint.

---

# 25. Endpoint save behavior

Request:

```json
{
  "endpoint": "wss://..."
}
```

Nếu runtime đang active và backend áp dụng endpoint ngay:

```text
save
→ reconnect
→ state connecting
```

UI nên hiển thị:

```text
Đang kết nối lại với endpoint mới...
```

---

# 26. Xiaozhi metadata

Không render protocol trong một box lớn.

Dùng metadata rows:

```text
Endpoint       wss://...****
Giao thức      2024-11-05
```

Optional:

```text
Runtime        Đang bật
```

không cần hiển thị nếu không mang giá trị UX rõ ràng.

---

# 27. Xiaozhi error state

Chỉ render error block khi thực sự có error.

```text
┌──────────────────────────────────────────────────┐
│ ⚠ Kết nối gần nhất thất bại                      │
│                                                  │
│ ESP xxx · HTTP xxx · WS xxx                      │
└──────────────────────────────────────────────────┘
```

Không giữ placeholder error row khi không có lỗi.

---

# 28. Reconnect action

Reconnect thuộc connection state.

Render:

```text
● Đã kết nối
MCP WebSocket đang hoạt động.

[ ↻ Kết nối lại ]
```

Không đặt Reconnect cạnh:

```text
Save
Clear
```

---

# 29. Reconnect availability

Button enabled khi:

```text
runtime_enabled == true

endpoint_configured == true

restart_required == false

state != connecting

state != handshaking
```

Nếu busy:

```text
disabled
```

và có thể đổi label:

```text
Đang kết nối...
```

---

# 30. Xiaozhi destructive section

Cuối card:

```text
────────────────────────────────────────────

Xóa cấu hình Xiaozhi

Tắt cấu hình và xóa endpoint đã lưu khỏi gateway.

                                      [ Xóa ]
```

Nút:

```text
outline / subtle red
```

Không dùng red primary button lớn.

---

# 31. Clear behavior

Confirmation:

```text
Tắt cầu nối và xóa endpoint Xiaozhi đã lưu?
```

Request giữ semantics hiện tại:

```json
{
  "enabled": false,
  "clear_endpoint": true
}
```

Sau đó render state mới.

---

# 32. System Actions

Danger Zone phải xuống cuối page.

Không đặt ngay dưới System Information.

New section:

```text
Hành động hệ thống

Khởi động lại Gateway

Khởi động lại sẽ tạm ngắt Wi-Fi, MCP và tất cả kết nối BLE.

                                      [ Khởi động lại ]
```

---

# 33. Restart UI

Giữ restart overlay hiện tại nếu đã hoạt động ổn.

Xiaozhi restart-required banner nên reuse cùng:

```text
settings.restartGateway()
```

Không tạo flow restart riêng.

---

# 34. Desktop responsive structure

```text
Settings Header

Summary:
    3 columns

Main cards:
    Network
    MCP Access

Xiaozhi:
    full width

System actions:
    full width
```

---

# 35. Tablet

Suggested:

```text
summary cards:
    2 + wrap

Network:
    full / half depending width

MCP:
    full / half

Xiaozhi:
    full

System actions:
    full
```

---

# 36. Mobile

```text
all sections:
    1 column
```

Actions stack vertically.

Không ép:

```text
Save
Reconnect
Clear
```

trên cùng một row.

Endpoint masked URL phải:

```text
break / truncate safely
```

không overflow card.

---

# 37. Suggested Tailwind layout

Main container:

```text
max-w-6xl
mx-auto
```

Summary:

```text
grid
grid-cols-1
md:grid-cols-3
gap-4
```

Network + MCP:

```text
grid
grid-cols-1
lg:grid-cols-2
gap-6
```

Xiaozhi:

```text
w-full
```

System actions:

```text
w-full
```

---

# 38. Visual hierarchy

Use:

```text
page title
    strongest

card title
    medium

state badge
    semantic color

metadata label
    muted gray

metadata value
    normal/dark

destructive
    red only when necessary
```

Không để quá nhiều background box nested trong card.

---

# 39. Card style consistency

Recommended base:

```text
bg-white
rounded-xl
border border-gray-200
shadow-sm
```

Padding:

```text
p-5 / p-6
```

Không dùng nhiều nested card backgrounds nếu chỉ để hiển thị một value.

---

# 40. Icons

Giữ Phosphor icons hiện tại.

Suggested:

```text
Firmware
    ph-cpu

Uptime
    ph-clock

Memory
    ph-memory

Network
    ph-wifi-high

MCP
    ph-plugs

Xiaozhi
    ph-cloud-arrow-up

System action
    ph-warning-circle
```

Icons chỉ hỗ trợ hierarchy.

Không nên làm icon box quá lớn.

---

# 41. `settings.js` refactor

Current object đang chứa:

```text
load
restartGateway

MCP token methods

renderXiaozhiStatus
saveXiaozhi
clearXiaozhi
reconnectXiaozhi
```

Refactor Xiaozhi thành:

```text
renderXiaozhiStatus()

toggleXiaozhiEnabled()

beginXiaozhiEndpointEdit()
cancelXiaozhiEndpointEdit()
saveXiaozhiEndpoint()

reconnectXiaozhi()
clearXiaozhi()
```

---

# 42. Settings JS state

Recommended:

```js
const settings = {
    mcpState: {
        configured: false,
        preview: ''
    },

    xiaozhiState: {
        enabled: false,
        runtime_enabled: false,
        restart_required: false,
        endpoint_configured: false,
        state: 'disabled'
    },

    xiaozhiEndpointEditing: false,
    xiaozhiBusy: false,
};
```

---

# 43. Remove old mixed save semantics

Remove or deprecate:

```text
saveXiaozhi()
```

because it currently combines:

```text
enabled
+
endpoint
+
save button lifecycle
```

Use focused methods instead.

---

# 44. Loading state

When Settings loads:

```text
summary:
    skeleton / checking

network:
    checking

MCP:
    loading

Xiaozhi:
    loading
```

Avoid layout shift as much as possible.

---

# 45. Xiaozhi request busy state

During:

```text
toggle
endpoint save
reconnect
clear
```

disable relevant controls.

Do not disable entire Settings page.

Example:

```text
reconnect request
    → reconnect button disabled

endpoint save
    → endpoint save button disabled

toggle request
    → switch disabled until response
```

---

# 46. i18n update

Update subtitle:

```text
settings.subtitle
```

Add:

```text
settings.system_summary
settings.network_connected
settings.network_disconnected

settings.mcp_unprotected
settings.mcp_protected

settings.xiaozhi_change_endpoint
settings.xiaozhi_current_endpoint
settings.xiaozhi_new_endpoint
settings.xiaozhi_save_endpoint
settings.xiaozhi_save_changes
settings.xiaozhi_cancel
settings.xiaozhi_remove_config
settings.xiaozhi_connection_active
settings.xiaozhi_restart_now

settings.system_actions
```

---

# 47. Vietnamese wording recommendation

```text
Gateway Settings
    → Cài đặt Gateway

Manage connectivity, access, and system configuration.
    → Quản lý kết nối, quyền truy cập và cấu hình hệ thống.

Change endpoint
    → Thay đổi

Reconnect
    → Kết nối lại

Remove configuration
    → Xóa cấu hình

System actions
    → Hành động hệ thống
```

---

# 48. English wording recommendation

```text
Change
Reconnect
Remove configuration
Restart now
Current endpoint
New endpoint
Connection active
Restart required
```

Avoid vague labels like:

```text
Save
Clear
```

when action semantics can be more explicit.

---

# 49. Files to modify

Primary:

```text
components/web_server/www_src/dashboard/views/settings.html
```

JS:

```text
components/web_server/www_src/dashboard/js/features/settings.js
```

i18n:

```text
components/web_server/www_src/dashboard/js/core/i18n.js
```

Optional shared helpers only if necessary:

```text
components/web_server/www_src/dashboard/js/core/api.js
components/web_server/www_src/dashboard/js/core/ui.js
```

---

# 50. Generated file rule

Do not manually modify:

```text
components/web_server/www/dashboard.html
```

It is generated from modular source.

After changes:

```text
run Web UI build pipeline
```

and allow generated dashboard to be rebuilt.

---

# 51. Backend impact

The redesign should reuse current Settings API as much as possible:

```text
GET /api/settings

PUT /api/settings/xiaozhi

POST /api/settings/xiaozhi/reconnect

POST restart API

MCP token APIs
```

No backend endpoint is required purely for the layout redesign.

---

# 52. Required backend data

Settings GET must continue providing:

```text
system

network

mcp.configured
mcp.preview

xiaozhi.supported
xiaozhi.enabled
xiaozhi.runtime_enabled
xiaozhi.restart_required
xiaozhi.endpoint_configured
xiaozhi.endpoint_display
xiaozhi.state
xiaozhi.protocol_version
xiaozhi error status
```

---

# 53. Security acceptance

Endpoint:

```text
never expose full stored endpoint back to browser
```

Token:

```text
full generated MCP token shown only once
```

HTML:

```text
do not place secret value in data-* attributes
```

Logs:

```text
do not log endpoint token
```

---

# 54. Accessibility

Switch:

```text
proper label association
```

Buttons:

```text
visible text
```

Status:

Do not rely only on color.

Example:

```text
green dot + "Đã kết nối"
```

Keyboard:

```text
endpoint edit
save
cancel
reconnect
```

must be keyboard accessible.

---

# 55. Phase 0 — UX structure

- [ ] Remove old two-column content assignment.
- [ ] Define new page hierarchy.
- [ ] Move Xiaozhi full-width.
- [ ] Move System Actions to bottom.
- [ ] Define responsive breakpoints.

Acceptance:

```text
layout approved before behavior refactor
```

---

# 56. Phase 1 — Summary cards

- [ ] Firmware card.
- [ ] Uptime card.
- [ ] Memory card.
- [ ] Responsive layout.
- [ ] Preserve current data source.

Acceptance:

```text
system summary compact and readable
```

---

# 57. Phase 2 — Network card

- [ ] Add connected state.
- [ ] Reorder metadata.
- [ ] Add RSSI semantic label if desired.
- [ ] Test long SSID.
- [ ] Test mobile.

Acceptance:

```text
network state readable without dense layout
```

---

# 58. Phase 3 — MCP Access redesign

- [ ] Add protected/unprotected status.
- [ ] Keep Generate flow.
- [ ] Keep Rotate flow.
- [ ] Make Revoke secondary destructive.
- [ ] Improve newly generated token panel.

Acceptance:

```text
security state is clear at a glance
```

---

# 59. Phase 4 — Xiaozhi structural redesign

- [ ] Full-width card.
- [ ] Header status badge.
- [ ] Enable switch row.
- [ ] Restart-required banner.
- [ ] Endpoint view container.
- [ ] Endpoint editor container.
- [ ] Protocol metadata.
- [ ] Error block.
- [ ] Reconnect section.
- [ ] Destructive section.

Acceptance:

```text
no endpoint input shown when endpoint exists
```

---

# 60. Phase 5 — Xiaozhi JS refactor

- [ ] Add endpoint editing state.
- [ ] Add busy state.
- [ ] Split toggle handler.
- [ ] Split endpoint save handler.
- [ ] Add begin/cancel edit.
- [ ] Preserve reconnect.
- [ ] Preserve clear.
- [ ] Remove mixed save semantics.

Acceptance:

```text
each UI action maps to one backend intent
```

---

# 61. Phase 6 — i18n

- [ ] Add new English keys.
- [ ] Add new Vietnamese keys.
- [ ] Remove obsolete labels after migration.
- [ ] Test language switch while endpoint editor open.
- [ ] Test status labels.

Acceptance:

```text
no untranslated key visible
```

---

# 62. Phase 7 — Responsive testing

Desktop:

```text
1440
1280
1024
```

Tablet:

```text
768
```

Mobile:

```text
390
375
```

Check:

- [ ] no text overflow;
- [ ] endpoint masked URL does not break layout;
- [ ] action buttons remain usable;
- [ ] status badge does not overlap title;
- [ ] language selector remains accessible.

---

# 63. Phase 8 — Functional regression

## System

- [ ] firmware displayed.
- [ ] IDF displayed.
- [ ] uptime updates/load.
- [ ] heap displayed.

## Network

- [ ] SSID.
- [ ] IP.
- [ ] MAC.
- [ ] RSSI.

## MCP

- [ ] no-token state.
- [ ] generate.
- [ ] copy.
- [ ] rotate.
- [ ] revoke.

## Xiaozhi

- [ ] not configured.
- [ ] configured.
- [ ] endpoint change.
- [ ] enable.
- [ ] disable.
- [ ] restart required.
- [ ] reconnect.
- [ ] error.
- [ ] clear.

## System actions

- [ ] restart confirmation.
- [ ] restart overlay.

---

# 64. Required Xiaozhi scenarios

## Scenario A — fresh system

```text
enabled=false
endpoint_configured=false
```

Expected:

```text
input visible
no masked endpoint
reconnect disabled
```

---

# 65. Scenario B — endpoint saved, disabled

Expected:

```text
masked endpoint visible
input hidden
Change available
Reconnect disabled
```

---

# 66. Scenario C — connected

Expected:

```text
green Connected badge
masked endpoint
protocol visible
Reconnect enabled
input hidden
```

---

# 67. Scenario D — edit endpoint

Expected:

```text
masked current endpoint
new endpoint input
Save changes
Cancel
```

No old full endpoint.

---

# 68. Scenario E — enable changed

Expected:

```text
restart_required banner
Restart button
Reconnect disabled
```

---

# 69. Scenario F — reconnect

Expected:

```text
button disabled while request active
state changes to Connecting
status badge updates
```

---

# 70. Scenario G — error

Expected:

```text
red Error badge
error panel visible
masked endpoint remains safe
```

---

# 71. Definition of Done

Settings redesign is complete when:

- [ ] System summary is compact.
- [ ] Network card has clear connection state.
- [ ] MCP card clearly shows auth state.
- [ ] Xiaozhi is full-width.
- [ ] Xiaozhi status appears as semantic badge.
- [ ] Enable switch persists independently.
- [ ] Restart-required state is explicit.
- [ ] Stored endpoint is shown only as masked value.
- [ ] Endpoint input is hidden when configured.
- [ ] Endpoint editor opens only after explicit Change action.
- [ ] Stored endpoint secret is never returned/prefilled.
- [ ] Reconnect is separated from configuration save.
- [ ] Clear/remove is visually destructive but secondary.
- [ ] Danger/System Actions is at page bottom.
- [ ] Desktop/tablet/mobile layouts are stable.
- [ ] English/Vietnamese translations are complete.
- [ ] Existing APIs remain compatible.
- [ ] Generated dashboard file is not manually edited.
- [ ] No endpoint/token secret appears in DOM or logs unexpectedly.

---

# 72. Final target structure

```text
SETTINGS

┌─ Header ───────────────────────────────────────────┐
│ title + subtitle + language                       │
└────────────────────────────────────────────────────┘

┌─ Summary ──────────────────────────────────────────┐
│ Firmware          Uptime          Free Memory      │
└────────────────────────────────────────────────────┘

┌─ Network ───────────────┐ ┌─ MCP Access ──────────┐
│ connectivity status    │ │ access/security       │
└────────────────────────┘ └────────────────────────┘

┌─ Xiaozhi ─────────────────────────────────────────┐
│ connection status badge                           │
│ enable switch                                     │
│ restart-required banner                           │
│ endpoint view OR endpoint editor                  │
│ protocol/error metadata                           │
│ reconnect                                         │
│ remove configuration                              │
└────────────────────────────────────────────────────┘

┌─ System Actions ──────────────────────────────────┐
│ restart gateway                                   │
└────────────────────────────────────────────────────┘
```

Mục tiêu cuối cùng là Settings có thể trả lời ngay bốn câu hỏi:

```text
Gateway đang ở trạng thái nào?

Mạng và MCP access có ổn không?

Xiaozhi đang chạy thế nào và endpoint nào đang dùng?

Tôi cần làm gì để thay đổi/reconnect/restart?
```

mà không buộc user phải đọc một form dài hoặc hiểu chi tiết implementation bên dưới.
