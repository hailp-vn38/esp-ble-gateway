# ESP BLE Gateway — Modern Feature UI Refactor Plan

**Document version:** 1.1 (reviewed)  
**Date:** 2026-09-06  
**Repository:** `hailp-vn38/esp-ble-gateway`  
**Branch:** `dev-ws`  
**Reviewed baseline:** `7905f0c39f6a165fff69f0f278d69baafe8e0e80`  
**Scope:** Device Detail → `Features` section only  
**Goal:** Redesign the feature UI into a compact, modern, semantic, schema-driven device dashboard.
**Review status:** implementation-ready after corrections listed in this version.

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
| `www/icons.css` | Medium | icon policy: chỉ dùng icon đã tồn tại hoặc thêm mapping đã xác minh |
| `web_command_api.c` | Medium | trả authoritative feature state trong command response |
| `www_src/dashboard/js/core/api.js` | Small | giữ/parse command authoritative state |
| `www/dashboard.css` | Conditional | rebuild nếu thêm Tailwind utilities mới |

## File cần kiểm tra / có thể sửa nhỏ

| File | Action |
|---|---|
| `web_device_detail_api.c` | robustness: serialize state theo `feature->value_type` |
| `tailwind.config.js` | content paths đã đúng; không tự tạo CSS mới |
| `tailwind.input.css` | optional nếu thêm component CSS |
| `CMakeLists.txt` | không add JS thủ công; nhưng cũng không compile Tailwind |
| `tools/build_webui.py` | build smoke test, không cần sửa logic |

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

## 5.2. Presentation resolution — không dùng semantic name đơn lẻ

Gateway hiện có semantic collision hợp lệ:

```text
ON_OFF_LIGHT      -> semantic.name = "light", property = "on_off"
DIMMABLE_LIGHT    -> semantic.name = "light", property = "level"
```

Vì vậy **không được** dùng registry:

```js
presentations.light = ...
presentations.dimmable_light = ...
```

vì backend không trả semantic `dimmable_light`.

Target key:

```js
presentationKey(feature) {
    const name = feature.semantic?.name || 'unknown';
    const property = feature.semantic?.property || 'unknown';
    return `${name}:${property}`;
}
```

Registry:

```js
const featureUi = {
    groupOrder: ['controls', 'environment', 'settings', 'other'],

    presentations: {
        'relay:on_off': {
            group: 'controls',
            renderer: 'toggle',
            icon: 'power'
        },

        'outlet:on_off': {
            group: 'controls',
            renderer: 'toggle',
            icon: 'plugs'
        },

        'light:on_off': {
            group: 'controls',
            renderer: 'toggle',
            icon: 'lightbulb'
        },

        'light:level': {
            group: 'controls',
            renderer: 'range',
            icon: 'lightbulb'
        },

        'fan:percent_setting': {
            group: 'controls',
            renderer: 'range',
            icon: 'devices'
        },

        'temperature:temperature': {
            group: 'environment',
            renderer: 'metric',
            icon: 'thermometer'
        },

        'humidity:humidity': {
            group: 'environment',
            renderer: 'metric',
            icon: 'info'
        },

        'contact:contact': {
            group: 'environment',
            renderer: 'contact',
            icon: 'info'
        },

        'value:value': {
            group: 'settings',
            renderer: 'generic-number',
            icon: 'info'
        }
    }
};
```

Các icon phía trên cố ý chỉ dùng subset đã tồn tại trong `www/icons.css`.

Nếu muốn icon riêng như:

```text
fan
drop
door
sun
sliders-horizontal
gauge
lightning
```

phải update `www/icons.css` bằng glyph mapping đã xác minh với font Phosphor đang embed. Không giả định class `ph-*` sẽ tự hoạt động.

---

## 5.3. Fallback presentation

Không chỉ dựa semantic name.

```js
resolvePresentation(feature) {
    const key = this.presentationKey(feature);

    if (this.presentations[key]) {
        const base = this.presentations[key];

        // Generic VALUE read-only không phải "settings".
        if (key === 'value:value' &&
            feature.control?.writable !== true) {
            return {
                ...base,
                group: 'other',
                renderer: 'metric'
            };
        }

        return base;
    }

    const valueKind = this.valueKind(feature);
    const writable = feature.control?.writable === true;

    if (valueKind === 'bool' && writable) {
        return {
            group: 'controls',
            renderer: 'toggle',
            icon: 'power'
        };
    }

    if (valueKind === 'int' && writable) {
        return {
            group: 'settings',
            renderer: 'generic-number',
            icon: 'info'
        };
    }

    if (valueKind === 'int') {
        return {
            group: 'other',
            renderer: 'metric',
            icon: 'info'
        };
    }

    return {
        group: 'other',
        renderer: 'fallback',
        icon: 'info'
    };
}
```

Rule quan trọng:

```text
known sensor semantics -> environment
GENERIC_VALUE read-only -> other/monitoring
```

Không mặc định mọi read-only INT là environment.

---

## 5.4. Icon policy

Current `www/icons.css` chỉ expose một subset nhỏ của Phosphor icons.

V2 UI phase 1 nên:

```text
power        -> ph-power
plugs        -> ph-plugs
lightbulb    -> ph-lightbulb
thermometer  -> ph-thermometer
devices      -> ph-devices
info         -> ph-info
```

Không dùng class chưa tồn tại.

Optional later phase:

```text
update icons.css
+ verify glyph codepoint
+ visual regression
```

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

Protocol/UI contract phải được khóa rõ:

```text
false -> Closed
true  -> Open
```

Chỉ dùng mapping này cho semantic:

```text
contact:contact
```

Không áp dụng cho unknown BOOL feature.

Nếu product/protocol chưa cam kết mapping trên, renderer phải fallback về `Inactive/Active` thay vì tự đoán.

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
    class="space-y-6">
</div>
```

**Không giữ `aria-live="polite"` trên toàn feature tree.**

Sensor events có thể cập nhật thường xuyên; nếu cả container là live-region,
screen reader sẽ bị spam announcement.

Dùng live region riêng cho:

```text
loading/error
command success/error
schema refresh status
```

Ví dụ:

```html
<div id="feature-status"
     class="sr-only"
     role="status"
     aria-live="polite"></div>
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

## 19.1. JS dependency và CSS build là hai vấn đề khác nhau

`CMakeLists.txt` hiện:

```cmake
file(GLOB_RECURSE WEBUI_SOURCES CONFIGURE_DEPENDS
    ".../www_src/*.html"
    ".../www_src/*.js"
)
```

Do đó `feature_ui.js` mới tự nằm trong dependency graph.

Nhưng file **không tự inject vào dashboard**.

Vẫn bắt buộc add:

```html
<!-- @js features/feature_ui.js -->
```

trong `shell.html`.

### Critical: CMake không compile Tailwind

Firmware embed trực tiếp:

```text
www/dashboard.css
```

CMake chỉ assemble/gzip HTML/JS.

Vì vậy việc thêm static Tailwind class vào `feature_ui.js` **không đảm bảo class đó tồn tại trong firmware CSS**.

Trước khi dùng utility mới:

1. kiểm tra class đã có trong `www/dashboard.css`; hoặc
2. rebuild `www/dashboard.css` bằng Tailwind toolchain của project/developer environment; hoặc
3. dùng component CSS đã tồn tại, ví dụ `.settings-switch`.

Đặc biệt các class như:

```text
h-6
w-11
```

không được giả định có sẵn chỉ vì `tailwind.config.js` scan `www_src/**/*.js`.

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

# 20.1. File mới trong scope — `web_command_api.c`

**Path:**

```text
components/web_server/web_command_api.c
```

Đây là correction quan trọng so với v1.0.

Current `device_command_result_t` đã nhận được authoritative fields:

```text
has_feature_value_bool
feature_value_bool
has_feature_value_int
feature_value_int
```

nhưng `web_command_api.c` hiện chỉ trả:

```json
{
  "success": true,
  "status": 0
}
```

UI v1.0 giả định authoritative state sẽ tới qua WebSocket.

Điều đó chưa đủ robust khi:

```text
WS degraded
event delayed
event dropped -> resync required
command clamp actual != requested
```

### Target response

Nếu result có feature state:

```json
{
  "success": true,
  "status": 0,
  "feature_state": {
    "value_type": "int",
    "value_int": 650
  }
}
```

BOOL:

```json
{
  "feature_state": {
    "value_type": "bool",
    "value_bool": true
  }
}
```

Tốt hơn nữa, nếu service/result có feature identity thì trả:

```text
feature_id
property_id
```

Nếu identity chưa nằm trong result, UI vẫn có thể apply state cho feature đã gửi command vì request context đã biết feature đó.

### UI order

```text
command response authoritative state
→ apply immediately to current feature card

later WebSocket feature.state
→ reconcile/deduplicate
```

WebSocket vẫn là realtime source cho local/sensor changes.

Command HTTP response là fallback đáng tin cậy cho command-origin state.

---

# 20.2. `api.js`

Không cần thay route.

Nhưng `sendCommand()` phải return nguyên response object để caller đọc:

```text
feature_state
```

Không discard/normalize mất authoritative state.

`sendFeatureCommand()` trong `devices.js`:

```js
const result = await this.sendCommand(...);

if (result.feature_state) {
    this.applyCommandFeatureState(feature, result.feature_state);
}
```

Sau đó clear pending.


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

Đây không phải cosmetic UI change; đây là robustness fix cho generic/future feature.

Current v2 property registry đã biết `VALUE`, nên đây **không phải blocker hiện tại** cho
known v2 features, nhưng nên sửa để API dùng chính `feature->value_type` là source of truth.

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

Config scan path hiện đúng cho source mới:

```text
www_src/**/*.js
www_src/**/*.html
```

Nhưng config chỉ có tác dụng khi **Tailwind compiler thực sự được chạy**.

ESP-IDF CMake hiện không chạy Tailwind; nó embed file CSS đã build sẵn.

Do đó implementation checklist phải có bước:

```text
new utility class?
→ grep/check dashboard.css
→ nếu chưa có: regenerate dashboard.css
```

Không coi `tailwind.config.js` là đủ.

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

# 24. File 9 — CSS assets / `CMakeLists.txt`

**Paths:**

```text
components/web_server/www/icons.css
components/web_server/www/dashboard.css
components/web_server/tailwind.input.css
components/web_server/CMakeLists.txt
```

## Icons

Nếu Phase 1 chỉ dùng icon hiện có:

```text
không sửa icons.css
```

Nếu muốn semantic icons mới:

```text
ph-fan
ph-drop
ph-door
...
```

thì `icons.css` bắt buộc phải được update bằng mapping hợp lệ.

Không chỉ add class name trong JavaScript.

## Tailwind CSS

CMake hiện embed:

```text
www/dashboard.css
```

và không compile Tailwind.

Nếu markup mới cần utility chưa có:

```text
regenerate www/dashboard.css
```

sau đó commit generated CSS cùng source changes.

## CMake

Không cần add `feature_ui.js` vào `idf_component_register()`.

`GLOB_RECURSE www_src/*.js` chỉ làm dependency cho generated dashboard.

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
→ preview on input, send once on change/release

GENERIC_VALUE
→ numeric input + explicit Apply
```

Trước khi gửi GENERIC_VALUE:

```text
display input
→ displayToRaw()
→ Number.isFinite(raw)
→ raw >= minimum
→ raw <= maximum
→ (raw - minimum) % step == 0
→ send
```

Validation phải chạy trên **raw integer**, không modulo số float hiển thị.

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

Source/reconciliation order:

```text
1. authoritative command ACK returned by /api/command, khi command-origin
2. WebSocket feature.state, cho realtime/global changes
3. REST detail snapshot, cho initial/resync
4. local preview only while user edits
```

Không giữ optimistic preview làm authoritative state.

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
- không đặt toàn bộ sensor grid trong `aria-live`.
- command/loading feedback dùng live-region riêng.
- metric updates không spam screen reader.

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
www/icons.css (verify only or update if needed)
www/dashboard.css (verify utility coverage)
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
web_command_api.c
api.js
```

### Work

- pending;
- disable control;
- spinner;
- error toast remains global;
- `/api/command` authoritative state được apply nếu có;
- WebSocket event reconcile sau đó;
- degraded WS vẫn cập nhật command-origin state.

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

## T0 — Presentation contract

Backend samples:

```text
light:on_off
light:level
```

Expected:

```text
light:on_off -> toggle
light:level  -> range
```

No semantic-name collision.

---

## T0.1 — Asset availability

For every icon class used by feature UI:

```text
grep/find in www/icons.css
```

Expected all present.

For every new Tailwind utility:

```text
verify in www/dashboard.css
```

or rebuild CSS.

---


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

Device authoritative ACK returns raw:

```text
650
```

Expected HTTP response carries authoritative state and UI final becomes:

```text
65.0°C
```

Test both:

```text
WebSocket healthy
WebSocket temporarily degraded
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

Explicit smoke build:

```bash
python components/web_server/tools/build_webui.py   --source components/web_server/www_src   --dashboard-out /tmp/dashboard.html
```

Verify generated output contains:

```text
// Source: features/feature_ui.js
```

No manual update to embedded HTML asset list is required because generated dashboard is the embedded artifact.

**CSS is different:** `www/dashboard.css` is embedded directly, so new utility classes require the CSS asset to be regenerated/updated.

---

# 38. Recommended commit sequence

```text
commit 1
refactor(web-ui): add composite semantic feature presentation module

commit 2
fix(web-ui-assets): align feature icons and embedded tailwind css

commit 3
feat(web-ui): group feature cards by controls sensors and settings

commit 4
feat(web-ui): add toggle range metric contact and generic renderers

commit 5
perf(web-ui): update feature cards incrementally from websocket state

commit 6
feat(web-command): return authoritative feature state to dashboard

commit 7
feat(web-ui): add pending offline and precise setpoint states

commit 8
test(web-ui): validate semantic feature dashboard scenarios
```

---

# 39. Definition of Done

- [ ] `feature_ui.js` exists and is loaded before `devices.js`.
- [ ] `devices.js` no longer owns card-specific markup.
- [ ] feature groups render correctly.
- [ ] `light:on_off` và `light:level` không bị nhầm renderer.
- [ ] icon classes đều tồn tại trong embedded `icons.css`.
- [ ] Tailwind classes mới đều tồn tại trong embedded `dashboard.css`.
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
- [ ] `/api/command` authoritative feature state được apply.
- [ ] command clamp vẫn đúng khi WebSocket degraded.
- [ ] feature grid không dùng global `aria-live` cho sensor stream.
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
