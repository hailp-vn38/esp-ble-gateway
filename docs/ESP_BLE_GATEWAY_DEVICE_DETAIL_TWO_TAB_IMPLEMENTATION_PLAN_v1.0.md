# ESP BLE Gateway — Device Detail 2-Tab Refactor Implementation Plan

**Document version:** 1.0  
**Date:** 2026-09-06  
**Repository:** `hailp-vn38/esp-ble-gateway`  
**Branch:** `dev-ws`  
**Reviewed baseline commit:** `9c663a645556108a4f167d408c558a897ba67d00`  
**Scope:** Web UI — Device Detail only  
**Target:** chia Device Detail thành 2 tab:

```text
Điều khiển / Control
Cài đặt / Settings
```

---

# 1. Mục tiêu

Device Detail hiện đã có semantic feature UI mới và realtime incremental update.

Bước tiếp theo là tách nội dung Device Detail thành hai context rõ ràng:

```text
CONTROL
= sử dụng thiết bị trong runtime

SETTINGS
= cấu hình / quản trị / quyền truy cập
```

Target cuối:

```text
Device Detail
│
├── Header                         luôn visible
│   ├── Device name
│   ├── BLE address
│   └── connection state
│
├── Summary                        luôn visible
│   ├── Connection
│   └── Schema
│
├── Inner tabs
│   │
│   ├── CONTROL
│   │    └── Feature Dashboard
│   │         ├── Controls
│   │         ├── Environment
│   │         ├── Process Settings
│   │         └── Other
│   │
│   └── SETTINGS
│        ├── Device Information
│        ├── AI / MCP Control
│        ├── Advanced Tools
│        └── Danger Zone
```

---

# 2. Không thay đổi các phần sau

Refactor này chỉ là Web UI presentation.

Không sửa:

```text
BLE protocol
CBOR protocol
device_schema
device_state
WebSocket wire format
MCP backend protocol
Device firmware
feature_ui semantic renderer
REST endpoint structure
```

Không cần thêm:

```text
tab field vào protocol
tab field vào schema
group field vào BLE
UI layout metadata từ device
```

---

# 3. Trạng thái code hiện tại

Current `dev-ws` đã có:

```text
components/web_server/www_src/dashboard/js/features/
├── devices.js
├── feature_ui.js
├── mcp_exposure.js
├── scanner.js
└── settings.js
```

`feature_ui.js` hiện đã chịu trách nhiệm:

```text
presentation registry
feature grouping
semantic renderer
numeric scaling
card map
incremental state update
offline state
pending state
```

Do đó refactor 2 tab **không được đưa presentation logic quay lại `devices.js`**.

`devices.js` tiếp tục chịu:

```text
device state
REST load
WebSocket orchestration
commands
schema refresh
detail view lifecycle
inner tab state
```

---

# 4. Các file cần sửa

## Bắt buộc

| File | Mức thay đổi | Mục đích |
|---|---:|---|
| `components/web_server/www_src/dashboard/views/device_detail.html` | Major | thêm tab bar, chia panel |
| `components/web_server/www_src/dashboard/js/features/devices.js` | Medium | thêm inner-tab state/lifecycle |
| `components/web_server/www_src/dashboard/js/core/i18n.js` | Small | thêm text Control/Settings/Danger Zone |

## Cần verify

| File | Action |
|---|---|
| `components/web_server/www/dashboard.css` | verify các Tailwind class dùng trong tab tồn tại |
| `components/web_server/www_src/dashboard/js/features/feature_ui.js` | không sửa, regression only |
| `components/web_server/www_src/dashboard/js/features/mcp_exposure.js` | không sửa, verify hidden panel vẫn render |
| `components/web_server/tools/build_webui.py` | smoke build |

---

# 5. UX architecture

## 5.1. Những gì luôn visible

Giữ ngoài tab:

```text
Back to Devices
Device Header
Connection / Schema summary
Tab bar
```

Lý do:

- khi đang ở Settings vẫn phải thấy device identity;
- connection state là global device state;
- schema state liên quan cả Control và Settings;
- không duplicate DOM giữa hai tab.

## 5.2. Tab `Control`

Chỉ chứa runtime interaction:

```text
Feature dashboard
Offline notice
Refresh schema
```

Các feature group bên trong vẫn do `feature_ui.js` tự tạo:

```text
Controls
Environment
Process settings
Other
```

Không đưa:

```text
MCP permission
Device ID
BLE address
Custom command
Remove device
```

vào tab này.

## 5.3. Tab `Settings`

Thứ tự đề xuất:

```text
1 Device Information
2 AI / MCP Control
3 Advanced Tools
4 Danger Zone
```

Không đặt MCP đầu tiên.

---

# 6. File 1 — `device_detail.html`

**Path:**

```text
components/web_server/www_src/dashboard/views/device_detail.html
```

## 6.1. Current high-level structure

Hiện đang là:

```html
<section id="view-device-detail">

    back

    detail-header

    detail-summary

    device-controls-card

    mcp-control-card

    device-info-card

    device-advanced-section

    device-management-card

</section>
```

Target:

```html
<section id="view-device-detail">

    back

    detail-header

    detail-summary

    detail-tabs

    detail-panel-control
        device-controls-card

    detail-panel-settings
        device-info-card
        mcp-control-card
        device-advanced-section
        device-management-card

</section>
```

## 6.2. Header — giữ ngoài tab

Current header:

```text
device name
MAC
status
Edit
```

Phase 1 có thể giữ nút Edit ở header để giảm scope.

Recommended final UX:

```text
device name
MAC
status
```

và chuyển Edit vào Settings.

Tài liệu này chọn:

```text
Phase 1: giữ Edit để refactor tab an toàn
Phase 2: optional move Edit vào Device Information
```

Không bắt buộc move Edit trong commit tab đầu tiên.

## 6.3. Thêm tab bar

Chèn ngay sau:

```html
<div id="detail-summary">...</div>
```

Target markup:

```html
<div
    id="device-detail-tabs"
    class="border-b border-gray-200"
    role="tablist">

    <button
        id="detail-tab-control"
        type="button"
        role="tab"
        aria-selected="true"
        aria-controls="detail-panel-control"
        tabindex="0"
        onclick="devices.setDetailTab('control')"
        onkeydown="devices.handleDetailTabKeydown(event)"
        class="px-1 py-3 mr-6 border-b-2 border-brand-500
               text-sm font-semibold text-brand-600
               transition-colors">

        <span data-i18n="device_detail.tab_control">
            Control
        </span>
    </button>

    <button
        id="detail-tab-settings"
        type="button"
        role="tab"
        aria-selected="false"
        aria-controls="detail-panel-settings"
        tabindex="-1"
        onclick="devices.setDetailTab('settings')"
        onkeydown="devices.handleDetailTabKeydown(event)"
        class="px-1 py-3 border-b-2 border-transparent
               text-sm font-semibold text-gray-500
               hover:text-gray-700 transition-colors">

        <span data-i18n="device_detail.tab_settings">
            Settings
        </span>
    </button>
</div>
```

## 6.4. Không dùng page navigation cho inner tab

Không:

```js
nav.switchTab('control')
nav.switchTab('settings')
```

`nav.switchTab()` dành cho page-level views:

```text
devices
device-detail
settings
...
```

Control/Settings mới là:

```text
inner local view state
```

chỉ hide/show panel.

## 6.5. Wrap Control panel

Current:

```html
<section id="device-controls-card">
    ...
</section>
```

Wrap thành:

```html
<div
    id="detail-panel-control"
    role="tabpanel"
    aria-labelledby="detail-tab-control"
    class="space-y-6">

    <section
        id="device-controls-card"
        class="bg-white rounded-xl border border-gray-200
               shadow-sm overflow-hidden">

        ...
    </section>

</div>
```

Không đổi ID:

```text
device-controls-card
feature-offline-notice
feature-cards
btn-refresh-schema
```

để tránh phải sửa code feature hiện tại.

## 6.6. Wrap Settings panel

Tạo:

```html
<div
    id="detail-panel-settings"
    role="tabpanel"
    aria-labelledby="detail-tab-settings"
    class="hidden space-y-6">
```

Sau đó move nguyên các section hiện có vào trong.

Order target:

```html
<div id="detail-panel-settings">

    <section id="device-info-card">
        ...
    </section>

    <section id="mcp-control-card">
        ...
    </section>

    <details id="device-advanced-section">
        ...
    </details>

    <section id="device-management-card">
        ...
    </section>

</div>
```

Important:

**move DOM node, không duplicate.**

Không tạo hai:

```text
mcp-control-card
device-info-card
```

có cùng ID.

## 6.7. Device Information nên đứng đầu Settings

Current order:

```text
MCP
Device Information
Advanced
Management
```

Target:

```text
Device Information
MCP
Advanced
Danger Zone
```

Lý do:

Settings context nên đi từ:

```text
identity
→ permissions/integrations
→ debug
→ destructive action
```

## 6.8. Rename Management section

Current:

```text
Device Management
```

Target visual label:

```text
Danger Zone
```

ID có thể giữ:

```text
device-management-card
```

để tránh code change.

Chỉ đổi i18n key/text.

Recommended:

```text
add device_detail.danger_zone
```

vì semantic rõ hơn.

## 6.9. Optional — move Edit vào Settings

Không bắt buộc cho refactor đầu tiên.

Nếu làm:

1. remove button trong `detail-header`;
2. add button vào `device-info-card`;
3. vẫn gọi:

```js
ui.openEditModal()
```

Target:

```html
<div class="flex items-start justify-between gap-4">
    <div>
        <h3>Device Information</h3>
        <p>Identity and BLE metadata</p>
    </div>

    <button onclick="ui.openEditModal()">
        Edit
    </button>
</div>
```

Không đổi edit modal logic.

## 6.10. Tab panel visibility

Initial HTML:

```text
control panel = visible
settings panel = hidden
```

Dùng class:

```text
hidden
```

không dùng inline style.

---

# 7. File 2 — `devices.js`

**Path:**

```text
components/web_server/www_src/dashboard/js/features/devices.js
```

## 7.1. Add inner-tab state

Near object-level state:

```js
const devices = {
    detailLoadId: 0,
    currentSchemaState: 'unknown',
    currentFeatures: [],
```

Add:

```js
currentDetailTab: 'control',
```

Không đưa state này vào global `state` object.

Lý do:

```text
inner tab = ephemeral presentation state
```

không phải domain state.

## 7.2. Add DOM helper

Recommended:

```js
_detailTabElements() {
    return {
        controlTab:
            document.getElementById('detail-tab-control'),

        settingsTab:
            document.getElementById('detail-tab-settings'),

        controlPanel:
            document.getElementById('detail-panel-control'),

        settingsPanel:
            document.getElementById('detail-panel-settings')
    };
},
```

## 7.3. Add `setDetailTab()`

Target implementation:

```js
setDetailTab(tab, options = {}) {
    const normalized =
        tab === 'settings'
            ? 'settings'
            : 'control';

    const {
        focus = false
    } = options;

    const {
        controlTab,
        settingsTab,
        controlPanel,
        settingsPanel
    } = this._detailTabElements();

    if (!controlTab ||
        !settingsTab ||
        !controlPanel ||
        !settingsPanel) {
        return;
    }

    this.currentDetailTab = normalized;

    const controlActive =
        normalized === 'control';

    controlPanel.classList.toggle(
        'hidden',
        !controlActive
    );

    settingsPanel.classList.toggle(
        'hidden',
        controlActive
    );

    this._updateDetailTabButton(
        controlTab,
        controlActive
    );

    this._updateDetailTabButton(
        settingsTab,
        !controlActive
    );

    if (focus) {
        (
            controlActive
                ? controlTab
                : settingsTab
        ).focus();
    }
},
```

## 7.4. Add button state helper

```js
_updateDetailTabButton(button, active) {
    button.setAttribute(
        'aria-selected',
        active ? 'true' : 'false'
    );

    button.tabIndex =
        active ? 0 : -1;

    button.classList.toggle(
        'border-brand-500',
        active
    );

    button.classList.toggle(
        'text-brand-600',
        active
    );

    button.classList.toggle(
        'border-transparent',
        !active
    );

    button.classList.toggle(
        'text-gray-500',
        !active
    );

    button.classList.toggle(
        'hover:text-gray-700',
        !active
    );
},
```

Không replace toàn `className` nếu không cần.

## 7.5. Keyboard navigation

Add:

```js
handleDetailTabKeydown(event) {
    if (!event) return;

    if (event.key !== 'ArrowLeft' &&
        event.key !== 'ArrowRight' &&
        event.key !== 'Home' &&
        event.key !== 'End') {
        return;
    }

    event.preventDefault();

    let next;

    if (event.key === 'Home') {
        next = 'control';
    } else if (event.key === 'End') {
        next = 'settings';
    } else {
        next =
            this.currentDetailTab === 'control'
                ? 'settings'
                : 'control';
    }

    this.setDetailTab(
        next,
        { focus: true }
    );
},
```

Expected:

```text
Arrow Left/Right = switch
Home             = Control
End              = Settings
```

## 7.6. Reset tab khi mở device

Current `openDetailView()` đã:

```js
state.selectedDeviceDetail = dev;
this.currentFeatures = [];
```

Add:

```js
this.currentDetailTab = 'control';
```

Sau:

```js
nav.switchTab('device-detail', updateRoute);
```

call:

```js
this.setDetailTab('control');
```

Target:

```js
openDetailView(dev, updateRoute = true) {
    state.selectedDeviceDetail = dev;
    this.currentFeatures = [];
    this.currentDetailTab = 'control';

    document.getElementById(
        'device-advanced-section'
    ).open = false;

    ...

    i18n.applyTranslations();

    nav.switchTab(
        'device-detail',
        updateRoute
    );

    this.setDetailTab('control');

    void this._reloadDetailCoalesced('open');
}
```

## 7.7. Vì sao reset Control khi mở device?

Bad flow:

```text
Device A
→ Settings
→ Back
→ Device B
→ vẫn Settings
```

Target:

```text
open any new device
→ Control
```

## 7.8. Không reload detail khi switch inner tab

`setDetailTab()` tuyệt đối không gọi:

```js
loadDetail()
_reloadDetailCoalesced()
api.getDeviceDetailSnapshot()
refreshSchema()
```

Tab switch chỉ:

```text
DOM visibility
ARIA state
focus
```

## 7.9. WebSocket khi Settings đang mở

Không suspend:

```text
feature.state
device.connection
device.schema
```

Current `devices.js` đã dùng:

```js
featureUi.updateFeatureState(...)
featureUi.updateConnectionState(...)
```

giữ nguyên.

Nếu Control panel hidden:

```text
DOM card vẫn tồn tại
→ update vẫn chạy
→ user quay lại thấy state mới
```

## 7.10. Không destroy feature DOM khi sang Settings

Không:

```js
featureCards.replaceChildren()
```

khi click Settings.

Chỉ:

```text
detail-panel-control.hidden
```

Lợi ích:

- không mất `featureUi.cardByFeatureKey`;
- WS incremental update vẫn tìm được card;
- không cần re-render khi quay lại Control.

## 7.11. Schema event khi Settings active

Current flow:

```text
device.schema
→ _reloadDetailCoalesced()
→ loadDetail()
→ render features
→ render MCP controls
```

Giữ nguyên.

Sau reload:

```text
Settings vẫn active
```

Do `loadDetail()` không gọi `setDetailTab()`.

Chỉ reset tab trong:

```text
openDetailView()
```

## 7.12. Connection event

Current:

```js
featureUi.updateConnectionState(...)
```

giữ nguyên.

Nếu Settings active, hidden Control state vẫn update.

## 7.13. MCP renderer trong hidden panel

`loadDetail()` vẫn phải gọi:

```js
mcpControls.renderFeatures(...)
```

kể cả Settings hidden.

Không defer MCP render theo tab trong phase đầu.

---

# 8. File 3 — `i18n.js`

**Path:**

```text
components/web_server/www_src/dashboard/js/core/i18n.js
```

## 8.1. English

Add:

```js
'device_detail.tab_control':
    'Control',

'device_detail.tab_settings':
    'Settings',

'device_detail.danger_zone':
    'Danger Zone',
```

Optional:

```js
'device_detail.settings_desc':
    'Configure device identity, AI access and advanced options.',
```

## 8.2. Vietnamese

Add:

```js
'device_detail.tab_control':
    'Điều khiển',

'device_detail.tab_settings':
    'Cài đặt',

'device_detail.danger_zone':
    'Khu vực nguy hiểm',
```

## 8.3. Naming recommendation

English:

```text
Control
Settings
```

Vietnamese:

```text
Điều khiển
Cài đặt
```

Không dùng label dài.

---

# 9. `feature_ui.js` — không sửa

**Path:**

```text
components/web_server/www_src/dashboard/js/features/feature_ui.js
```

Current engine đã có:

```text
cardByFeatureKey
featureKey()
renderFeatures()
updateFeatureState()
updateConnectionState()
groupFeatures()
```

Tab refactor không cần chạm renderer.

Important:

```text
Control panel hidden
!=
feature cards destroyed
```

Do đó `cardByFeatureKey` vẫn valid.

---

# 10. `mcp_exposure.js` — không sửa

MCP panel chỉ move vào Settings.

Giữ nguyên ID:

```text
mcp-feature-controls
```

Existing renderer không cần biết panel hidden hay visible.

---

# 11. Gateway global Settings không liên quan

Project có:

```text
features/settings.js
```

đó là Gateway-level settings.

Không reuse cho Device Detail tab Settings.

Hai context khác nhau:

```text
Gateway Settings
vs
Device Settings
```

---

# 12. Refresh Schema

Giữ button:

```text
btn-refresh-schema
```

trong Control.

Không move vào Settings trong phase đầu.

Reason:

```text
schema refresh
→ feature dashboard thay đổi trực tiếp
```

---

# 13. Advanced Tools

Giữ `<details>`.

Không mở default.

Behavior:

```text
same device:
Settings → Control → Settings
→ giữ open state

new device:
→ close Advanced
```

Current `openDetailView()` đã close Advanced, giữ behavior đó.

---

# 14. Offline behavior

Control:

```text
offline notice
last values
writable controls disabled
```

Settings:

```text
device info available
MCP config available
Advanced visible
Remove usable
```

Tab navigation luôn usable.

---

# 15. Pending command khi đổi tab

Scenario:

```text
Control
→ send fan command
→ click Settings
→ ACK/WS arrives
```

Expected:

```text
command continues
hidden card updates
pending clears
return Control → actual state shown
```

Không cancel command vì inner tab switch.

---

# 16. Accessibility

Required:

```text
role="tablist"
role="tab"
role="tabpanel"
aria-selected
aria-controls
aria-labelledby
tabindex
ArrowLeft/ArrowRight
Home/End
```

Không đặt whole tab panel vào:

```text
aria-live
```

Sensor stream không được spam screen reader.

---

# 17. Hidden panel accessibility

Class:

```text
hidden
```

=> `display:none`.

Hidden panel tự ra khỏi accessibility tree.

Không cần `aria-hidden` thêm.

---

# 18. Focus behavior

Click:

```text
switch panel
```

Keyboard:

```text
switch + focus active tab
```

Open Device:

```text
không force focus tab
```

---

# 19. Routing

MVP không encode inner tab vào URL.

Không thêm:

```text
?tab=settings
```

Không lưu localStorage.

Source of truth:

```js
devices.currentDetailTab
```

---

# 20. CSS / Tailwind

Project embed generated:

```text
components/web_server/www/dashboard.css
```

Không giả định class mới tự build.

Before merge:

```text
verify tab utility classes exist
```

Nếu thiếu:

```text
regenerate dashboard.css
```

Không dùng dynamic Tailwind:

```js
`border-${color}-500`
```

Dùng literal classes.

---

# 21. Suggested tab classes

Base:

```text
px-1
py-3
border-b-2
text-sm
font-semibold
transition-colors
```

Control active:

```text
border-brand-500
text-brand-600
```

Inactive:

```text
border-transparent
text-gray-500
hover:text-gray-700
```

---

# 22. Mobile

Tab bar vẫn 2 tab cùng hàng.

Không stack.

Target touch height khoảng 44px.

Settings cards:

```text
full width
```

Feature dashboard responsive hiện tại giữ nguyên.

---

# 23. State lifecycle

```text
OPEN DEVICE
   ↓
Control

click Settings
   ↓
Settings

feature event
   ↓
hidden Control card updated
   ↓
remain Settings

schema event
   ↓
detail reload
   ↓
remain Settings

connection event
   ↓
header/summary/control state update
   ↓
remain Settings

back
   ↓
Devices

open new device
   ↓
Control
```

---

# 24. Test plan

## T1 — Open Device

Expected:

```text
Control active
Settings hidden
header visible
summary visible
```

ARIA:

```text
control aria-selected=true
settings=false
control tabindex=0
settings=-1
```

## T2 — Switch Settings

Expected:

```text
Control hidden
Settings visible
no REST request
```

## T3 — Return Control

Expected:

```text
same feature DOM
no card map reset
```

## T4 — New Device

```text
Device A → Settings
Back
Device B
```

Expected:

```text
Device B → Control
```

## T5 — Schema event while Settings active

Expected:

```text
stay Settings
hidden features reload
MCP reload
summary update
```

## T6 — Feature WS event while Settings active

Expected hidden feature state update.

Return Control → latest state.

## T7 — Disconnect while Settings

Expected:

```text
header offline
summary offline
hidden feature controls disabled
Settings remains active
```

## T8 — Reconnect while Settings

Expected:

```text
online
feature controls enabled
Settings remains active
```

## T9 — MCP hidden render

Open device default Control.

Expected MCP renderer still runs.

Switch Settings → data immediately available.

## T10 — MCP action

Settings → MCP permission toggle.

Expected unchanged.

## T11 — Advanced

Settings → open Advanced → Control → Settings.

Expected still open.

Open another device → closed.

## T12 — Delete

Settings → Danger Zone → Remove.

Expected existing delete flow.

## T13 — Edit

Edit flow unchanged.

## T14 — Keyboard

```text
ArrowRight → Settings
ArrowLeft  → Control
End        → Settings
Home       → Control
```

## T15 — Mobile

No overflow.

Tabs same row.

## T16 — Offline initial open

Default Control, Settings still accessible.

## T17 — Detail/schema error

Control error state, Settings still usable.

## T18 — Language

EN:

```text
Control / Settings
```

VI:

```text
Điều khiển / Cài đặt
```

Active tab retained.

## T19 — Network

Click tabs repeatedly.

Expected:

```text
0 device-detail REST calls caused by tab switch
```

## T20 — Realtime performance

Sensor updates:

```text
no detail panel rebuild
no full feature rebuild
incremental card update only
```

---

# 25. Implementation phases

## Phase TAB-0 — HTML

### File

```text
device_detail.html
```

### Work

- tab bar;
- two panels;
- move sections;
- reorder Settings.

### Checklist

- [ ] no duplicate IDs.
- [ ] Control default.
- [ ] Settings hidden.
- [ ] header outside.
- [ ] summary outside.
- [ ] Feature card moved, not duplicated.
- [ ] MCP moved, not duplicated.
- [ ] Advanced moved.
- [ ] Management moved.

### Exit

Static layout correct.

---

## Phase TAB-1 — JS

### File

```text
devices.js
```

### Add

```text
currentDetailTab
_detailTabElements
_updateDetailTabButton
setDetailTab
handleDetailTabKeydown
```

### Modify

```text
openDetailView
```

### Checklist

- [ ] no API call in setDetailTab.
- [ ] no feature render in setDetailTab.
- [ ] no WS subscription change.
- [ ] schema reload preserves tab.
- [ ] new device resets Control.

---

## Phase TAB-2 — i18n

### File

```text
i18n.js
```

### Add

```text
Control
Settings
Danger Zone
```

EN/VI.

---

## Phase TAB-3 — Accessibility

Verify:

- [ ] role tablist.
- [ ] role tab.
- [ ] role tabpanel.
- [ ] aria-selected.
- [ ] aria-controls.
- [ ] tabindex.
- [ ] Arrow.
- [ ] Home/End.
- [ ] focus visible.

---

## Phase TAB-4 — Regression

Test:

```text
feature realtime
MCP
schema refresh
offline/reconnect
edit
delete
advanced
mobile
i18n
```

---

# 26. File-level summary

## `device_detail.html`

Add IDs:

```text
device-detail-tabs
detail-tab-control
detail-tab-settings
detail-panel-control
detail-panel-settings
```

Control:

```text
device-controls-card
```

Settings:

```text
device-info-card
mcp-control-card
device-advanced-section
device-management-card
```

Outside:

```text
detail-header
detail-summary
```

## `devices.js`

Add:

```js
currentDetailTab
_detailTabElements()
_updateDetailTabButton()
setDetailTab()
handleDetailTabKeydown()
```

Modify:

```js
openDetailView()
```

Do not change behavior of:

```text
_handleFeatureStateEvent
_applyConnectionEvent
_handleSchemaEvent
loadDetail
sendFeatureCommand
refreshSchema
```

## `i18n.js`

Add EN/VI keys.

---

# 27. Files không cần sửa

```text
feature_ui.js
mcp_exposure.js
api.js
events.js
web_device_detail_api.c
web_command_api.c
device_schema
device_state
mcp_endpoint
```

---

# 28. Build verification

Assemble Web UI:

```bash
python components/web_server/tools/build_webui.py   --source components/web_server/www_src   --dashboard-out /tmp/dashboard.html
```

Verify output contains:

```text
detail-tab-control
detail-tab-settings
detail-panel-control
detail-panel-settings
setDetailTab
```

Then verify embedded CSS classes.

---

# 29. Browser smoke checklist

- [ ] detail opens.
- [ ] Control active.
- [ ] features visible.
- [ ] Settings hidden.
- [ ] Settings click instant.
- [ ] Device Info visible.
- [ ] MCP visible.
- [ ] Advanced visible.
- [ ] Danger Zone visible.
- [ ] Control return instant.
- [ ] no network request on tab switch.
- [ ] realtime values fresh.

---

# 30. Suggested commit sequence

```text
commit 1
feat(web-ui): split device detail into control and settings tabs

commit 2
feat(web-ui): add accessible keyboard navigation for device detail tabs

commit 3
refactor(web-ui): move device edit action into settings
```

Commit 3 optional.

Không mix feature renderer changes vào commit tabs.

---

# 31. Definition of Done

1. Device Detail có đúng 2 inner tabs.
2. Header và summary luôn visible.
3. Control là default.
4. Control chỉ chứa feature runtime dashboard.
5. Settings chứa Device Info/MCP/Advanced/Danger Zone.
6. Không duplicate DOM IDs.
7. Tab switch không gọi REST.
8. Tab switch không rebuild feature UI.
9. Tab switch không cancel command.
10. WebSocket vẫn update hidden feature cards.
11. Schema reload giữ active tab.
12. New device reset Control.
13. EN/VI hoạt động.
14. Keyboard navigation hoạt động.
15. Mobile không overflow.
16. MCP/edit/delete/advanced regression pass.
17. `feature_ui.js` architecture giữ nguyên.
18. Build Web UI/firmware pass.
