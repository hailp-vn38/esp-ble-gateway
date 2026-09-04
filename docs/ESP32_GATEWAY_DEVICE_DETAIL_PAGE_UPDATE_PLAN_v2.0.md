# ESP32 BLE Gateway — Device Detail Page Update Plan v2.0

**Repository:** `hailp-vn38/esp-ble-gateway`  
**Branch:** `dev-ws`  
**Reviewed HEAD:** `2260d5f814e11cc4f234a9034ad845266d95446e`  
**Previous reviewed HEAD:** `f09321d2e010e3fbea19d071bc8c83bd90bfbcb8`  
**Current delta:** `dev-ws` is 18 commits ahead of the previous reviewed baseline  
**Date:** 2026-09-04  
**Scope:** Device Detail Web page only, using the current compact-only semantic MCP architecture.

> This document replaces the earlier Device Detail plan that was written against `f09321d2...`.  
> Do not reintroduce `command_executor`, `command_dispatcher`, dynamic MCP tools, dynamic tool names, or raw-command MCP exposure.

---

# 1. Current code baseline after the latest commits

The code has changed substantially since the previous page plan.

The following architecture work is already complete and **must not be implemented again**.

## 1.1 Legacy command stack is gone

The repository has removed:

```text
components/command_executor/
components/command_dispatcher/
device_request_manager
dispatch_result_t routing
legacy dispatcher ACK fallback
```

Device commands now use:

```text
device_command_service
```

as the authoritative command/ACK path.

---

## 1.2 Typed device management already exists

Current component:

```text
components/device_management/
```

already owns typed:

```text
add
edit
delete
snapshot
```

`GET /api/devices` already calls:

```text
device_management_snapshot()
```

instead of the old generic dispatcher.

---

## 1.3 `GET /api/devices` already contains semantic control hints

Current backend response already contains:

```json
{
  "device_id": "...",
  "name": "...",
  "connected": true,
  "ready": true,
  "capabilities": {
    "available": true,
    "state": "ready",
    "revision": 4,
    "feature_count": 2,
    "writable_feature_count": 1
  },
  "controls": [
    {
      "feature_id": "relay_1",
      "semantic_name": "relay",
      "property": "on_off",
      "value_type": "bool",
      "writable": true
    }
  ]
}
```

The backend uses:

```text
mcp_semantic_control_get_hints()
mcp_semantic_control_serialize_hints()
```

and bounds the list using:

```text
MCP_SEMANTIC_CONTROL_HINT_MAX
```

This is already the correct two-call MCP discovery model.

---

## 1.4 MCP is now compact-only

The MCP registry is now exactly:

```text
get_status
list_devices
device_control
```

There is no longer a dynamic/compact surface choice.

The dynamic catalog implementation has already been removed:

```text
mcp_tool_catalog.c       DELETED
mcp_tool_name.c          DELETED
dynamic tools/call       DELETED
dynamic tool naming      DELETED
```

Therefore the Web page must not use terminology or UI behavior based on:

```text
dynamic MCP tools
tool_name
catalog revision
per-command MCP exposure
```

---

## 1.5 Exposure backend is already feature-oriented

Current:

```http
GET /api/mcp/exposures?device_id=X
```

returns:

```json
{
  "device_id": "X",
  "policy_revision": 12,
  "capacity": {...},
  "features": [
    {
      "feature_id": "relay_1",
      "semantic_name": "relay",
      "property": "on_off",
      "value_type": "bool",
      "control_enabled": true,
      "health": "enabled"
    }
  ]
}
```

It no longer returns a legacy:

```json
commands[]
```

array.

Missing feature exposure is already fail-closed:

```json
{
  "control_enabled": false,
  "health": "missing"
}
```

---

## 1.6 Exposure mutation is already feature-oriented

Current compact mutation is already:

```http
PUT /api/mcp/exposures
```

with:

```json
{
  "device_id": "X",
  "feature_id": "relay_1",
  "enabled": true
}
```

and backend calls:

```c
mcp_tool_exposure_set_feature_enabled(
    device_id,
    feature_id,
    enabled);
```

Do not convert `feature_id` back to a raw command in frontend code.

---

# 2. Current Device Detail bug

The backend has migrated, but the frontend Device Detail page is still using the old dynamic-command contract.

This is now the primary cause of the broken **MCP Tools** section.

---

## 2.1 Frontend still expects `data.commands`

Current `mcp_exposure.js` does:

```js
const commands = Array.isArray(data.commands)
    ? data.commands
    : [];
```

But current backend returns:

```text
features[]
```

and no longer returns:

```text
commands[]
```

Therefore the page reaches:

```text
commands.length == 0
```

and renders the MCP section as empty.

---

## 2.2 Frontend still sends `command`

Current frontend toggle sends:

```json
{
  "device_id": "...",
  "command": "...",
  "enabled": true
}
```

Current backend only accepts:

```json
{
  "device_id": "...",
  "feature_id": "...",
  "enabled": true
}
```

Therefore even if rows are manually rendered, the current toggle contract is wrong.

---

## 2.3 Frontend still renders deleted dynamic concepts

Current page still expects fields such as:

```text
command.command
command.label
command.tool_name
command.feature_bound
capacity.enabled
capacity.max_enabled
```

Those concepts no longer match the compact-only architecture.

---

## 2.4 Device Detail still has multiple independent REST owners

Current detail open flow is effectively:

```text
openDetailView()
      |
      v
loadSchema()
      |
      +--> GET /api/devices/schema?device_id=X
      |
      +--> GET /api/mcp/exposures?device_id=X
```

At the same time WebSocket resync logic can call:

```text
GET /api/devices
GET /api/devices/schema?device_id=X
```

and a `device.schema` event can trigger another:

```text
GET /api/devices/schema
then
GET /api/mcp/exposures
```

The generic GET dedupe in `api.js` only deduplicates identical URLs. It cannot merge three different REST resources.

---

## 2.5 Backend `/api/devices` data is discarded by frontend mapping

Current backend returns:

```text
capabilities
controls
controls_truncated
```

but current `api.getDevicesSnapshot()` maps only:

```text
id
mac
addrType
customName
connected
ready
status
rssi
```

The new fields are discarded.

This is not the direct cause of MCP Detail failure, but the frontend model is behind the current backend contract.

---

# 3. Final target architecture

The final Device Detail flow should be:

```text
User opens Device Detail
        |
        v
GET /api/devices/detail?device_id=X
        |
        +--> device_store / device identity
        +--> BLE runtime status
        +--> device_schema
        +--> device_state
        +--> device_template
        +--> mcp_tool_exposure feature policy
        |
        v
one bounded detail snapshot
        |
        +--> Header
        +--> Connection
        +--> Schema summary
        +--> Feature controls
        +--> AI / MCP Control
```

No separate MCP GET is required for page rendering.

---

# 4. WebSocket target flow

## Feature state

```text
feature.state
    |
    v
update currentDetail.features[n].state
    |
    v
re-render affected feature
```

REST request count:

```text
0
```

---

## Connection state

```text
device.connection
    |
    v
update selected device + currentDetail.connection
    |
    v
re-render status/offline controls
```

REST request count:

```text
0
```

---

## Schema change

```text
device.schema
    |
    v
scheduleDetailReload()
    |
    v
GET /api/devices/detail?device_id=X
```

REST request count:

```text
1
```

with coalescing when several schema events arrive together.

---

# 5. Final Device Detail API contract

Add:

```http
GET /api/devices/detail?device_id=<device_id>
```

Recommended response:

```json
{
  "device_id": "AC:27:6E:CC:F2:26",
  "name": "TEST",
  "ble_addr": "AC:27:6E:CC:F2:26",
  "ble_addr_type": 0,

  "connection": {
    "connected": true,
    "ready": true
  },

  "schema": {
    "state": "ready",
    "revision": 8,
    "has_committed": true,
    "updated_at_ms": 192033
  },

  "features": [
    {
      "feature_id": "relay_1",
      "feature_type": 1,
      "property_id": 1,

      "semantic": {
        "name": "relay",
        "property": "on_off",
        "value_type": "bool",
        "primary_property": 1
      },

      "control": {
        "writable": true,
        "write_command": "set_relay"
      },

      "state": {
        "valid": true,
        "value_bool": false,
        "updated_at_ms": 192033
      },

      "mcp_control": {
        "eligible": true,
        "enabled": true,
        "health": "enabled"
      }
    }
  ],

  "mcp": {
    "tool": "device_control",
    "policy_revision": 12
  }
}
```

---

# 6. Why `write_command` remains in the Web detail snapshot

The MCP architecture must not expose or depend on raw commands.

However the existing Device Detail feature controls currently submit:

```http
POST /api/command
```

which still uses an internally validated device command.

To keep this page update focused and avoid creating a second semantic Web command execution stack:

```text
feature.control.write_command
```

may remain in the same-origin Web Admin detail snapshot.

Rules:

```text
- never display write_command in AI/MCP Control UI
- never use write_command to identify MCP permission
- never send write_command to /api/mcp/exposures
- device_control MCP remains feature_id based
- /api/command remains validated by device_command_service
```

This preserves the existing Web feature controls while removing raw command concepts from the MCP UI.

---

# 7. INT feature contract

For writable INT controls:

```json
{
  "control": {
    "writable": true,
    "write_command": "set_level",
    "minimum": 0,
    "maximum": 100,
    "step": 5
  }
}
```

This replaces frontend dependence on:

```text
currentTools[]
writable_tool_index
```

for range rendering.

After migration, frontend does not need to retain the complete schema `tools[]` array.

---

# 8. MCP control health contract

Recommended normalized states:

```text
enabled
needs_review
orphaned
missing
read_only
unsupported
```

Examples:

## Writable + approved

```json
{
  "eligible": true,
  "enabled": true,
  "health": "enabled"
}
```

## User disabled

```json
{
  "eligible": true,
  "enabled": false,
  "health": "enabled"
}
```

## Capability changed

```json
{
  "eligible": true,
  "enabled": false,
  "health": "needs_review"
}
```

## Missing policy record

```json
{
  "eligible": false,
  "enabled": false,
  "health": "missing"
}
```

## Read-only feature

```json
{
  "eligible": false,
  "enabled": false,
  "health": "read_only"
}
```

No missing/unknown policy state may default to enabled.

---

# 9. Phase 0 — Characterize the current page before changing it

## Goal

Freeze the current failure so the update proves it fixes the correct problem.

## Files

No production change required.

Optional report:

```text
docs/reports/DEVICE_DETAIL_PAGE_BASELINE_2026-09-04.md
```

## Record

Open a ready device and record browser Network requests:

```text
/api/devices
/api/devices/schema?device_id=...
/api/mcp/exposures?device_id=...
/ws/events
```

Record the `/api/mcp/exposures` JSON.

Verify current response has:

```text
features[]
```

and does not have:

```text
commands[]
```

Record browser console errors if present.

## Tests

Manually execute:

```http
GET /api/mcp/exposures?device_id=X
```

Expected current backend:

```text
200
features[] present
policy_revision present
```

Then:

```http
PUT /api/mcp/exposures
{
  "device_id": "X",
  "feature_id": "relay_1",
  "enabled": false
}
```

Expected:

```text
success == true
```

## Checklist

- [x] Current HEAD recorded as `2260d5f814e11cc4f234a9034ad845266d95446e`.
- [ ] Browser request count recorded. (UNVERIFIED: no browser session or gateway URL available.)
- [x] Exposure response confirmed feature-oriented from current backend contract.
- [ ] Current blank MCP UI reproduced. (UNVERIFIED: no browser session or gateway URL available.)
- [x] No production fix made before baseline capture.

---

# 10. Phase 1 — Fix MCP UI contract immediately ✅ DONE (2026-09-04)

This phase fixes the broken UI using the backend that already exists.

It is deliberately designed so its renderer can later be reused by the single detail snapshot.

## Files to modify

```text
components/web_server/www_src/dashboard/js/features/mcp_exposure.js
components/web_server/www_src/dashboard/views/device_detail.html
components/web_server/www_src/dashboard/js/core/i18n.js
components/web_server/www_src/dashboard/shell.html   # only if JS module is renamed
```

## Recommended rename

Rename:

```text
mcp_exposure.js
```

to:

```text
mcp_control.js
```

and rename global:

```js
mcpTools
```

to:

```js
mcpControls
```

because the page no longer manages MCP tools.

## New frontend model

Use feature objects:

```js
{
    feature_id,
    semantic_name,
    property,
    value_type,
    control_enabled,
    health
}
```

Do not use:

```js
command.command
command.label
command.tool_name
command.feature_bound
```

## Temporary loader

During this phase only, the renderer may still load:

```text
GET /api/mcp/exposures?device_id=X
```

but it must read:

```js
data.features
```

not:

```js
data.commands
```

## Toggle

Use:

```js
async setFeatureEnabled(deviceId, featureId, enabled) {
    return api.request('/api/mcp/exposures', {
        method: 'PUT',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({
            device_id: deviceId,
            feature_id: featureId,
            enabled
        })
    });
}
```

## Remove from UI

Delete rendering for:

```text
tool_name
feature_bound badge
dynamic MCP capacity
max_enabled
raw command identity
```

## HTML

Rename:

```text
MCP Tools
```

to:

```text
AI / MCP Control
```

Description:

```text
Choose which device features AI clients may control.
```

Replace IDs:

```text
mcp-tools-card       -> mcp-control-card
mcp-tool-rows        -> mcp-feature-controls
```

Delete:

```text
mcp-capacity-text
```

## Tests

Use an exposure fixture containing one feature:

```json
{
  "features": [
    {
      "feature_id": "relay_1",
      "semantic_name": "relay",
      "property": "on_off",
      "value_type": "bool",
      "control_enabled": true,
      "health": "enabled"
    }
  ]
}
```

Assert:

```text
one row rendered
relay displayed
relay_1 displayed
switch checked
no tool_name shown
no command shown
```

Toggle OFF.

Assert request body exactly contains:

```json
{
  "device_id": "...",
  "feature_id": "relay_1",
  "enabled": false
}
```

and does not contain:

```text
command
tool_name
```

## Checklist

- [x] `data.commands` removed.
- [x] Toggle sends `feature_id`.
- [x] MCP rows display semantic features.
- [x] Capacity UI removed.
- [x] Raw commands are not shown in MCP UI.
- [x] Page MCP section works before the single-snapshot migration. (Static assemble/syntax validation passed; live browser validation unavailable.)

---

# 11. Phase 2 — Add the single Device Detail backend snapshot ✅ DONE (2026-09-04)

## Goal

Replace the separate schema + exposure reads used to render the detail page.

## Files to create

```text
components/web_server/web_device_detail_api.c
```

## Files to modify

```text
components/web_server/CMakeLists.txt
components/web_server/web_modules.h
components/web_server/web_gateway_api.c
components/web_server/web_server.c
components/web_server/test/CMakeLists.txt
components/web_server/test/test_web_api_baseline.c
```

## Route registration

Add:

```c
esp_err_t web_device_detail_api_register(httpd_handle_t server);
```

Register from:

```c
web_gateway_api_register()
```

after the device inventory API.

## Route budget

Current gateway budget comment describes 31 routes.

Adding:

```text
GET /api/devices/detail
```

temporarily increases the route count by one.

Keep:

```text
WEB_GATEWAY_MAX_URI_HANDLERS = 34
```

and update the route-budget comment.

## Snapshot read order

```text
1. validate device_id
2. gateway_events_current_seq()
3. device_store_get()
4. ble_central_get_device_status()
5. device_schema_get()
6. device_state_snapshot()
7. device_template semantic resolution per feature
8. mcp_tool_exposure_get_feature() per feature
9. mcp_tool_exposure_get_policy_revision()
10. serialize once
11. set X-Gateway-Event-Seq
12. send JSON
```

## Memory rule

Do not copy the schema twice.

If `device_schema_snapshot_t` / `device_state_snapshot_t` are large enough to threaten the 12 KB HTTPD stack, allocate request workspace with:

```text
gw_mem_calloc(..., GW_MEM_EXTERNAL_PREFERRED)
```

and release before returning.

Do not retain the snapshot after the HTTP request.

## Feature serialization

For each committed feature, attach:

```text
semantic metadata
Web control metadata
current cached state
MCP control policy
```

Do not create a second parallel:

```text
commands[]
```

array.

## Tests

### Unknown device

Expected:

```text
404 device_not_found
```

### Device with no committed schema

Expected:

```json
{
  "schema": {
    "has_committed": false
  },
  "features": []
}
```

HTTP:

```text
200
```

### BOOL feature

Assert:

```text
semantic.value_type == bool
state.value_bool present
control.writable correct
mcp_control present
```

### INT feature

Assert:

```text
minimum
maximum
step
```

are copied from the resolved writable capability.

### Missing exposure

Expected:

```text
mcp_control.enabled == false
mcp_control.health == missing
```

### Response safety

Assert detail response does not contain MCP dynamic concepts:

```text
tool_name
catalog_revision
commands[]
max_enabled
```

## Checklist

- [x] One detail endpoint exists.
- [x] One schema snapshot per request.
- [x] Feature state included.
- [x] MCP feature policy included.
- [x] INT constraints included.
- [x] Event sequence header included.
- [x] No dynamic MCP metadata.

> Validation: firmware build passed. `test/run_tests.sh` reached 370 tests
> (359 pass, 11 pre-existing MCP endpoint failures) before being stopped after
> the USB serial device became unavailable; no Phase 2-specific failure was
> observed.

---

# 12. Phase 3 — Upgrade exposure PUT response ✅ DONE (2026-09-04)

The feature-oriented mutation is already correct. Do not rewrite its input path.

Only improve its response so frontend does not need a follow-up GET.

## File

```text
components/web_server/web_exposure_api.c
```

## Current behavior

Current success response:

```json
{
  "success": true
}
```

## Target success response

After:

```c
mcp_tool_exposure_set_feature_enabled(...)
```

read the resulting feature exposure and return:

```json
{
  "success": true,
  "device_id": "X",
  "feature_id": "relay_1",
  "control_enabled": false,
  "health": "enabled",
  "policy_revision": 13
}
```

## Important

Do not add back:

```text
command
tool_name
catalog_revision
confirm_destructive dynamic-tool logic
```

## Tests

Toggle false.

Assert:

```text
control_enabled == false
policy_revision incremented
```

Toggle true.

Assert:

```text
control_enabled == true
```

Missing feature remains fail closed.

## Checklist

- [x] PUT remains `feature_id` based.
- [x] No raw command input.
- [x] Response can update local page state.
- [x] No follow-up exposure GET needed after toggle.

> Validation: firmware build passed. `test/run_tests.sh --skip-build` flashed
> and ran the suite; it was stopped after the serial test runner stalled, with
> the same 11 pre-existing MCP endpoint failures and no exposure API failure.

---

# 13. Phase 4 — Add frontend detail API ✅ DONE (2026-09-04)

## File

```text
components/web_server/www_src/dashboard/js/core/api.js
```

## Add

```js
async getDeviceDetailSnapshot(deviceId) {
    const {data, eventSeq} = await this.requestWithMeta(
        `/api/devices/detail?device_id=${encodeURIComponent(deviceId)}`
    );
    return {
        detail: data,
        eventSeq
    };
}
```

## Preserve current inventory metadata

Current backend `/api/devices` already supplies semantic summaries.

Update both:

```js
getDevicesSnapshot()
getDevices()
```

to preserve:

```js
capabilities: device.capabilities || null,
controls: Array.isArray(device.controls) ? device.controls : [],
controlsTruncated: device.controls_truncated === true
```

These are inventory hints only; they are not the authority for Device Detail policy state.

## Keep

```js
_pendingGetRequests
```

for exact-URL concurrent GET dedupe.

## Tests

Assert mapping does not discard:

```text
capabilities
controls
controls_truncated
```

Assert duplicate concurrent:

```text
/api/devices/detail?device_id=X
```

shares one in-flight request.

## Checklist

- [x] `getDeviceDetailSnapshot()` added.
- [x] Device inventory semantic hints retained.
- [x] Existing GET dedupe retained.

> Validation: dashboard assemble and API module syntax passed. The requested
> test runner was started and flashed the test app, but was stopped after the
> serial runner stalled; 116 tests had passed at that point.
- [ ] No MCP-specific command mapping added to `api.js`.

---

# 14. Phase 5 — Replace `loadSchema()` with `loadDetail()`

## File

```text
components/web_server/www_src/dashboard/js/features/devices.js
```

## Remove detail-page state

After migration remove:

```text
schemaLoadId
currentTools
```

and replace with:

```js
detailLoadId: 0,
detailLoadPromise: null,
detailLoadedDeviceId: null,
detailReloadRequested: false,
currentDetail: null,
currentFeatures: [],
```

## `openDetailView()`

Target:

```js
openDetailView(dev, updateRoute = true) {
    state.selectedDeviceDetail = dev;
    this.currentDetail = null;
    this.currentFeatures = [];

    this.renderDeviceHeader(dev);
    this.renderConnectionState(dev);
    this.renderDetailLoading();

    i18n.applyTranslations();
    nav.switchTab('device-detail', updateRoute);

    void this.loadDetail(dev.id);
}
```

Remove:

```js
this.loadSchema(dev);
```

## `loadDetail()`

Use one request:

```js
async loadDetail(deviceId) {
    if (this.detailLoadPromise &&
        this.detailLoadedDeviceId === deviceId) {
        return this.detailLoadPromise;
    }

    const loadId = ++this.detailLoadId;
    this.detailLoadedDeviceId = deviceId;

    const operation = (async () => {
        const result = await api.getDeviceDetailSnapshot(deviceId);

        if (loadId !== this.detailLoadId) return false;
        if (state.selectedDeviceDetail?.id !== deviceId) return false;

        this.currentDetail = result.detail;
        this.currentFeatures = result.detail.features || [];

        this.applyDetailSnapshot(result.detail);
        this.reconcileFeatureEventsAfterDetailSnapshot(
            deviceId,
            result.eventSeq
        );
        return true;
    })();

    this.detailLoadPromise = operation;

    try {
        return await operation;
    } finally {
        if (this.detailLoadPromise === operation) {
            this.detailLoadPromise = null;
        }
    }
}
```

## Important event cursor rule

Normal Device Detail load must **not** restart the global event stream with:

```js
events.goLive(...)
```

The list/page lifecycle already owns global WS snapshot activation.

The detail `eventSeq` is used only to reconcile cached:

```text
feature.state
```

events newer than the detail snapshot.

## Remove old parallel MCP load

Delete detail lifecycle calls to:

```text
mcpTools.loadDevice()
mcpControls.loadDevice()
```

MCP UI should render from:

```text
currentDetail.features[].mcp_control
```

## Checklist

- [ ] Open detail calls one detail endpoint.
- [ ] `loadSchema()` is no longer the Device Detail owner.
- [ ] `currentTools` removed.
- [ ] No separate MCP load.
- [ ] Global WS cursor is not reset by normal detail load.

---

# 15. Phase 6 — Adapt feature rendering to normalized detail features

## File

```text
components/web_server/www_src/dashboard/js/features/devices.js
```

## Current dependency to remove

Current renderer uses:

```text
feature.writable_tool_index
tools[feature.writable_tool_index]
```

to find:

```text
minimum
maximum
step
```

After detail migration use:

```js
feature.control
```

instead.

## BOOL

Use:

```text
feature.control.writable
feature.control.write_command
feature.state.value_bool
```

## INT

Use:

```text
feature.control.minimum
feature.control.maximum
feature.control.step
feature.control.write_command
```

## Existing Web command execution

Keep:

```text
POST /api/command
```

for Web device feature control in this page update.

Do not route Web feature buttons through MCP.

## Tests

BOOL toggle:

```text
POST /api/command exactly once
```

INT slider:

```text
uses minimum/maximum/step from feature.control
```

Offline:

```text
control disabled
no POST
```

## Checklist

- [ ] No `currentTools`.
- [ ] No `writable_tool_index` lookup in frontend.
- [ ] Existing Web device controls still work.
- [ ] MCP permission remains independent from Web command transport.

---

# 16. Phase 7 — Make `mcpControls` a pure renderer + mutation controller

## File

Recommended:

```text
components/web_server/www_src/dashboard/js/features/mcp_control.js
```

## Responsibility

`mcpControls` must own only:

```text
render feature MCP permission
send feature permission mutation
apply mutation result locally
```

It must not own:

```text
Device Detail GET lifecycle
schema GET lifecycle
exposure GET lifecycle
device inventory lifecycle
```

## Render input

```js
mcpControls.render(currentDetail);
```

Filter/display features using:

```text
feature.feature_id
feature.semantic
feature.mcp_control
```

## Toggle

On success update:

```js
feature.mcp_control.enabled = response.control_enabled;
feature.mcp_control.health = response.health;
currentDetail.mcp.policy_revision = response.policy_revision;
```

No post-toggle GET.

## Health behavior

```text
enabled:
    toggle interactive

needs_review:
    toggle disabled
    show "Needs review"

orphaned:
    toggle disabled
    show "Unavailable"

missing:
    toggle disabled
    show "Unavailable"

read_only:
    no write switch

unsupported:
    no write switch
```

## Checklist

- [ ] Pure renderer/mutation controller.
- [ ] No GET method inside MCP UI module.
- [ ] No command field.
- [ ] No dynamic capacity.
- [ ] No tool identity.

---

# 17. Phase 8 — Update Device Detail HTML and i18n

## Files

```text
components/web_server/www_src/dashboard/views/device_detail.html
components/web_server/www_src/dashboard/js/core/i18n.js
components/web_server/www_src/dashboard/shell.html
```

## Replace section

Old:

```text
MCP Tools
Choose commands exposed to MCP clients.
```

New:

```text
AI / MCP Control
Choose which device features AI clients may control.
```

## IDs

Recommended:

```text
mcp-tools-card      -> mcp-control-card
mcp-tool-rows       -> mcp-feature-controls
```

Remove:

```text
mcp-capacity-text
```

## Required i18n concepts

Add/update:

```text
device_detail.mcp_control
device_detail.mcp_control_desc
device_detail.mcp_enabled
device_detail.mcp_disabled
device_detail.mcp_needs_review
device_detail.mcp_unavailable
device_detail.mcp_missing
device_detail.mcp_updated
```

Remove UI dependencies on:

```text
mcp_feature_bound
mcp_capacity
mcp_row_semantic_desc
dynamic tool wording
```

if no longer used elsewhere.

## Checklist

- [ ] Page terminology matches compact-only MCP.
- [ ] No "choose commands exposed to MCP clients".
- [ ] No dynamic capacity text.
- [ ] Loading/error states remain accessible.

---

# 18. Phase 9 — Fix WebSocket / resync ownership

## File

```text
components/web_server/www_src/dashboard/js/features/devices.js
```

## `feature.state`

Continue caching state for all devices.

For selected detail:

```text
update currentDetail feature
render affected feature
```

Do not call:

```text
loadDetail
loadSchema
MCP GET
```

REST count:

```text
0
```

## `device.connection`

Update:

```text
state.connectedDevices
state.selectedDeviceDetail
currentDetail.connection
```

and rerender status.

REST count:

```text
0
```

## `device.schema`

Replace:

```js
this.loadSchema(state.selectedDeviceDetail, true);
```

with:

```js
this.scheduleDetailReload('device.schema');
```

## Coalescing

Allow at most:

```text
one active detail GET
one queued follow-up reload
```

Example:

```js
scheduleDetailReload(reason) {
    if (!state.selectedDeviceDetail) return;

    if (this.detailLoadPromise) {
        this.detailReloadRequested = true;
        return;
    }

    void this.reloadDetailCoalesced(reason);
}
```

## Global `resync:required`

Global resync may still require:

```text
GET /api/devices
```

If a selected device remains after the inventory snapshot, then perform:

```text
GET /api/devices/detail
```

once.

Do not additionally request:

```text
/api/devices/schema
/api/mcp/exposures
```

## Checklist

- [ ] Feature delta = zero REST.
- [ ] Connection delta = zero REST.
- [ ] Schema structural event = one coalesced detail GET.
- [ ] Global resync = inventory + optional one detail GET.
- [ ] No independent MCP reload owner.

---

# 19. Phase 10 — Remove now-unused exposure GET from the page

After Phase 5–9 pass, Device Detail no longer needs:

```http
GET /api/mcp/exposures
```

## File

```text
components/web_server/web_exposure_api.c
```

## Preferred cleanup

If code search confirms no other consumer needs the GET endpoint, remove:

```text
exposure_get_handler()
HTTP_GET /api/mcp/exposures
capacity serialization
feature serialization duplicate
```

Keep:

```text
PUT /api/mcp/exposures
```

as the feature permission mutation endpoint.

This reduces duplicate schema traversal and one registered URI handler.

## If GET compatibility must be retained

It may remain temporarily, but:

```text
Device Detail MUST NOT call it.
```

Do not block the page migration on deleting a public compatibility GET.

## Zero-consumer check

Search:

```bash
git grep -n "/api/mcp/exposures" -- \
  components/web_server/www_src \
  components/web_server/test
```

Expected frontend GET consumer:

```text
0
```

PUT consumer is expected.

## Checklist

- [ ] Device Detail has zero exposure GET dependency.
- [ ] GET handler deleted if zero external requirement.
- [ ] PUT feature mutation retained.
- [ ] Route-budget comment updated if handler removed.

---

# 20. Phase 11 — Remove old schema-detail frontend path

The schema API can remain a backend diagnostics API and refresh trigger, but the Device Detail renderer must stop depending on GET schema.

## Frontend candidates to remove if no other consumers

From:

```text
api.js
```

remove:

```text
getDeviceSchema()
getDeviceSchemaSnapshot()
```

only after code search proves they are unused.

From:

```text
devices.js
```

remove:

```text
loadSchema()
_applySchemaSnapshot()
schemaLoadId
currentTools
```

Retain:

```text
refreshDeviceSchema()
```

because the refresh button still needs:

```http
POST /api/devices/schema/refresh
```

After refresh:

```text
POST accepted
    ->
device.schema WS event
    ->
one detail reload
```

Do not immediately issue another schema GET from the button handler.

## Tests

Press Refresh.

Expected:

```text
POST /api/devices/schema/refresh
```

Then when schema event arrives:

```text
one GET /api/devices/detail
```

No:

```text
GET /api/devices/schema
GET /api/mcp/exposures
```

## Checklist

- [ ] GET schema no longer drives Device Detail.
- [ ] Refresh POST remains.
- [ ] Refresh completion is event-driven.
- [ ] No polling introduced.

---

# 21. Phase 12 — Generated Web UI source of truth

Current firmware build source is:

```text
components/web_server/www_src/
```

Build:

```text
www_src
  -> tools/build_webui.py
  -> build/.../dashboard.html
  -> gzip
  -> EMBED_FILES
```

Do not implement the new page by directly editing a stale generated:

```text
components/web_server/www/dashboard.html
```

## Build validation

Run a clean build after JS/HTML changes.

Confirm generated dashboard contains:

```text
AI / MCP Control
mcp-feature-controls
```

and does not contain old Device Detail dependency on:

```text
mcp-capacity-text
data.commands
command.tool_name
```

## Checklist

- [ ] `www_src` is the only edited source of Web UI behavior.
- [ ] Generated dashboard rebuilt.
- [ ] Firmware embeds the rebuilt gzip.
- [ ] No stale browser asset from previous firmware.

---

# 22. Phase 13 — Test matrix

## 22.1 Backend detail API

Required tests:

```text
unknown device -> 404
offline device -> 200 valid identity + connection=false
ready device -> 200
schema not committed -> features=[]
BOOL state serialization
INT state serialization
BOOL writable control
INT min/max/step
read-only feature
unsupported semantic mapping
MCP exposure enabled
MCP user-disabled
MCP needs_review
MCP orphaned
MCP missing -> fail closed
policy_revision included
X-Gateway-Event-Seq included
no commands[] MCP list
no tool_name
no catalog_revision
no dynamic capacity
```

---

## 22.2 Exposure mutation

```text
enable feature
disable feature
unknown feature
read-only feature
schema not ready
response returns current control_enabled
response returns health
response returns policy_revision
```

---

## 22.3 Frontend MCP renderer

```text
semantic feature renders
enabled switch checked
disabled switch unchecked
needs_review switch disabled
missing switch disabled
no raw command text
no tool_name text
PUT uses feature_id
no post-toggle GET
```

---

## 22.4 Detail lifecycle

Opening a detail page:

Expected:

```text
1 x GET /api/devices/detail?device_id=X
```

Not expected:

```text
GET /api/devices/schema?device_id=X
GET /api/mcp/exposures?device_id=X
```

---

## 22.5 WebSocket

`feature.state`:

```text
REST count = 0
```

`device.connection`:

```text
REST count = 0
```

`device.schema`:

```text
detail GET count = 1
```

Burst of schema events while GET active:

```text
max concurrent detail GET = 1
max queued follow-up = 1
```

---

## 22.6 Existing feature control regression

BOOL button:

```text
POST /api/command
BLE send = 1
ACK
feature.state update
```

INT slider:

```text
valid min/max/step
POST /api/command
BLE send = 1
```

The Device Detail page refactor must not break Web device control while fixing MCP permission UI.

---

## 22.7 MCP integration regression

MCP surface:

```text
tools/list == exactly 3
```

Names:

```text
get_status
list_devices
device_control
```

`list_devices` still returns:

```text
capabilities
controls[]
```

MCP SET enabled feature:

```text
success
```

Disable same feature from Web.

MCP SET again:

```text
denied
BLE send = 0
```

Re-enable.

MCP SET:

```text
success
```

---

# 23. Request-count acceptance criteria

## Device list page

Normal load may request:

```text
/api/devices
settings/status resources
/ws/events
```

No detail resource before a device is opened.

---

## Open Device Detail

Additional request:

```text
GET /api/devices/detail?device_id=X
```

Exactly one active request for that detail snapshot.

---

## Toggle MCP permission

Expected:

```text
PUT /api/mcp/exposures
```

No follow-up GET.

---

## Feature state event

Expected:

```text
0 REST
```

---

## Connection event

Expected:

```text
0 REST
```

---

## Schema event

Expected:

```text
1 detail GET
```

---

# 24. ESP32-S3 memory acceptance

Measure before and after opening Device Detail:

```text
free internal heap
minimum free internal heap
largest internal block
free PSRAM
HTTPD stack high-water mark
```

Stress sequence:

```text
open/close detail 50 times
100 feature.state events
30 device.connection transitions
20 schema reload events
50 MCP enable/disable mutations
```

Pass conditions:

```text
no progressive heap loss
no stack overflow
no request storm
no HTTP timeout caused by parallel detail APIs
no cJSON workspace retained after response
```

---

# 25. Files changed by the final page update

## New

```text
components/web_server/web_device_detail_api.c
```

## Modify

```text
components/web_server/CMakeLists.txt
components/web_server/web_modules.h
components/web_server/web_gateway_api.c
components/web_server/web_server.c
components/web_server/web_exposure_api.c

components/web_server/www_src/dashboard/js/core/api.js
components/web_server/www_src/dashboard/js/core/i18n.js
components/web_server/www_src/dashboard/js/features/devices.js
components/web_server/www_src/dashboard/js/features/mcp_exposure.js
components/web_server/www_src/dashboard/views/device_detail.html
components/web_server/www_src/dashboard/shell.html

components/web_server/test/CMakeLists.txt
components/web_server/test/test_web_api_baseline.c
```

If renamed:

```text
mcp_exposure.js
    ->
mcp_control.js
```

---

# 26. Files that should not need architecture changes for this page update

Do not modify these to solve the Device Detail UI problem unless a test proves a real defect:

```text
components/mcp_endpoint/mcp_registry.c
components/mcp_endpoint/mcp_tools.c
components/mcp_endpoint/mcp_device_control.c
components/mcp_endpoint/mcp_semantic_control.c

components/device_command_service/
components/device_management/
components/device_store/
components/device_schema/

main/main.c
```

The core MCP/control architecture is already migrated.

The page must adapt to it, not add another execution layer.

---

# 27. Zero-reference gates

After final migration:

```bash
git grep -n "data.commands" -- components/web_server/www_src
git grep -n "command.tool_name" -- components/web_server/www_src
git grep -n "command.feature_bound" -- components/web_server/www_src
git grep -n "mcp-capacity-text" -- components/web_server/www_src
git grep -n "mcpTools.loadDevice" -- components/web_server/www_src
git grep -n "loadExposures" -- components/web_server/www_src
```

Expected:

```text
0 matches
```

For Device Detail GET dependencies:

```bash
git grep -n "getDeviceSchemaSnapshot" -- components/web_server/www_src
```

Expected:

```text
0 matches
```

if no other UI flow requires it.

Verify deleted architectures remain deleted:

```bash
git grep -nE \
'command_executor|command_dispatcher|mcp_tool_catalog|mcp_tool_name_generate' \
-- components main test
```

Expected production-code matches:

```text
0
```

Do not solve page issues by restoring any of them.

---

# 28. Recommended commit sequence

```text
1. fix(web-ui): render semantic MCP feature permissions

2. feat(web): add unified device detail snapshot endpoint

3. feat(web): return updated feature policy from exposure mutation

4. refactor(web-ui): migrate device detail to unified snapshot

5. refactor(web-ui): remove schema tool-array dependency from feature renderer

6. refactor(web-ui): make MCP control renderer fetch-free

7. fix(web-ui): coalesce detail reloads on websocket schema events

8. refactor(web): remove unused exposure GET from device detail flow

9. refactor(web-ui): remove obsolete schema detail loader

10. test(web): qualify unified device detail and MCP feature control
```

Each commit should build before continuing.

---

# 29. Definition of Done

The page update is complete only when all conditions below are true.

## Architecture

- [ ] Current compact-only MCP architecture remains unchanged.
- [ ] No command dispatcher/executor restored.
- [ ] No dynamic MCP catalog restored.
- [ ] Device Detail has one REST snapshot owner.

## MCP UI

- [ ] Heading is `AI / MCP Control`.
- [ ] UI renders semantic `feature_id`, not raw command tools.
- [ ] Toggle uses `feature_id`.
- [ ] Missing policy fails closed.
- [ ] No tool names or dynamic capacity are shown.
- [ ] No separate exposure GET is required to render the page.

## REST lifecycle

- [ ] Open detail = one detail GET.
- [ ] MCP toggle = one PUT and no GET.
- [ ] Feature WS event = zero REST.
- [ ] Connection WS event = zero REST.
- [ ] Schema WS event = one coalesced detail GET.

## Feature controls

- [ ] Existing BOOL Web control still works.
- [ ] Existing INT Web control still works.
- [ ] `minimum/maximum/step` are available without `currentTools[]`.
- [ ] `/api/command` remains the existing Web device-control transport.

## MCP execution

- [ ] `tools/list` remains exactly 3.
- [ ] `list_devices` retains semantic control hints.
- [ ] Web disable blocks MCP SET.
- [ ] Denied MCP SET sends zero BLE commands.

## ESP32-S3

- [ ] No progressive heap loss.
- [ ] No HTTPD stack overflow.
- [ ] No duplicate schema cJSON responses during normal detail open.
- [ ] No request storm during WebSocket resync.

---

# 30. Final flow

```text
                         DEVICE LIST
                             |
                             | GET /api/devices
                             | capabilities + control hints
                             v
                       user opens device
                             |
                             v
                 GET /api/devices/detail
                             |
          +------------------+-------------------+
          |                  |                   |
          v                  v                   v
     device_schema      device_state       feature MCP policy
          |                  |                   |
          +------------------+-------------------+
                             |
                             v
                    one detail snapshot
                             |
             +---------------+----------------+
             |               |                |
             v               v                v
         Features         State         AI / MCP Control
             |                                |
             |                                |
             |                        PUT feature_id
             |                                |
             v                                v
       POST /api/command             mcp_tool_exposure
             |
             v
   device_command_service


WebSocket:
feature.state      -> local state patch
device.connection  -> local connection patch
device.schema      -> one coalesced detail reload


MCP:
tools/list         -> exactly 3
list_devices       -> controls[]
device_control     -> describe/read/set
```

This is the target Device Detail architecture for the current `dev-ws` codebase at `2260d5f814e11cc4f234a9034ad845266d95446e`.
