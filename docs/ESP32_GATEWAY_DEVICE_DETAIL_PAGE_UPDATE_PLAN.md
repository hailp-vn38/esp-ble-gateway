# ESP32 BLE Gateway — Device Detail Page Update Plan

**Repository:** `hailp-vn38/esp-ble-gateway`  
**Target branch:** `dev-ws`  
**Reviewed HEAD:** `f09321d2e010e3fbea19d071bc8c83bd90bfbcb8`  
**Target:** ESP32-S3, Web UI + BLE + MCP compact semantic control  
**Scope:** Chỉ cập nhật Device Detail page theo kiến trúc semantic compact đề xuất. Không giữ UI quản lý MCP theo raw command/dynamic tool.

---

## 1. Mục tiêu

Cập nhật Device Detail page để:

1. Chỉ tải **một detail snapshot** cho mỗi lần mở hoặc structural resync.
2. Hiển thị MCP theo **semantic feature permission**, không theo raw command.
3. Toggle quyền MCP bằng `feature_id`, không dùng `command`.
4. Không reload REST khi chỉ có `feature.state` WebSocket event.
5. Khi schema thay đổi, chỉ reload **một detail snapshot**.
6. Loại bỏ các dữ liệu dynamic MCP không còn ý nghĩa trong compact mode khỏi Device Detail.
7. Fail closed khi trạng thái MCP feature không tồn tại hoặc không hợp lệ.
8. Giảm cJSON allocation, số HTTP request và copy schema trên ESP32-S3.
9. Không tạo thêm generic abstraction thay cho flow hiện tại.

---

## 2. Kiến trúc đích

### 2.1 Device Detail load

```text
User opens Device Detail
        |
        v
GET /api/devices/detail?device_id=<id>
        |
        +--> device_store
        +--> ble_central status
        +--> device_schema snapshot
        +--> device_state snapshot
        +--> device_template semantic mapping
        +--> mcp_tool_exposure feature policy
        |
        v
one bounded JSON snapshot
        |
        +--> header
        +--> connection status
        +--> schema status
        +--> feature controls
        +--> AI / MCP Control
```

Không còn flow:

```text
GET /api/devices/schema
GET /api/mcp/exposures
GET /api/devices
```

đồng thời chỉ để dựng một Device Detail page.

### 2.2 WebSocket update

```text
feature.state
    |
    v
update local feature state only
    |
    v
re-render affected feature
```

Không gọi REST.

```text
device.connection
    |
    v
update local connection status only
```

Không gọi REST nếu device vẫn tồn tại.

```text
device.schema
    |
    v
GET /api/devices/detail?device_id=<id>
    |
    v
replace structural detail snapshot
```

Chỉ một request.

---

## 3. Public API mới cho Device Detail

### 3.1 Endpoint

```http
GET /api/devices/detail?device_id=<device_id>
```

Mục đích: trả một snapshot đầy đủ, nhất quán, bounded để dựng Device Detail page.

### 3.2 Response contract

```json
{
  "device_id": "AC:27:6E:CC:F2:26",
  "name": "Living Room Relay",
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
      "semantic": {
        "name": "relay",
        "property": "on_off",
        "value_type": "bool"
      },
      "writable": true,
      "state": {
        "valid": true,
        "value_bool": false,
        "updated_at_ms": 192033
      },
      "mcp_control": {
        "enabled": true,
        "health": "enabled"
      }
    }
  ],
  "mcp": {
    "surface_mode": "compact",
    "tool": "device_control",
    "policy_revision": 12
  }
}
```

### 3.3 Không trả các field sau cho compact Device Detail

```text
commands[]
tool_name
catalog_revision
dynamic tool capacity
raw command label
writable_tool_index
dynamic enabled count
raw tool flags
```

`writable_tool_index` chỉ là internal schema linkage. Raw command chỉ dùng nội bộ.

---

## 4. Backend implementation

### 4.1 File đề xuất

Thêm:

```text
components/web_server/web_device_detail_api.c
```

Cập nhật:

```text
components/web_server/CMakeLists.txt
components/web_server/include/web_modules.h
components/web_server/web_server.c
```

### 4.2 Data read order

```text
1. validate device_id
2. device_store_get()
3. ble_central_get_device_status()
4. gateway_events_current_seq()
5. device_schema_get()
6. device_state_snapshot()
7. mcp_tool_exposure_get_policy_revision()
8. build bounded JSON
9. set X-Gateway-Event-Seq
10. send response
```

Không gọi trong compact detail endpoint:

```text
mcp_tool_catalog_get_revision()
mcp_tool_catalog_find(...)
mcp_tool_name_generate(...)
```

---

## 5. Feature mapping

Dùng trusted `device_template`.

### Feature

```text
GENERIC_RELAY         -> relay
ON_OFF_PLUGIN_UNIT    -> outlet
ON_OFF_LIGHT          -> light
DIMMABLE_LIGHT        -> light
FAN                   -> fan
TEMPERATURE_SENSOR    -> temperature
HUMIDITY_SENSOR       -> humidity
CONTACT_SENSOR        -> contact
```

### Property

```text
ON_OFF            -> on_off           -> bool
LEVEL             -> level            -> int
PERCENT_SETTING   -> percent_setting  -> int
PERCENT_CURRENT   -> percent_current  -> int
TEMPERATURE       -> temperature      -> int
HUMIDITY          -> humidity         -> int
CONTACT           -> contact          -> bool
```

Unknown mapping phải fail closed:

```json
{
  "semantic": {
    "name": null,
    "property": null,
    "value_type": "none"
  },
  "writable": false,
  "mcp_control": {
    "enabled": false,
    "health": "unavailable"
  }
}
```

Không suy luận bằng label/string heuristic.

---

## 6. MCP feature policy contract

### 6.1 UI model

Device Detail compact quản lý:

```text
MCP / AI write permission per semantic feature
```

MCP client vẫn thấy đúng 3 tool:

```text
get_status
list_devices
device_control
```

Feature toggle chỉ quyết định `device_control(operation=set)` có được phép write feature đó hay không.

### 6.2 API mutation

Giữ:

```http
PUT /api/mcp/exposures
```

Compact path chỉ dùng:

```json
{
  "device_id": "AC:27:6E:CC:F2:26",
  "feature_id": "relay_1",
  "enabled": true
}
```

Destructive:

```json
{
  "device_id": "AC:27:6E:CC:F2:26",
  "feature_id": "relay_1",
  "enabled": true,
  "confirm_destructive": true
}
```

### 6.3 Backend compact mutation

Web API phải gọi typed feature policy:

```c
esp_err_t mcp_tool_exposure_set_feature_enabled(
    const char *device_id,
    const char *feature_id,
    bool enabled,
    const mcp_exposure_enable_options_t *options);
```

Không làm ở Web layer:

```text
feature_id
  -> writable_tool_index
  -> raw command
  -> mcp_tool_exposure_enable(device_id, command)
```

Raw command resolution, nếu còn cần, phải nằm trong exposure/policy layer.

---

## 7. Fail-closed behavior

Nếu `mcp_tool_exposure_get_feature()` không tìm thấy record hoặc trả lỗi, không được default enable.

Phải trả:

```json
{
  "enabled": false,
  "health": "unavailable"
}
```

Feature chỉ được hiển thị toggle khi:

```text
trusted semantic mapping exists
AND writable capability hợp lệ
AND policy layer nhận diện feature
```

Nếu không:

```text
toggle disabled
enabled = false
health = unavailable
```

---

## 8. Frontend API refactor

Cập nhật:

```text
components/web_server/www_src/dashboard/js/core/api.js
```

Thêm:

```js
async getDeviceDetailSnapshot(deviceId) {
    const {data, eventSeq} = await this.requestWithMeta(
        `/api/devices/detail?device_id=${encodeURIComponent(deviceId)}`
    );
    return { detail: data, eventSeq };
}
```

Khi mở Device Detail, bỏ:

```js
api.getDeviceSchemaSnapshot(deviceId)
mcpTools.loadDevice(deviceId)
```

thành:

```js
const result = await api.getDeviceDetailSnapshot(deviceId);
```

---

## 9. Device Detail controller

### 9.1 State

Trong `devices.js`:

```js
detailLoadId: 0,
currentDetail: null,
detailLoadPromise: null,
detailReloadRequested: false,
```

Không để `mcpTools` tự sở hữu lifecycle riêng của page.

### 9.2 Open detail

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

Không gọi:

```js
this.loadSchema(dev);
mcpTools.loadDevice(dev.id);
```

### 9.3 `loadDetail()`

```js
async loadDetail(deviceId) {
    const loadId = ++this.detailLoadId;

    try {
        const result = await api.getDeviceDetailSnapshot(deviceId);

        if (loadId !== this.detailLoadId) return;
        if (state.selectedDeviceDetail?.id !== deviceId) return;

        this.currentDetail = result.detail;
        this.applyDetailSnapshot(result.detail);
        events.goLive(result.eventSeq);
    } catch (error) {
        if (loadId !== this.detailLoadId) return;
        if (state.selectedDeviceDetail?.id !== deviceId) return;

        this.renderDetailError(error);
    }
}
```

---

## 10. Render split

```js
applyDetailSnapshot(detail) {
    this.renderDeviceHeaderFromDetail(detail);
    this.renderConnectionFromDetail(detail.connection);
    this.renderSchemaFromDetail(detail.schema);
    this.renderFeatures(detail.features || []);
    mcpControls.render(detail);
}
```

Feature card input chỉ dùng:

```text
feature_id
semantic
writable
state
mcp_control
```

Frontend không cần raw command.

---

## 11. MCP UI refactor

Refactor:

```text
components/web_server/www_src/dashboard/js/features/mcp_exposure.js
```

thành semantic feature controller.

### Toggle API

```js
async setFeatureEnabled(deviceId, featureId, enabled, confirmDestructive = false) {
    return api.request('/api/mcp/exposures', {
        method: 'PUT',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({
            device_id: deviceId,
            feature_id: featureId,
            enabled,
            ...(confirmDestructive ? {confirm_destructive: true} : {})
        })
    });
}
```

Không gửi `command`.

Không gọi `/api/mcp/exposures` GET riêng khi load detail.

Render từ:

```js
detail.features
```

và:

```js
feature.mcp_control.enabled
feature.mcp_control.health
```

---

## 12. UI text

Đổi heading:

```text
MCP Tools
```

thành:

```text
AI / MCP Control
```

Đổi description:

```text
Choose commands exposed to MCP clients.
```

thành:

```text
Choose which device features AI clients may control.
```

Row:

```text
Relay
relay_1 · on_off

AI Control                        [ ON ]
Health                            Ready
```

Health mapping:

```text
enabled       -> Ready
needs_review  -> Needs review
orphaned      -> Unavailable
unavailable   -> Unavailable
```

Nếu health khác `enabled`, toggle disabled.

---

## 13. HTML update

Cập nhật:

```text
components/web_server/www_src/dashboard/views/device_detail.html
```

Target:

```html
<section id="mcp-control-card"
         class="bg-white rounded-xl border border-gray-200 shadow-sm overflow-hidden">
    <div class="p-5 border-b border-gray-100">
        <h3 data-i18n="device_detail.mcp_control"
            class="font-semibold text-gray-900">
            AI / MCP Control
        </h3>
        <p data-i18n="device_detail.mcp_control_desc"
           class="text-xs text-gray-500 mt-1">
            Choose which device features AI clients may control.
        </p>
    </div>

    <div id="mcp-feature-controls"
         class="divide-y divide-gray-100"
         aria-live="polite">
    </div>
</section>
```

Xóa:

```text
mcp-capacity-text
mcp-tool-rows
dynamic capacity UI
raw tool_name display
raw command display
```

---

## 14. WebSocket integration

### `feature.state`

Chỉ update local state, không REST.

### `device.connection`

Chỉ update local connection/UI, không REST.

### `device.schema`

```js
events.on('device.schema', ev => {
    if (state.selectedDeviceDetail?.id !== ev.deviceId) return;
    void this.scheduleDetailReload('device.schema');
});
```

### Coalesce structural reload

```js
scheduleDetailReload(reason) {
    if (this.detailLoadPromise) {
        this.detailReloadRequested = true;
        return;
    }

    void this.reloadDetailCoalesced(reason);
}
```

Mục tiêu:

```text
one active detail GET
+
one queued follow-up GET maximum
```

---

## 15. Ownership

### Device list controller

```text
/api/devices
device grid
add/edit/delete
selected device identity
```

### Device Detail controller

```text
/api/devices/detail
schema summary
semantic features
feature state snapshot
MCP feature policy
detail structural reload
```

### WebSocket delta

```text
connection delta
feature state delta
schema revision trigger
```

Không để cùng dữ liệu có nhiều owner tự reload độc lập.

---

## 16. Remove legacy Device Detail flow

Sau migration, Device Detail frontend không còn reference:

```text
mcpTools.loadExposures()
mcpTools.loadDevice()
data.commands
command.tool_name
command.command
capacity.max_enabled
capacity.enabled
mcp-capacity-text
```

Compact Device Detail không cần:

```text
catalog_revision
dynamic tool catalog size
dynamic tool naming
```

---

## 17. `/api/mcp/exposures` GET

Sau khi Device Detail dùng `/api/devices/detail`, Web page không được gọi GET `/api/mcp/exposures` khi mở detail.

Device Detail production flow không phụ thuộc endpoint GET này.

---

## 18. Fix legacy capacity bug

Nếu legacy GET vẫn tồn tại trong migration:

Sai:

```c
cJSON_AddNumberToObject(
    cap_obj,
    "max_records",
    capacity.max_enabled);
```

Đúng:

```c
cJSON_AddNumberToObject(
    cap_obj,
    "max_records",
    capacity.max_records);
```

Field này không được dùng trong compact Device Detail mới.

---

## 19. Generated Web UI source of truth

Source chính:

```text
components/web_server/www_src/
```

Build flow:

```text
www_src/
  -> tools/build_webui.py
  -> generated dashboard.html
  -> gzip
  -> EMBED_FILES
```

Không sửa trực tiếp generated `www/dashboard.html` để implement page mới.

Sau update phải clean build để firmware embed asset mới.

---

## 20. Memory rules

1. Không tạo duplicate full schema snapshots trong cùng request.
2. Không tạo `features[]` và `commands[]` song song.
3. Không generate dynamic tool name cho Device Detail.
4. Không tạo 4KB generic response buffer.
5. Không giữ cJSON tree qua async boundary.
6. Không giữ page data duplicate ở nhiều JS controller.
7. Detail JSON phải bounded theo schema compile-time limits.
8. Chỉ giữ normalized detail snapshot cần thiết ở frontend.

---

## 21. Error contract

Unknown device:

```http
404
```

Schema chưa ready vẫn trả `200` nếu device tồn tại:

```json
{
  "schema": {
    "state": "discovering",
    "has_committed": false
  },
  "features": []
}
```

Internal snapshot failure:

```http
500
```

---

## 22. Tests bắt buộc

### Backend

```text
GET detail unknown device -> 404
GET detail offline device -> valid snapshot
GET detail connected ready -> valid snapshot
GET detail schema discovering -> features=[]
GET detail schema ready -> semantic feature list
GET detail bool state
GET detail int state
GET detail unknown semantic mapping -> fail closed
GET detail exposure missing -> enabled=false
GET detail exposure enabled
GET detail needs_review
GET detail orphaned/unavailable
GET detail includes policy_revision
GET detail does not include commands[]
GET detail does not include catalog_revision
GET detail does not include tool_name
GET detail sets X-Gateway-Event-Seq
```

### MCP mutation

```text
PUT feature_id + enabled=true
PUT feature_id + enabled=false
unknown feature -> 404
read-only feature -> deny
unknown schema -> deny
needs review -> cannot silently enable
destructive feature without confirmation -> deny
destructive feature with confirmation -> success
```

### Frontend

```text
open detail -> exactly one /api/devices/detail
open detail -> no /api/mcp/exposures GET
open detail -> no /api/devices/schema GET
feature.state -> no REST
device.connection -> no REST
device.schema -> one detail GET
multiple schema events during active load -> max one queued reload
route restoration -> no duplicate detail GET
ws connected during initial detail load -> no duplicate detail GET
toggle -> feature_id payload
toggle never sends raw command
missing MCP policy -> disabled
health != enabled -> toggle disabled
```

---

## 23. Acceptance request count

### Open one Device Detail

Expected đúng:

```text
GET /api/devices/detail?device_id=X
```

Không được có ngay sau open:

```text
GET /api/devices/schema?device_id=X
GET /api/mcp/exposures?device_id=X
```

### Feature state event

```text
0 REST requests
```

### Connection event

```text
0 REST requests
```

### Schema event

```text
1 GET /api/devices/detail?device_id=X
```

---

## 24. Implementation sequence

1. Tạo `web_device_detail_api.c` và backend tests.
2. Chuyển compact PUT exposure sang typed `feature_id` policy.
3. Thêm `api.getDeviceDetailSnapshot()`.
4. Refactor `openDetailView()`, `loadDetail()`, `applyDetailSnapshot()`.
5. Refactor `mcp_exposure.js` sang semantic feature permission.
6. Update WebSocket lifecycle.
7. Xóa stale Device Detail references.
8. Clean Web UI build và firmware build.
9. Chạy unit/integration tests.
10. Hardware smoke test.

---

## 25. Zero-reference checks cho Device Detail compact flow

```bash
git grep -n "mcpTools.loadDevice" -- components/web_server/www_src
git grep -n "loadExposures" -- components/web_server/www_src
git grep -n "data.commands" -- components/web_server/www_src
git grep -n "mcp-capacity-text" -- components/web_server/www_src
git grep -n "tool_name" -- components/web_server/www_src/dashboard
```

Các reference thuộc Device Detail compact flow phải được loại bỏ.

---

## 26. Exit criteria

- [ ] Device Detail open chỉ tạo một `/api/devices/detail`.
- [ ] Device Detail không gọi `/api/mcp/exposures` GET.
- [ ] Device Detail không gọi `/api/devices/schema` riêng.
- [ ] MCP UI render từ semantic `features[]`.
- [ ] MCP toggle dùng `feature_id`.
- [ ] Frontend không biết raw command của feature.
- [ ] Missing MCP exposure fail closed.
- [ ] `feature.state` không tạo REST request.
- [ ] `device.connection` không tạo REST request.
- [ ] `device.schema` chỉ tạo một coalesced detail reload.
- [ ] Compact detail response không có dynamic catalog metadata.
- [ ] Không có `tool_name` trên compact Device Detail UI.
- [ ] Không có MCP dynamic capacity trên compact Device Detail UI.
- [ ] Không duplicate schema traversal cho cùng page load.
- [ ] Web UI tests pass.
- [ ] Backend Web API tests pass.
- [ ] Clean firmware build pass.
- [ ] Hardware smoke test pass trên ESP32-S3.

---

## 27. Hardware smoke test

1. Boot gateway.
2. Add một BLE device.
3. Chờ device READY.
4. Mở Devices page.
5. Mở Device Detail.
6. Xác nhận browser chỉ gọi một `/api/devices/detail`.
7. Xác nhận Features hiển thị semantic feature.
8. Xác nhận `AI / MCP Control` hiển thị feature permission.
9. Enable một feature.
10. Xác nhận PUT dùng `feature_id`.
11. Gọi MCP `device_control set`.
12. Xác nhận BLE command thực thi.
13. Xác nhận `feature.state` WebSocket cập nhật UI mà không gọi REST.
14. Disable feature trong Web.
15. Gọi lại MCP `device_control set`.
16. Xác nhận bị deny.
17. Refresh schema.
18. Khi `device.schema` event đến, xác nhận chỉ có một `/api/devices/detail`.
19. Reboot gateway.
20. Xác nhận trạng thái disabled vẫn được giữ.
21. Disconnect device.
22. Xác nhận UI chuyển offline không phát sinh detail REST request không cần thiết.

---

## 28. Target final flow

```text
                        WEB DEVICE DETAIL
                               |
                               v
                GET /api/devices/detail?id=X
                               |
       +-----------------------+-----------------------+
       |                       |                       |
       v                       v                       v
  device_schema           device_state          MCP feature policy
       |                       |                       |
       +-----------------------+-----------------------+
                               |
                               v
                    semantic detail snapshot
                               |
              +----------------+----------------+
              |                |                |
              v                v                v
          Features         State UI        AI/MCP Control


WebSocket:
feature.state      -> local state delta
device.connection  -> local connection delta
device.schema      -> one detail snapshot reload


MCP permission mutation:
Web toggle
   -> PUT /api/mcp/exposures
   -> device_id + feature_id + enabled
   -> typed feature policy


MCP execution:
device_control(set)
   -> semantic resolver
   -> feature policy
   -> device_command_service
   -> BLE
```

Đây là flow mục tiêu duy nhất cho Device Detail compact semantic MCP.
