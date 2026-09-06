# ESP BLE Gateway — Modern Feature UI Refactor Plan

**Document version:** 1.0
**Date:** 2026-09-06
**Repository:** `hailp-vn38/esp-ble-gateway`
**Branch:** `dev-ws`
**Reviewed baseline:** `7905f0c39f6a165fff69f0f278d69baafe8e0e80`
**Scope:** Device Detail → `Features` section only
**Goal:** Redesign the feature UI into a compact, modern, semantic, schema-driven device dashboard.

---

# 1. Mục tiêu

UI `Features` hiện tại đang hiển thị tất cả feature theo cùng một card layout:

```text
Feature title + semantic badge
state
generic control area
```

Điều này hoạt động về mặt chức năng nhưng chưa tốt về UX khi thiết bị có nhiều feature:

- relay / plug / light quá tốn chiều cao;
- sensor read-only bị hiển thị như "không có write tool";
- fan / dimmer dùng slider quá dài;
- state quan trọng bị đặt nhỏ ở góc;
- semantic badge lặp lại title;
- generic feature chưa có presentation riêng;
- mỗi WebSocket state event đang rebuild toàn bộ feature list.

Mục tiêu mới:

```text
Features
├── Controls
│   ├── Relay
│   ├── Plug
│   ├── Light
│   ├── Dimmer
│   └── Fan
│
├── Environment
│   ├── Temperature
│   ├── Humidity
│   └── Contact
│
└── Process Settings
    ├── Dryer Temperature
    └── Drying Time
```

UI phải:

- tự chọn renderer theo semantic type/value type;
- dùng `feature.title` làm display name;
- giữ `feature_id` cho internal identity;
- hiển thị read-only sensor như metric, không phải lỗi;
- render `GENERIC_VALUE` bằng metadata;
- cập nhật riêng card khi WebSocket state thay đổi;
- không hard-code theo model thiết bị.

---

# 2. Hiện trạng code

Các file chính hiện tại:

```text
components/web_server/
├── web_device_detail_api.c
├── CMakeLists.txt
├── tailwind.config.js
└── www_src/dashboard/
    ├── shell.html
    ├── views/
    │   └── device_detail.html
    └── js/
        ├── core/
        │   ├── api.js
        │   └── i18n.js
        └── features/
            ├── devices.js
            └── mcp_exposure.js
```

`devices.js` hiện đang làm quá nhiều trách nhiệm:

```text
device loading
device realtime
feature grouping
feature rendering
feature controls
format numeric
send command
DOM rebuild
```

Target refactor:

```text
devices.js
    ↓ orchestration/state/commands

feature_ui.js
    ↓ presentation/grouping/render/update DOM
```

---

# 3. File change matrix

## File bắt buộc sửa

| File | Mức thay đổi | Mục đích |
|---|---:|---|
| `www_src/dashboard/js/features/devices.js` | Major | tách renderer, incremental realtime update |
| `www_src/dashboard/js/features/feature_ui.js` | New | semantic presentation registry + renderers |
| `www_src/dashboard/views/device_detail.html` | Medium | đổi feature container/layout |
| `www_src/dashboard/shell.html` | Small | include `feature_ui.js` trước `devices.js` |
| `www_src/dashboard/js/core/i18n.js` | Medium | thêm labels mới |

## File cần kiểm tra / có thể sửa nhỏ

| File | Action |
|---|---|
| `web_device_detail_api.c` | verify API contract; hiện đã gần đủ metadata |
| `www_src/dashboard/js/core/api.js` | không bắt buộc sửa |
| `tailwind.config.js` | không cần sửa nếu class dùng static strings |
| `CMakeLists.txt` | không cần thêm JS thủ công vì đã `GLOB_RECURSE www_src/*.js` |

---

# 4. File 1 — `devices.js`

**Path:**

```text
components/web_server/www_src/dashboard/js/features/devices.js
```

Đây là file cần refactor lớn nhất.

---

## 4.1. Logic hiện tại cần giữ

Các phần sau vẫn thuộc `devices.js`:

```text
load()
loadDetail()
_syncFromSnapshot()
_handleSchemaEvent()
sendCommand()
sendToggle()
refreshSchema()
device connection handling
feature state cache
```

Các helper numeric hiện có cũng có thể giữ hoặc chuyển sang `feature_ui.js`:

```js
scaleOf(feature)
rawToDisplay(feature, raw)
displayToRaw(feature, value)
formatNumeric(feature, raw)
```

Khuyến nghị: chuyển các helper presentation sang `featureUi`.

---

## 4.2. Logic cần tách khỏi `devices.js`

Hiện có:

```js
renderFeatures()
renderFeatureCard()
renderFeaturesLoading()
renderFeaturesEmptyState()
renderFeaturesError()
```

Target:

```js
featureUi.renderFeatures()
featureUi.renderFeatureCard()
featureUi.updateFeatureState()
featureUi.renderLoading()
featureUi.renderEmpty()
featureUi.renderError()
```

`devices.js` chỉ gọi renderer.

---

## 4.3. Update `renderFeatures()`

### Hiện tại

```js
renderFeatures(features, device) {
    const container = document.getElementById('feature-cards');
    container.replaceChildren();

    if (!features.length) {
        this.renderFeaturesEmptyState(container, device);
        return;
    }

    features.forEach(feature =>
        container.appendChild(this.renderFeatureCard(feature, device)));
}
```

### Target

```js
renderFeatures(features, device) {
    featureUi.renderFeatures({
        container: document.getElementById('feature-cards'),
        features,
        device,
        onToggle: feature => this.sendToggle(feature),
        onNumericSet: (feature, displayValue, controls) => {
            const raw = featureUi.displayToRaw(feature, displayValue);

            return this.sendCommand(
                feature.control.write_command,
                'integer',
                raw,
                controls
            );
        }
    });
}
```

Hoặc đơn giản:

```js
renderFeatures(features, device) {
    featureUi.renderFeatures(
        document.getElementById('feature-cards'),
        features,
        device,
        this
    );
}
```

Không khuyến nghị truyền toàn object `devices` nếu muốn loose coupling; callback tốt hơn.

---

## 4.4. Update `_handleFeatureStateEvent()`

### Hiện tại

Sau khi update state:

```js
this.renderFeatures(
    this.currentFeatures,
    state.selectedDeviceDetail
);
```

Điều này rebuild tất cả card.

### Target

Sau khi update state object:

```js
featureUi.updateFeatureState(feature, state.selectedDeviceDetail);
```

Pseudo:

```js
_handleFeatureStateEvent(ev) {
    ...
    for (const feature of this.currentFeatures) {
        if (feature.feature_id === ev.featureId &&
            feature.property_id === ev.propertyId) {

            if (!feature.state) feature.state = {};

            feature.state.valid = true;

            if (ev.valueType === 'bool') {
                feature.state.value_bool = ev.value;
            } else if (ev.valueType === 'int') {
                feature.state.value_int = ev.value;
            }

            feature.state.updated_at_ms = ev.updatedAtMs;

            featureUi.updateFeatureState(
                feature,
                state.selectedDeviceDetail
            );

            break;
        }
    }
}
```

---

## 4.5. Update `_reconcileFeatureCacheAfterSnapshot()`

Hiện cuối hàm gọi:

```js
this.renderFeatures(...)
```

Target:

Nếu có nhiều cached state mới hơn snapshot:

```text
update state objects
collect changed feature keys
update affected cards
```

Pseudo:

```js
const changed = [];

...
changed.push(feature);

...

for (const feature of changed) {
    featureUi.updateFeatureState(
        feature,
        state.selectedDeviceDetail
    );
}
```

Fallback:

Nếu DOM chưa được tạo:

```js
this.renderFeatures(...)
```

---

## 4.6. Update `_applyConnectionEvent()`

Hiện online/offline change có thể gọi lại:

```js
this.renderFeatures(...)
```

Target:

```js
featureUi.updateConnectionState(
    this.currentFeatures,
    state.selectedDeviceDetail
);
```

Không rebuild toàn feature tree.

`updateConnectionState()` chỉ:

- disable/enable writable controls;
- giữ metric value;
- update offline state class.

---

## 4.7. Add pending state per feature

Trong `devices`:

```js
pendingFeatureCommands: new Set(),
```

Key:

```text
feature_id:property_id
```

Hoặc Map:

```js
pendingFeatureCommands: new Map()
```

Helper:

```js
featureKey(feature) {
    return `${feature.feature_id}:${feature.property_id}`;
}
```

Khi send command:

```text
before request
→ featureUi.setPending(feature, true)

after success/error
→ featureUi.setPending(feature, false)
```

Tốt hơn nếu `sendCommand()` nhận optional feature:

```js
async sendFeatureCommand(feature, valueType, value)
```

Target:

```js
async sendFeatureCommand(feature, valueType, value) {
    featureUi.setPending(feature, true);

    try {
        return await this.sendCommand(
            feature.control.write_command,
            valueType,
            value
        );
    } finally {
        featureUi.setPending(feature, false);
    }
}
```

---

## 4.8. Không dùng `feature.value_type` và `semantic.value_type` lẫn lộn

Hiện code có:

```js
feature.value_type === 1
```

và nơi khác:

```js
feature.semantic?.value_type === 'bool'
```

V2 UI nên chuẩn hóa trong `feature_ui.js`:

```js
valueKind(feature) {
    if (feature.value_type === 1) return 'bool';
    if (feature.value_type === 2) return 'int';

    return feature.semantic?.value_type || 'none';
}
```

Mọi renderer chỉ gọi helper này.

---

# 5. File 2 — tạo mới `feature_ui.js`

**New path:**

```text
components/web_server/www_src/dashboard/js/features/feature_ui.js
```

Đây là file presentation chính.

---

## 5.1. Responsibilities

`featureUi` chịu trách nhiệm:

```text
semantic presentation registry
feature grouping
icon mapping
card renderer dispatch
numeric scaling
state formatting
incremental card updates
control pending state
connection/offline visual state
```

Không chịu trách nhiệm:

```text
fetch API
BLE command
WebSocket subscription
device store
schema refresh
MCP
```

---

## 5.2. Base object

```js
const featureUi = {
    groupOrder: [
        'controls',
        'environment',
        'settings',
        'other'
    ],

    presentations: {
        relay: {
            group: 'controls',
            renderer: 'toggle',
            icon: 'ph-lightning'
        },

        outlet: {
            group: 'controls',
            renderer: 'toggle',
            icon: 'ph-plug'
        },

        light: {
            group: 'controls',
            renderer: 'toggle',
            icon: 'ph-lightbulb'
        },

        dimmer: {
            group: 'controls',
            renderer: 'range',
            icon: 'ph-sun'
        },

        fan: {
            group: 'controls',
            renderer: 'range',
            icon: 'ph-fan'
        },

        temperature: {
            group: 'environment',
            renderer: 'metric',
            icon: 'ph-thermometer'
        },

        humidity: {
            group: 'environment',
            renderer: 'metric',
            icon: 'ph-drop'
        },

        contact: {
            group: 'environment',
            renderer: 'contact',
            icon: 'ph-door'
        },

        value: {
            group: 'settings',
            renderer: 'generic-number',
            icon: 'ph-sliders-horizontal'
        }
    }
};
```

Tên semantic thực tế phải map theo `device_template_semantic_name()`.

Nếu current semantic names khác:

```text
relay
outlet
light
dimmable_light
fan
temperature
humidity
contact
value
```

thì registry phải dùng đúng tên backend đang trả.

---

## 5.3. Presentation resolution

Không chỉ dựa semantic name.

Target:

```js
resolvePresentation(feature) {
    const semantic = feature.semantic?.name;

    if (semantic && this.presentations[semantic]) {
        return this.presentations[semantic];
    }

    // Fallback theo metadata.
    const valueKind = this.valueKind(feature);
    const writable = feature.control?.writable === true;

    if (valueKind === 'bool' && writable) {
        return {
            group: 'controls',
            renderer: 'toggle',
            icon: 'ph-toggle-left'
        };
    }

    if (valueKind === 'int' && writable) {
        return {
            group: 'settings',
            renderer: 'generic-number',
            icon: 'ph-sliders-horizontal'
        };
    }

    if (valueKind === 'int') {
        return {
            group: 'environment',
            renderer: 'metric',
            icon: 'ph-gauge'
        };
    }

    return {
        group: 'other',
        renderer: 'fallback',
        icon: 'ph-cube'
    };
}
```

Điều này giúp future feature chưa có renderer semantic vẫn dùng được.

---

# 6. Group features

Add:

```js
groupFeatures(features) {
    const groups = {
        controls: [],
        environment: [],
        settings: [],
        other: []
    };

    for (const feature of features) {
        const presentation =
            this.resolvePresentation(feature);

        groups[presentation.group].push(feature);
    }

    return groups;
}
```

---

## 6.1. Group order

Recommended:

```text
Controls
Environment
Process settings
Other
```

Không sort theo alphabetical mặc định.

Trong mỗi group giữ registration/schema order để UI ổn định.

---

# 7. Render group

Add:

```js
renderFeatureGroup(groupKey, features, device, callbacks)
```

Structure:

```html
<section class="feature-group">
    <div class="feature-group-header">
        <h4>Controls</h4>
        <span>5</span>
    </div>

    <div class="feature-grid">
        ...
    </div>
</section>
```

Tailwind target:

```text
mb-6 last:mb-0
grid grid-cols-1 md:grid-cols-2 gap-3
```

Environment có thể:

```text
grid-cols-1 sm:grid-cols-2 xl:grid-cols-3
```

Nhưng để code đơn giản v1:

```text
grid-cols-1 md:grid-cols-2
```

cho tất cả group.

---

# 8. Common card shell

Mọi feature card có common shell:

```js
createFeatureCardShell(feature, presentation)
```

Target structure:

```html
<article
  data-feature-key="fan_main:3"
  class="rounded-xl border border-gray-200 bg-white p-4 shadow-sm">

  <div class="flex items-start justify-between gap-3">

      <div class="flex items-center gap-3">
          <div class="icon"></div>

          <div>
              <h5>Quạt sấy</h5>
              <div class="optional secondary"></div>
          </div>
      </div>

      <div data-role="feature-state"></div>

  </div>

  <div data-role="feature-control"></div>

</article>
```

Do not show semantic badge by default.

`feature_id` có thể:

- tooltip title;
- `data-feature-id`;
- advanced/debug section.

---

# 9. Toggle renderer

Function:

```js
renderToggleFeature(feature, device, callbacks)
```

Dùng cho:

```text
relay
plug
light
generic writable BOOL
```

---

## 9.1. UI target

```text
┌────────────────────────────────┐
│ ⚡ Relay chính            On    │
│                                │
│                         ─────●  │
└────────────────────────────────┘
```

Không dùng button text:

```text
Turn On
Turn Off
```

---

## 9.2. Accessible switch

HTML:

```html
<button
  type="button"
  role="switch"
  aria-checked="true"
  data-role="toggle">
</button>
```

Hoặc reuse switch CSS pattern ở `shell.html`.

Recommended CSS/Tailwind-only:

```text
relative inline-flex h-6 w-11 items-center rounded-full
```

Knob:

```text
inline-block h-5 w-5 transform rounded-full bg-white
```

Nếu Tailwind dynamic translate class khó safelist, dùng explicit class strings trong code.

---

## 9.3. States

### OFF

```text
label: Off
switch: gray
```

### ON

```text
label: On
switch: brand
```

### PENDING

```text
disable switch
spinner small
```

### OFFLINE

```text
disable switch
keep last state visible
```

### UNKNOWN

```text
state = —
disabled if no known state policy requires
```

---

# 10. Range renderer

Function:

```js
renderRangeFeature()
```

Dùng cho:

```text
dimmer
fan
generic numeric when slider suitable
```

---

## 10.1. UI target

```text
🌀 Quạt sấy                       60 %

0 ━━━━━━━━━━━━━━━●━━━━━━━━━━━━ 100
```

Không dùng layout:

```text
slider + input + Apply
```

trên một hàng dài như hiện tại.

---

## 10.2. Slider behavior

Important:

```text
input event
→ preview only

change / pointerup
→ send command once
```

Không gửi command mỗi pixel drag.

Pseudo:

```js
range.oninput = () => {
    valueLabel.textContent =
        this.formatDisplay(
            feature,
            Number(range.value)
        );
};

range.onchange = async () => {
    await callbacks.onNumericSet(
        feature,
        Number(range.value),
        [range]
    );
};
```

---

## 10.3. Numeric input

Có thể giữ numeric input nhưng không hiển thị song song trên desktop v1.

Recommended:

- click current value → edit;
- hoặc small input dưới slider;
- hoặc giữ hidden/advanced.

MVP:

```text
slider + value label
```

Generic precise values:

```text
slider + numeric field
```

---

# 11. Metric renderer

Function:

```js
renderMetricFeature()
```

Dùng cho:

```text
temperature
humidity
generic read-only INT
```

---

## 11.1. UI target

```text
┌─────────────────────────┐
│ 🌡 Nhiệt độ hiện tại   │
│                         │
│       25.1 °C           │
│                         │
│ Updated just now        │
└─────────────────────────┘
```

State phải là visual focus.

---

## 11.2. Không hiển thị

Không render:

```text
No write tool available
```

Read-only là hợp lệ.

Chỉ hiển thị warning nếu:

```text
control says writable
but write_command missing
```

Đó mới là schema inconsistency.

---

# 12. Contact renderer

Function:

```js
renderContactFeature()
```

Mapping:

```text
false -> Closed
true  -> Open
```

Target:

```text
🚪 Cửa buồng sấy

● Closed
```

UI màu:

```text
Closed -> neutral/green
Open   -> amber
```

Không dùng text ON/OFF.

---

# 13. Generic numeric renderer

Function:

```js
renderGenericNumberFeature()
```

Dùng cho:

```text
GENERIC_VALUE
unknown writable INT
```

Target metadata:

```text
title
unit
decimals
control.minimum
control.maximum
control.step
state.value_int
```

---

## 13.1. Numeric conversion

Move helpers here:

```js
scaleOf(feature) {
    return 10 ** Math.max(
        0,
        Number(feature?.decimals) || 0
    );
},

rawToDisplay(feature, raw) {
    return Number(raw) / this.scaleOf(feature);
},

displayToRaw(feature, value) {
    return Math.round(
        Number(value) * this.scaleOf(feature)
    );
}
```

---

## 13.2. Formatting

```js
formatNumeric(feature, raw) {
    const decimals =
        Math.max(0, Number(feature.decimals) || 0);

    const value =
        this.rawToDisplay(feature, raw);

    return `${value.toFixed(decimals)}${
        feature.unit ? ` ${feature.unit}` : ''
    }`;
}
```

---

## 13.3. Dryer temperature example

Backend:

```json
{
  "title": "Nhiệt độ sấy",
  "unit": "°C",
  "decimals": 1,
  "control": {
    "minimum": 300,
    "maximum": 1000,
    "step": 5
  },
  "state": {
    "value_int": 655
  }
}
```

UI:

```text
Nhiệt độ sấy
65.5 °C

30.0 ━━━━━━━●━━━━━━━━ 100.0
```

---

# 14. Group label renderer

Add i18n keys:

```text
feature_group.controls
feature_group.environment
feature_group.settings
feature_group.other
```

Display:

```text
Controls
Environment
Process settings
Other
```

Vietnamese:

```text
Điều khiển
Môi trường
Thiết lập
Khác
```

---

# 15. Incremental realtime update

Đây là thay đổi performance quan trọng.

---

## 15.1. Card identity

Every card:

```html
data-feature-key="temperature_main:5"
```

Helper:

```js
featureKey(feature) {
    return `${feature.feature_id}:${feature.property_id}`;
}
```

DOM-safe lookup tránh CSS escaping:

```js
findCard(feature) {
    const key = this.featureKey(feature);

    return document.querySelector(
        `[data-feature-key="${CSS.escape(key)}"]`
    );
}
```

Hoặc maintain:

```js
cardByFeatureKey = new Map()
```

Map tốt hơn, không cần querySelector mỗi event.

Recommended:

```js
cardByFeatureKey: new Map()
```

Clear map khi full render.

---

## 15.2. Update state only

```js
updateFeatureState(feature, device) {
    const key = this.featureKey(feature);
    const card = this.cardByFeatureKey.get(key);

    if (!card) return false;

    const presentation =
        this.resolvePresentation(feature);

    switch (presentation.renderer) {
    case 'toggle':
        this.updateToggleState(card, feature, device);
        break;

    case 'range':
    case 'generic-number':
        this.updateNumericState(card, feature, device);
        break;

    case 'metric':
        this.updateMetricState(card, feature);
        break;

    case 'contact':
        this.updateContactState(card, feature);
        break;
    }

    return true;
}
```

---

## 15.3. Slider interaction guard

Problem:

WebSocket state event có thể tới trong lúc user đang drag slider.

Add:

```text
data-user-editing=true
```

On pointer down:

```js
range.dataset.userEditing = 'true';
```

On change/end:

```js
delete range.dataset.userEditing;
```

When WS update:

```js
if (range.dataset.userEditing !== 'true') {
    range.value = displayValue;
}
```

Không làm slider nhảy dưới tay user.

---

# 16. Last updated display

Helper:

```js
formatUpdatedAt(updatedAtMs)
```

Không cần realtime ticking mỗi second.

Recommended buckets:

```text
< 10 sec  -> Just now
< 60 sec  -> Less than a minute ago
< 1 hour  -> Xm ago
otherwise -> time string
```

Để tránh timer per card:

- update text khi event đến;
- optionally refresh all timestamps mỗi 60s bằng một shared timer.

MVP có thể chỉ hiển thị:

```text
Updated just now
```

khi state event mới.

---

# 17. Offline state

Feature UI offline behavior:

```text
read-only state
→ vẫn visible

write control
→ disabled

card
→ không opacity toàn bộ

status
→ optional "Offline"
```

Do not hide last known state.

---

# 18. File 3 — `device_detail.html`

**Path:**

```text
components/web_server/www_src/dashboard/views/device_detail.html
```

---

## 18.1. Current container

Hiện:

```html
<div
  id="feature-cards"
  class="space-y-3"
  aria-live="polite">
</div>
```

---

## 18.2. Target container

Use:

```html
<div
    id="feature-cards"
    class="space-y-6"
    aria-live="polite">
</div>
```

Group renderer tự tạo grid.

Không hard-code grid ở root vì mỗi group có thể khác layout.

---

## 18.3. Features header

Current:

```text
Features
Semantic features detected from this device's schema.
Refresh
```

Target description:

```text
Features
Monitor and control this device in real time.
```

Vietnamese:

```text
Tính năng
Theo dõi và điều khiển thiết bị theo thời gian thực.
```

---

## 18.4. Optional feature count

Có thể add:

```html
<span id="feature-count"></span>
```

Example:

```text
Features · 10
```

Không bắt buộc MVP.

---

# 19. File 4 — `shell.html`

**Path:**

```text
components/web_server/www_src/dashboard/shell.html
```

Current script order:

```html
<!-- @js core/nav.js -->
<!-- @js features/devices.js -->
<!-- @js features/scanner.js -->
```

Target:

```html
<!-- @js core/nav.js -->

<!-- @js features/feature_ui.js -->
<!-- @js features/devices.js -->

<!-- @js features/scanner.js -->
```

`feature_ui.js` phải xuất hiện trước `devices.js`.

---

## 19.1. Không cần sửa CMake khi thêm JS

`CMakeLists.txt` hiện:

```cmake
file(GLOB_RECURSE WEBUI_SOURCES CONFIGURE_DEPENDS
    ".../www_src/*.html"
    ".../www_src/*.js"
)
```

Do đó file mới tự nằm trong dependency graph.

Nhưng file **không tự inject vào dashboard**.

Vẫn bắt buộc add:

```html
<!-- @js features/feature_ui.js -->
```

trong `shell.html`.

---

# 20. File 5 — `i18n.js`

**Path:**

```text
components/web_server/www_src/dashboard/js/core/i18n.js
```

Add keys ít nhất:

```text
device_detail.features_modern_desc

feature_group.controls
feature_group.environment
feature_group.settings
feature_group.other

feature_state.on
feature_state.off
feature_state.open
feature_state.closed
feature_state.unknown

feature_state.updated_now
feature_state.offline

feature_control.pending
feature_control.apply
```

English:

```js
'feature_group.controls': 'Controls',
'feature_group.environment': 'Environment',
'feature_group.settings': 'Process settings',
'feature_group.other': 'Other',

'feature_state.on': 'On',
'feature_state.off': 'Off',
'feature_state.open': 'Open',
'feature_state.closed': 'Closed',
'feature_state.unknown': 'Unknown',
'feature_state.updated_now': 'Updated just now',
```

Vietnamese:

```js
'feature_group.controls': 'Điều khiển',
'feature_group.environment': 'Môi trường',
'feature_group.settings': 'Thiết lập',
'feature_group.other': 'Khác',

'feature_state.on': 'Bật',
'feature_state.off': 'Tắt',
'feature_state.open': 'Mở',
'feature_state.closed': 'Đóng',
'feature_state.unknown': 'Chưa có dữ liệu',
'feature_state.updated_now': 'Vừa cập nhật',
```

---

# 21. File 6 — `web_device_detail_api.c`

**Path:**

```text
components/web_server/web_device_detail_api.c
```

## Current assessment

API hiện đã trả gần đủ metadata UI cần:

```text
feature_id
title
unit
feature_type
property_id
value_type
decimals

semantic.name
semantic.property
semantic.value_type
semantic.primary_property

control.writable
control.write_command
control.minimum
control.maximum
control.step

state.valid
state.value_bool / state.value_int
state.updated_at_ms
```

Do đó **UI redesign MVP không bắt buộc sửa backend**.

---

## 21.1. Chỉ sửa nếu cần normalize field

Có thể optional add:

```json
"control": {
  "writable": false
}
```

đã có.

Không cần add:

```text
ui_type
icon
group
color
```

Không đưa presentation metadata vào firmware/Gateway schema.

Presentation là responsibility của Web UI.

---

## 21.2. Bug/robustness cần review

Current `state_to_json()` đang quyết định BOOL/INT bằng:

```c
device_template_property_value_type(
    feature->property_id
)
```

Trong v2 schema đã có:

```c
feature->value_type
```

Khuyến nghị cân nhắc chuyển sang:

```c
feature->value_type
```

để unknown/generic feature vẫn serialize đúng, kể cả template registry chưa biết property.

Target:

```c
if (feature->value_type == DEVICE_TEMPLATE_VALUE_BOOL) {
    ...
} else if (feature->value_type == DEVICE_TEMPLATE_VALUE_INT) {
    ...
}
```

Đây không phải cosmetic UI change; đây là robustness fix cho generic feature.

---

# 22. File 7 — `api.js`

**Path:**

```text
components/web_server/www_src/dashboard/js/core/api.js
```

Không bắt buộc sửa.

Hiện:

```js
getDeviceDetailSnapshot()
```

trả nguyên detail payload.

UI có thể dùng trực tiếp metadata mới.

Không map/sanitize feature fields lại trong `api.js`.

---

# 23. File 8 — `tailwind.config.js`

**Path:**

```text
components/web_server/tailwind.config.js
```

Không cần sửa nếu tất cả class tồn tại dưới dạng literal string trong:

```text
www_src/**/*.js
www_src/**/*.html
```

Config hiện đã scan cả hai path.

---

## 23.1. Không tạo class động

Avoid:

```js
`bg-${color}-100`
```

Tailwind compiler có thể không phát hiện.

Use explicit registry:

```js
styles: {
    active: 'bg-brand-600',
    inactive: 'bg-gray-300'
}
```

---

# 24. File 9 — `CMakeLists.txt`

**Path:**

```text
components/web_server/CMakeLists.txt
```

Không cần thêm `feature_ui.js`.

Current build đã:

```cmake
GLOB_RECURSE
www_src/*.html
www_src/*.js
```

Need only ensure normal build regenerates:

```text
dashboard.html
dashboard.html.gz
```

---

# 25. Proposed `feature_ui.js` public API

```js
const featureUi = {
    cardByFeatureKey: new Map(),

    renderFeatures({
        container,
        features,
        device,
        onToggle,
        onNumericSet
    }),

    updateFeatureState(feature, device),

    updateConnectionState(features, device),

    setPending(feature, pending),

    groupFeatures(features),

    resolvePresentation(feature),

    valueKind(feature),

    scaleOf(feature),

    rawToDisplay(feature, raw),

    displayToRaw(feature, value),

    formatNumeric(feature, raw)
};
```

Keep public API nhỏ.

Private/internal helpers dùng prefix `_`:

```js
_renderGroup()
_renderToggle()
_renderRange()
_renderMetric()
_renderContact()
_renderGenericNumber()

_updateToggle()
_updateRange()
_updateMetric()
_updateContact()
```

---

# 26. Semantic renderer matrix

| Semantic | Renderer | Group | Icon | Writable |
|---|---|---|---|---|
| `relay` | toggle | controls | lightning | yes |
| `outlet` | toggle | controls | plug | yes |
| `light` | toggle | controls | lightbulb | yes |
| `dimmable_light` | range | controls | sun | yes |
| `fan` | range | controls | fan | yes |
| `temperature` | metric | environment | thermometer | no |
| `humidity` | metric | environment | drop | no |
| `contact` | contact | environment | door | no |
| `value` writable | generic-number | settings | sliders | yes |
| `value` read-only | metric | environment/other | gauge | no |
| unknown BOOL writable | toggle | controls | toggle | yes |
| unknown INT writable | generic-number | settings | sliders | yes |
| unknown INT readonly | metric | other | gauge | no |

---

# 27. UI dimensions

Recommended desktop card:

```text
min-height: ~120px toggle
min-height: ~140px range
min-height: ~140px metric
```

Do not force identical height if content differs significantly.

Grid:

```text
mobile:
1 column

>= md:
2 columns
```

Avoid 3 columns for control group because:

```text
Vietnamese titles
slider
numeric unit
```

need width.

Environment group may become 3 columns later.

---

# 28. Read-only behavior

Current anti-pattern:

```text
Temperature
25.1°C
No write tool available
```

Target:

```text
Temperature
25.1°C
Updated just now
```

Only display error for actual invalid schema:

```text
writable == true
but write_command missing
```

Then show:

```text
Control unavailable
```

---

# 29. Command UX

## Toggle

Send immediately once.

## Slider

Preview on drag.

Send once on release/change.

## Generic numeric

If precision matters:

```text
range
+
number input
```

Apply behavior:

Option A — explicit Apply:

```text
safer for industrial setpoint
```

Option B — send on change:

```text
faster for dimmer/fan
```

Recommended split:

```text
dimmer/fan
→ send on slider change

GENERIC_VALUE
→ numeric input + explicit Apply
```

This is important for setpoints such as:

```text
Nhiệt độ sấy
Áp suất
Thời gian
```

---

# 30. Generic setpoint card

Target:

```text
┌────────────────────────────────────┐
│ 🌡 Nhiệt độ sấy          65.5 °C  │
│                                    │
│ 30.0 ━━━━━━━━━●━━━━━━━━━━ 100.0    │
│                                    │
│ [ 65.5 ] °C             [ Apply ] │
└────────────────────────────────────┘
```

This renderer is different from fan/dimmer despite all being writable INT.

Rule:

```text
semantic fan/dimmer
→ range quick-control

generic value
→ precise setpoint control
```

---

# 31. State synchronization rules

Source of truth order:

```text
1. authoritative WebSocket feature.state
2. REST detail snapshot
3. local preview only while user edits
```

Do not permanently mutate authoritative state on slider drag.

Use local preview separately.

---

# 32. Accessibility

Required:

- toggle `role="switch"`;
- `aria-checked`;
- range has label;
- numeric input has label;
- card heading semantic;
- disabled control conveys offline;
- focus-visible style;
- keyboard range works;
- pending state not color-only.

---

# 33. Mobile behavior

At mobile width:

```text
1 card / row
```

Toggle card:

```text
title left
switch right
```

Range:

```text
header
value
slider full width
```

Generic setpoint:

```text
numeric input + unit
Apply full/compact right
```

Avoid:

```text
slider + number + Apply
```

in one horizontal row.

---

# 34. Performance constraints

No expensive full DOM rebuild for state event.

Full render allowed only on:

```text
open detail
schema reload
schema change
device switch
```

Incremental update on:

```text
feature.state
connection state
pending command
```

Target:

```text
sensor 2 Hz
→ update one text node/card
```

not:

```text
sensor 2 Hz
→ recreate all 10 cards
```

---

# 35. Implementation phases

## Phase UI-0 — Presentation foundation

### Files

```text
feature_ui.js new
shell.html
```

### Work

- presentation registry;
- group resolution;
- numeric helpers;
- include order.

### Checklist

- [ ] feature_ui loads before devices.
- [ ] no API calls inside featureUi.
- [ ] semantic fallback works.

### Tests

- unknown BOOL;
- unknown INT;
- known semantic types.

---

## Phase UI-1 — Grouped card layout

### Files

```text
device_detail.html
feature_ui.js
i18n.js
```

### Work

- group headers;
- 2-column grid;
- feature title;
- icons;
- remove semantic badge from normal UI.

### Checklist

- [ ] Controls group.
- [ ] Environment group.
- [ ] Settings group.
- [ ] Other fallback.
- [ ] responsive layout.

---

## Phase UI-2 — Specialized renderers

### Files

```text
feature_ui.js
devices.js
```

### Work

- toggle;
- range;
- metric;
- contact;
- generic setpoint.

### Checklist

- [ ] relay.
- [ ] plug.
- [ ] light.
- [ ] dimmer.
- [ ] fan.
- [ ] temperature.
- [ ] humidity.
- [ ] contact.
- [ ] generic value.

---

## Phase UI-3 — Incremental realtime

### Files

```text
devices.js
feature_ui.js
```

### Work

- card map;
- updateFeatureState;
- updateConnectionState;
- slider editing guard.

### Checklist

- [ ] no full render on feature.state.
- [ ] state value updates.
- [ ] toggle updates.
- [ ] slider not jump while editing.
- [ ] offline state updates without full render.

---

## Phase UI-4 — Command states

### Files

```text
devices.js
feature_ui.js
i18n.js
```

### Work

- pending;
- disable control;
- spinner;
- error toast remains global;
- authoritative ACK/WS wins.

---

## Phase UI-5 — Polish / accessibility

### Files

```text
feature_ui.js
device_detail.html
i18n.js
```

### Work

- aria;
- focus;
- mobile;
- timestamp;
- empty/loading states.

---

# 36. Detailed test plan

## T1 — Layout with 10 features

Expected:

```text
2-column desktop
1-column mobile
grouping correct
```

---

## T2 — BOOL

Relay OFF:

```text
switch off
label Off
```

Click.

Expected:

```text
pending
command
WS/ACK state
switch on
```

---

## T3 — Fan

Raw:

```text
60
```

Expected:

```text
60%
slider=60
```

Drag to 75.

Expected:

```text
preview 75
one command after release
```

---

## T4 — Temperature

Raw:

```text
251
decimals=1
```

Expected:

```text
25.1 °C
```

No "No write tool".

---

## T5 — Humidity

Expected metric card.

---

## T6 — Contact

false:

```text
Closed
```

true:

```text
Open
```

---

## T7 — Generic dryer temperature

Raw:

```text
655
```

Expected:

```text
65.5 °C
```

Input 70.0.

Expected API:

```text
700
```

---

## T8 — Command clamp

User asks:

```text
65.5
```

Device state returns:

```text
650
```

Expected UI final:

```text
65.0°C
```

---

## T9 — WS update performance

Send repeated temp events.

Verify:

```text
only temp card DOM changes
```

No full `feature-cards.replaceChildren()`.

---

## T10 — Offline

Disconnect.

Expected:

```text
last values remain
controls disabled
banner visible
```

Reconnect:

```text
controls enabled
```

---

## T11 — Unknown feature

Unknown INT writable.

Expected:

```text
generic numeric card
```

not unsupported error.

---

## T12 — Old schema

Missing:

```text
title
unit
decimals
```

Expected:

```text
title fallback semantic/feature_id
unit empty
decimals 0
```

---

# 37. Build verification

Web UI build uses:

```text
tools/build_webui.py
```

Normal ESP-IDF build regenerates:

```text
dashboard.html
dashboard.html.gz
```

After adding new file, verify build dependency picks it up.

No manual update to embedded asset list is required because generated dashboard is the embedded artifact.

---

# 38. Recommended commit sequence

```text
commit 1
refactor(web-ui): add semantic feature presentation module

commit 2
feat(web-ui): group feature cards by controls sensors and settings

commit 3
feat(web-ui): add toggle range metric contact and generic renderers

commit 4
perf(web-ui): update feature cards incrementally from websocket state

commit 5
feat(web-ui): add pending offline and precise setpoint states

commit 6
test(web-ui): validate semantic feature dashboard scenarios
```

---

# 39. Definition of Done

- [ ] `feature_ui.js` exists and is loaded before `devices.js`.
- [ ] `devices.js` no longer owns card-specific markup.
- [ ] feature groups render correctly.
- [ ] no semantic badges cluttering normal cards.
- [ ] relay/plug/light use switch.
- [ ] fan/dimmer use compact range controls.
- [ ] temperature/humidity are metric cards.
- [ ] contact uses Open/Closed semantic state.
- [ ] generic numeric setpoint renderer works.
- [ ] title/unit/decimals work.
- [ ] read-only features do not show "No write tool".
- [ ] offline keeps last state visible.
- [ ] feature.state updates one card only.
- [ ] sliders do not fight WS updates while dragging.
- [ ] command pending state visible.
- [ ] desktop uses compact grid.
- [ ] mobile is usable.
- [ ] old feature schema fallback works.
- [ ] no backend presentation enum was added.
- [ ] no Device protocol change is required for this UI refactor.

---

# 40. Final target architecture

```text
REST /api/devices/detail
          │
          ▼
      devices.js
 state + commands + WS
          │
          ▼
     feature_ui.js
 presentation engine
          │
     ┌────┼─────────┬────────┬─────────────┐
     ▼    ▼         ▼        ▼             ▼
 Toggle  Range    Metric   Contact    Generic Value
     │    │         │        │             │
 Relay  Fan      Temp      Door        Setpoints
 Plug   Dimmer   Humidity
 Light
```

Nguyên tắc cuối:

```text
Backend describes WHAT the feature is.
Web UI decides HOW it should look.
```

Không đưa icon, màu sắc, layout hoặc widget type vào BLE protocol.
