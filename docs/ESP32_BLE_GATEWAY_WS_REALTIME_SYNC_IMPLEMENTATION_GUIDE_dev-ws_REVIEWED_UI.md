# ESP32 BLE Gateway — WebSocket Realtime Sync Implementation Guide (Reviewed)

**Repository:** `hailp-vn38/esp-ble-gateway`  
**Branch:** `dev-ws`  
**Reviewed HEAD:** `c23ddd275830df7742bedf06ba52affe3fe04c37`  
**Review date:** 2026-09-02  
**Target:** ESP32-S3 / ESP-IDF `v6.1-rc1`  
**Purpose:** Implementation-ready remediation plan aligned with the actual repository state.

> Important: the reference file is named `...PLAN_v2.3...md`, but its internal header currently says **Version 2.2**. Treat the repository source code and fresh tests as authoritative; do not infer implementation completion from the filename or checked boxes in the plan.

---

# 1. Review conclusion

The architecture in the existing plan is still correct:

```text
REST = authoritative snapshot + recovery
WebSocket = ordered realtime delta/invalidation stream
```

However, the `dev-ws` branch is **not yet at the completion level implied by the `[x]` checkboxes in the plan**.

The branch already has:

- `gateway_events`;
- domain event producers;
- `/ws/events` route;
- bounded WS ring;
- REST snapshot sequence headers;
- frontend WebSocket singleton;
- reconnect/backoff;
- partial event-driven device/schema/feature handling;
- modular Web UI build and gzip/embed pipeline.

The branch still has blocking correctness issues in:

1. ESP-IDF 6.x WS post-handshake client registration;
2. WS client lifecycle / stale FD cleanup;
3. producer-path synchronization;
4. frontend startup/resync ordering;
5. duplicate/gap replay handling;
6. schema refresh fixed delay;
7. READY-vs-connected status semantics;
8. degraded-mode UX;
9. integration test quality.

Do not add more polling to work around these issues.

---

# 2. Actual repository status

| Area | Repo state | Review |
|---|---|---|
| `gateway_events` component | Implemented | **Needs hardening** |
| CRUD event producers | Implemented | Good |
| BLE ready/disconnect producer | Implemented | Good, READY semantic |
| Schema event producer | Implemented | Good |
| Feature state producer | Implemented | Good |
| Device REST event cursor | Implemented | Good |
| Schema REST event cursor | Implemented | Good |
| `/ws/events` route | Implemented | **Lifecycle broken for IDF 6.x** |
| WS bounded ring | Implemented | Needs recovery hardening |
| WS client limit 2 | Implemented | Needs stale FD protection |
| Frontend WS singleton | Implemented | Partial |
| Startup snapshot + replay | Partial | **Race exists** |
| Duplicate handling | Partial | **Incorrect order** |
| Gap recovery | Partial | Replay path bypasses gap checks |
| Reconnect | Implemented | Needs session/buffer reset |
| Schema refresh | Partial | **Still has 500 ms fixed fetch delay** |
| Feature delta | Partial | Background events dropped |
| Degraded banner | Missing | Required |
| Real handshake integration tests | Missing | Required |
| Web UI modular build | Implemented | Good |

---

# 3. Evidence that the plan checkboxes are stale

The reference plan marks P01–P05 gates as complete, but current tests do not provide the required evidence.

Examples from `components/web_server/test/test_event_ws.c`:

```c
TEST_CASE("P02-T01: WS handler handshake registration", ...)
{
    TEST_ASSERT_EQUAL(ESP_OK, web_event_ws_init());
    TEST_ASSERT_EQUAL(ESP_OK, web_event_ws_init());
}
```

This does not:

- start HTTPD;
- perform an HTTP Upgrade;
- execute a real post-handshake callback;
- verify a WS client FD was registered.

Likewise the current "ring overflow" test publishes into `gateway_events` and checks listener count; it does not fill the `web_event_ws` ring.

The current P03 duplicate/gap tests assert arithmetic expressions rather than running the actual browser/event state machine.

Therefore:

```text
plan checkbox = historical/intended status
fresh executable evidence = actual completion status
```

Use the status table in this reviewed document until the tests are replaced.

---

# 4. P0 — Fix synchronization primitives first

This is more urgent than stated in the previous version of this guide.

## 4.1 `gateway_events.c`

Current code uses:

```c
xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1000));
```

and ignores the return value.

It then accesses shared state and later calls:

```c
xSemaphoreGive(s_mutex);
```

even if the take timed out.

This creates two problems:

```text
1. producer path can block for up to 1 second
2. timeout path may mutate protected state without owning the mutex
```

This contradicts the plan requirement:

```text
producer path must not block
```

### Required change

Prefer a very short critical section/spinlock for:

- assigning global `seq`;
- copying the fixed listener table.

Conceptual implementation:

```c
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

void gateway_events_publish(gateway_event_t *event)
{
    if (event == NULL) return;

    listener_slot_t local[GATEWAY_EVENT_MAX_LISTENERS];

    portENTER_CRITICAL(&s_lock);

    event->seq = ++s_seq;
    memcpy(local, s_listeners, sizeof(local));

    portEXIT_CRITICAL(&s_lock);

    for (size_t i = 0; i < GATEWAY_EVENT_MAX_LISTENERS; i++) {
        if (local[i].in_use && local[i].fn != NULL) {
            local[i].fn(event, local[i].context);
        }
    }
}
```

Registration can use the same short critical section.

Do not call listeners while the critical section is held.

### Also fix init semantics

Current `gateway_events_init()` resets sequence/listeners even when the mutex already exists.

Make initialization clearly one-shot or explicitly test-only-resettable.

---

## 4.2 `web_event_ws.c`

Current:

```c
static void lock_ws(void)
{
    xSemaphoreTake(s_ws.mutex, pdMS_TO_TICKS(1000));
}
```

has the same unchecked-take problem.

`on_gateway_event()` is called in domain producer context, so it must not wait 1 second for the WS ring lock.

### Required change

Use a short `portMUX_TYPE` critical section for:

- ring indices/count;
- `work_pending`;
- `resync_required`;
- fixed client registry metadata;
- counters.

Do not hold it while:

- serializing JSON;
- calling `httpd_queue_work`;
- sending socket data;
- logging heavily.

---

## 4.3 `device_state.c`

This is pre-existing P00 technical debt, but it is directly on the feature-state producer path.

Current `lock_state()` also ignores the result of a 1000 ms semaphore take.

At minimum:

- check lock acquisition;
- never call `xSemaphoreGive()` without ownership;
- define a deterministic failure path.

Prefer fixing this in the same hardening series because the plan already claims P00 concurrency hardening is complete.

---

# 5. P0 — Correct ESP-IDF 6.x WebSocket lifecycle

## 5.1 Current bug

`web_event_ws_handler()` currently tries to register a client inside:

```c
if (req->method == HTTP_GET) {
    register_client(fd);
}
```

ESP-IDF 6.x performs the WebSocket handshake separately and does not call the URI data handler for connection-time initialization.

Therefore this is not a reliable client-registration point.

---

## 5.2 Enable post-handshake support

Add to:

```text
sdkconfig.defaults
test/sdkconfig.defaults
```

```ini
CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y
```

Keep:

```ini
CONFIG_HTTPD_WS_SUPPORT=y
CONFIG_WS_TRANSPORT=y
```

`CONFIG_WS_TRANSPORT` is still needed by the outgoing MCP/Xiaozhi WebSocket client and is not a replacement for HTTPD WS support.

---

## 5.3 Add post-handshake callback

```c
static esp_err_t web_event_ws_on_connect(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);

    if (!register_client(fd)) {
        /*
         * Handshake has already completed.
         * Returning failure causes HTTPD to close the upgraded socket.
         */
        return ESP_FAIL;
    }

    return ESP_OK;
}
```

Route:

```c
static const httpd_uri_t ws_uri = {
    .uri = "/ws/events",
    .method = HTTP_GET,
    .handler = web_event_ws_handler,
    .is_websocket = true,
    .handle_ws_control_frames = true,
    .ws_control_handler = web_event_ws_control_handler,
    .ws_post_handshake_cb = web_event_ws_on_connect,
};
```

Remove handshake registration from the data handler.

---

# 6. P0 — Client registry must handle CLOSE, stale FDs and FD reuse

The previous guide only required CLOSE cleanup. That is not sufficient.

A client can disappear without a clean CLOSE:

```text
browser killed
Wi-Fi lost
TCP reset
AP transition
laptop sleeps
```

A stale slot can then block reconnect before another event causes a failed send.

## 6.1 Graceful CLOSE

Add:

```c
static esp_err_t web_event_ws_control_handler(
    httpd_req_t *req,
    const httpd_ws_frame_t *frame)
{
    if (frame->type == HTTPD_WS_TYPE_CLOSE) {
        prune_client(httpd_req_to_sockfd(req));
    }
    return ESP_OK;
}
```

ESP-IDF still performs the protocol reply.

---

## 6.2 Validate registry slots

Before counting a slot as active:

```c
httpd_ws_get_fd_info(s_ws.server, fd)
```

must be:

```c
HTTPD_WS_CLIENT_WEBSOCKET
```

Otherwise prune the slot.

Do this:

- before enforcing the 2-client limit;
- before broadcast;
- after reconnect churn when possible.

---

## 6.3 Handle duplicate/reused FD

`register_client(fd)` must first check:

```text
does this exact fd already exist in an active valid slot?
```

If yes:

```text
return success without allocating another slot
```

If the same numeric FD is present but no longer a valid WebSocket:

```text
clear old slot
then register
```

This prevents duplicate slot consumption after FD reuse.

---

# 7. Correct an inaccurate source comment

Current `web_event_ws.c` says:

```text
httpd_ws_send_frame_async() queues the frame through httpd_queue_work()
and can be called from any task
```

That is incorrect for ESP-IDF 6.1.

`httpd_ws_send_frame_async()` sends through the session socket directly.

The current architecture is safe because it does:

```text
producer
 -> ring
 -> httpd_queue_work()
 -> web_event_ws_drain() in HTTPD context
 -> httpd_ws_send_frame_async()
```

Keep that model.

Do **not** change producer code to call `httpd_ws_send_frame_async()` directly.

Also avoid blindly switching every send to `httpd_ws_send_data_async()`: that API allocates a transfer object per send, which is unnecessary for the existing bounded worker architecture.

---

# 8. P0 — Make `web_event_ws_init()` truly idempotent

Current code:

```c
memset(&s_ws, 0, sizeof(s_ws));
s_ws.mutex = xSemaphoreCreateMutex();
```

Calling it twice destroys the stored mutex handle and allocates another mutex.

The existing test calls it twice but only checks `ESP_OK`, so the test does not detect the leak.

After changing to `portMUX_TYPE`, initialization should be one-shot and must not reset a running WS registry/ring.

Example design:

```c
static bool s_initialized;

esp_err_t web_event_ws_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    memset(&s_ws, 0, sizeof(s_ws));
    s_ws.lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    s_initialized = true;

    return ESP_OK;
}
```

Use the exact initialization form supported by the project's ESP-IDF compiler/configuration.

---

# 9. P0/P1 — Serializer must be pure

Current `serialize_event()` does:

```c
if (n < 0 || (size_t)n >= len) {
    s_ws.resync_required = true;
    return -1;
}
```

This is wrong for two reasons:

1. serializer mutates global WS state;
2. it mutates `resync_required` without the WS lock.

There is also a recovery hole:

```text
drain captures need_resync
drain clears work_pending
serialize later fails
serializer sets resync_required
no new work is necessarily scheduled
```

The recovery signal can remain pending until another event arrives.

### Required structure

```c
static int serialize_event(...)
{
    ...
    if (failed) return -1;
    return n;
}
```

The drain worker handles failure:

```text
serialize failure
 -> increment serialize_error_count
 -> mark recovery reason
 -> send resync.required in the current drain if possible
```

Do not rely on a future event to schedule recovery.

---

# 10. P1 — `resync.required` contract

Current overflow path creates:

```json
{
  "seq": 0,
  "type": "resync.required"
}
```

Use the current event high-water mark:

```json
{
  "seq": 1234,
  "type": "resync.required",
  "reason": "ring_overflow"
}
```

Recommended reasons:

```text
ring_overflow
queue_work_failed
serialize_failed
```

The frontend does not apply this as a normal state delta; it immediately leaves LIVE and takes a snapshot.

---

# 11. P1 — WebSocket metrics

Current `/api/status` exposes:

```json
{
  "websocket": {
    "active_clients": ...,
    "max_clients": 2,
    "ring_used": ...,
    "ring_depth": 32,
    "resync_pending": ...
  }
}
```

Current `web_event_ws_get_stats()` calls its last output `resync_count`, but it actually returns a boolean pending flag.

Do not silently change the semantic under the same field.

Recommended API:

```json
{
  "websocket": {
    "active_clients": 1,
    "max_clients": 2,
    "ring_used": 0,
    "ring_depth": 32,
    "resync_pending": false,
    "resync_total": 4,
    "send_error_total": 1,
    "connect_total": 12,
    "disconnect_total": 11
  }
}
```

Files:

```text
components/web_server/web_event_ws.c
components/web_server/include/web_modules.h
components/web_server/web_system_api.c
```

---

# 12. P1 — Feature event timestamp

`device_state.c` already fills:

```c
ev.updated_at_ms = entry->updated_at_ms;
```

The WS serializer currently omits it.

Add:

```json
"updatedAtMs": 123456
```

Frontend must use this value.

Current frontend incorrectly substitutes:

```js
Date.now()
```

which is browser wall-clock time, not the gateway state timestamp.

---

# 13. P1 — JSON safety

Current serializer inserts identifiers with raw `%s`.

Current test explicitly admits that a quote/backslash creates malformed JSON and still passes because it only checks buffer length.

That is not an acceptable P05 test.

Choose one policy:

## Option A — strict identifiers

Validate allowed device/feature IDs before they enter persistent/domain state.

Example policy:

```text
[A-Za-z0-9_.:-]+
```

Only use this if it is compatible with the actual BLE protocol/device identity requirements.

## Option B — bounded JSON escaping

Escape:

```text
"
\
control characters
```

while preserving the 512-byte output limit.

If arbitrary identifiers are allowed today, use Option B.

The test must parse the produced JSON, not merely check `snprintf()` length.

---

# 14. P0 — Frontend startup must have one synchronization owner

Current `devices.load()` does both:

```text
events.init()
loadFresh() -> GET /api/devices without event cursor
```

At the same time:

```text
ws:connected
 -> _syncFromSnapshot()
 -> GET /api/devices with event cursor
```

These requests can overwrite each other.

Remove `loadFresh()` from realtime synchronization.

---

# 15. Correct startup flow

The desired sequence is:

```text
register event listeners
        |
        v
events.init()
        |
        v
WS CONNECTING
        |
        +----------------------------+
        |                            |
REST initial snapshot          WS opens
can render degraded UI             |
        |                           v
        |                       BUFFERING
        |                           |
        +---------------------------+
                    |
                    v
        authoritative cursor snapshot
                    |
                    v
              replace cache
                    |
                    v
          replay buffered seq>N
                    |
                    v
                   LIVE
```

A practical implementation may fetch one REST snapshot immediately so the page does not wait for WS connectivity.

But there must not be two uncontrolled state writers.

Use one function:

```js
_syncFromSnapshot(reason)
```

for:

- initial page state;
- WS-open synchronization;
- sequence gap;
- `resync.required`;
- controlled degraded-mode reconciliation;
- `device.changed`.

The function must be single-flight/coalesced.

---

# 16. P0 — `events.js` sequence processing

## 16.1 Current duplicate bug

Current live path checks:

```js
msg.seq !== lastSeq + 1
```

before:

```js
msg.seq <= lastSeq
```

Therefore a duplicate can trigger false resync.

Correct order:

```js
if (!isValidEvent(msg)) return;

if (msg.type === 'resync.required') {
    requestResync(...);
    return;
}

if (msg.seq <= lastSeq) {
    return;
}

if (lastSeq !== 0 && msg.seq !== lastSeq + 1) {
    requestResync('sequence_gap');
    return;
}

lastSeq = msg.seq;
apply(msg);
```

---

## 16.2 Replay must run the same gap logic

Current `_replayBuffered()` calls `_applyEvent()` directly.

That means:

```text
buffer = [N+1, N+3]
```

can be applied without detecting missing `N+2`.

Create one function:

```js
_acceptSequencedEvent(event)
```

and use it for:

- live frames;
- startup replay;
- reconnect replay.

---

## 16.3 Validate sequence values

Reject/ignore malformed application events where:

```js
!Number.isSafeInteger(msg.seq)
msg.seq < 0
typeof msg.type !== 'string'
```

Do not allow `undefined`/string sequence values to corrupt `_lastSeq`.

---

# 17. P0 — Reset event buffer per WebSocket session

The previous guide did not make this explicit enough.

Current code preserves `_buffer` across socket close/reconnect.

That is dangerous if the gateway reboots:

```text
old gateway seq ~ 5000
socket closes
gateway restarts
new gateway seq starts at 0
browser reconnects
old buffered seq 5001 remains in memory
new snapshot baseline is small
old session delta could be replayed into new session
```

A fresh authoritative snapshot makes pre-close buffered events unnecessary.

On socket close/session replacement:

```js
this._buffer = [];
this._live = false;
this._resyncPending = false;
```

Use a connection/session generation if asynchronous snapshot work can outlive a socket.

Example:

```js
_sessionId++;
const sessionId = this._sessionId;
```

Ignore completion from stale session IDs.

---

# 18. P0 — Explicit `close()` must not reconnect

Current:

```js
events.close()
 -> ws.close()
 -> onclose()
 -> _scheduleReconnect()
```

Add:

```js
_stopped: false
```

and gate reconnect.

```js
close() {
    this._stopped = true;
    ...
}

_onClose() {
    ...
    if (!this._stopped) {
        this._scheduleReconnect();
    }
}
```

---

# 19. P0 — Resync must be single-flight

Current `_resyncPending` is set but does not prevent repeated:

```text
resync:required
 -> devices._handleResync()
 -> _syncFromSnapshot()
```

calls.

Add:

```js
_syncPromise
_syncRequested
```

or equivalent generation logic in `devices.js`.

Only one snapshot request may be authoritative at a time.

Additional invalidations while it is active should set:

```text
syncRequested = true
```

and run at most one follow-up snapshot.

---

# 20. `events.js` ownership decision

Keep `events.js` transport/sequence-focused.

Recommended responsibilities:

```text
events.js:
- socket lifecycle
- buffer
- seq validation
- reconnect
- resync signal

devices.js:
- REST device snapshot
- device cache
- selected device reconciliation
- rendering
```

With this split, current script order:

```text
events.js
api.js
devices.js
```

can remain because `events.js` does not depend on `api`.

If future code moves REST resync directly into `events.js`, reorder scripts so `api.js` is loaded first.

Do not create a hidden circular dependency.

---

# 21. P0 — Device snapshot reconcile

Current `_syncFromSnapshot()` replaces:

```js
state.connectedDevices
```

but does not replace:

```js
state.selectedDeviceDetail
```

with the new object.

The selected detail can therefore point at stale data.

Required:

```js
_applyDeviceSnapshot(devicesSnapshot) {
    const selectedId = state.selectedDeviceDetail?.id ?? null;

    state.connectedDevices = devicesSnapshot;
    state.devicesLoaded = true;

    if (selectedId) {
        const selected = state.connectedDevices.find(
            item => item.id === selectedId
        );

        if (selected) {
            state.selectedDeviceDetail = selected;
            this.renderConnectionState(selected);
            this.renderDeviceHeader(selected);
        } else {
            state.selectedDeviceDetail = null;
            nav.switchTab('devices');
        }
    }

    this.renderGrid();
}
```

---

# 22. P1 — Coalesce `device.changed`

Current code runs a snapshot for every event.

Use a short coalescing timer, for example 50–100 ms:

```js
_scheduleDeviceResync() {
    if (this._deviceResyncTimer) return;

    this._deviceResyncTimer = setTimeout(() => {
        this._deviceResyncTimer = null;
        void this._syncFromSnapshot('device.changed');
    }, 75);
}
```

This is debounce/coalescing, not polling.

---

# 23. P0/P1 — CRUD local mutation policy

Current add flow:

```text
POST add
optimistically push local device
device.changed may trigger snapshot
await this.load() triggers another legacy GET
```

This creates multiple writers.

After WS migration:

```text
POST add success
 -> optional local optimistic placeholder
 -> device.changed authoritative snapshot
 -> device.connection patches READY state
```

If WS is degraded:

```text
POST add success
 -> one controlled _syncFromSnapshot('local-add')
```

Remove:

```js
await this.load();
```

from the add flow.

For edit/delete:

- optimistic local UI is allowed;
- subsequent `device.changed` snapshot is authoritative;
- do not re-enter legacy `loadFresh()`.

---

# 24. P0 — READY vs connected semantics

Current `/api/devices` uses:

```c
status.connected
```

Current WS online event is emitted from:

```c
on_device_ready(...)
```

Thus REST and WS do not mean the same thing.

Define:

```text
connected = BLE ACL exists
ready     = secure/GATT/notify ready and usable by gateway commands
UI online = ready
```

Recommended REST:

```json
{
  "device_id": "...",
  "connected": true,
  "ready": true
}
```

Frontend:

```js
status: device.ready ? 'online' : 'offline'
```

Keep `connected` for diagnostic transport state.

This change is directly relevant to the existing symptom:

```text
device is physically connected but UI online/offline behavior does not converge
```

---

# 25. P0 — Schema refresh must be event-driven

Current code still does:

```js
await api.refreshDeviceSchema(device.id);
await new Promise(resolve => setTimeout(resolve, 500));
await this.loadSchema(device, true);
```

Remove the fixed fetch delay.

Correct flow:

```text
POST /api/devices/schema/refresh
        |
        v
202 Accepted + generation
        |
        v
UI loading/discovering
        |
        v
device.schema WS event
        |
        v
GET /api/devices/schema
        |
        v
render
        |
        v
success toast
```

A 10–15 second timeout may only show:

```text
schema discovery is taking longer than expected
```

It must not trigger periodic or fallback polling.

---

# 26. Schema snapshot metadata

Backend already returns:

```http
X-Gateway-Event-Seq
```

for schema GET.

Add:

```js
async getDeviceSchemaSnapshot(deviceId) {
    const { data, eventSeq } = await this.requestWithMeta(
        `/api/devices/schema?device_id=${encodeURIComponent(deviceId)}`
    );

    return {
        schema: data,
        eventSeq
    };
}
```

Use schema revision for schema identity.

Use global event sequence for realtime stream consistency.

Do not mix them.

---

# 27. P1 — Feature state cache

Current frontend discards:

```text
feature.state for non-selected device
```

Add to `core/state.js`:

```js
featureStateByDevice: new Map(),
schemaRevisionByDevice: new Map(),
```

Every feature event updates cache.

Only selected-device events update the visible controls.

This satisfies:

```text
device B realtime state changes
while user remains on device A detail
```

without changing route.

---

# 28. P1 — Targeted feature update

Current code re-renders all feature cards.

With max ~12 features this is acceptable as a first correct implementation.

Priority order:

```text
1. correctness
2. direct cache update
3. full current-feature render
4. later optimize to one control/card
```

Do not perform:

```text
GET full device list
GET full schema
```

for a known `feature.state` event.

If an event references an unknown feature/revision, request one controlled schema snapshot.

---

# 29. P1 — Degraded mode

Current `_showDegradedBanner()` is empty.

Add visible UI markup, preferably in:

```text
www_src/dashboard/shell.html
or a reusable partial
```

Example:

```text
Realtime connection unavailable.
Showing last synchronized state.
```

Behavior:

```text
WS CLOSED
 -> show banner
 -> keep last snapshot visible
 -> reconnect with bounded backoff

WS OPEN
 -> do not hide banner immediately
 -> take snapshot
 -> replay
 -> enter LIVE
 -> hide banner
```

Do not equate socket OPEN with synchronized state.

---

# 30. Web UI build pipeline — already correct

Do not edit the wrong dashboard artifact.

Production build uses:

```text
components/web_server/www_src/
    |
    v
tools/build_webui.py
    |
    v
build/dashboard.html
    |
    v
gzip
    |
    v
EMBED_FILES
```

`components/web_server/CMakeLists.txt` already embeds the generated gzip file.

A tracked legacy/stale file under:

```text
components/web_server/www/dashboard.html
```

is not the authoritative modular Web UI source for firmware.

All realtime JS changes must be made under:

```text
components/web_server/www_src/
```

then firmware rebuilt/reflashed.

---

# 31. Backend event producers — already wired correctly

Do not rewrite these unless semantics change.

## CRUD

`gateway_commands.c` publishes:

```text
GW_EVENT_DEVICE_CHANGED
```

after add/edit/delete completion.

Good architectural location because REST/MCP/future transports converge on the same domain command.

## BLE lifecycle

`main.c` publishes:

```text
GW_EVENT_DEVICE_CONNECTION true
```

from `on_device_ready()` and false from disconnect.

Good, but this proves the event means READY/usable, not merely ACL-connected.

## Schema

`device_schema` publishes after committed schema transition.

## Feature state

`device_state` copies primitive state into the event after updating the runtime cache.

These producers are not the main missing piece.

---

# 32. REST cursor implementation — already correct

`GET /api/devices` captures:

```c
uint32_t base_seq = gateway_events_current_seq();
```

before building the snapshot and returns:

```http
X-Gateway-Event-Seq
```

`GET /api/devices/schema` does the same.

This conservative capture order is correct:

```text
event after baseline
 -> event seq > baseline
 -> replay after snapshot
```

A semantic state may appear both in snapshot and replay; event handlers must therefore be idempotent.

---

# 33. WebSocket event contracts

## `device.connection`

```json
{
  "seq": 101,
  "type": "device.connection",
  "deviceId": "device-01",
  "connected": true
}
```

Until contract versioning changes, treat `connected=true` here as:

```text
READY / command-usable
```

and align REST UI mapping to the same semantic.

---

## `device.changed`

```json
{
  "seq": 102,
  "type": "device.changed",
  "deviceId": "device-01"
}
```

Action:

```text
coalesced /api/devices snapshot
```

---

## `device.schema`

```json
{
  "seq": 103,
  "type": "device.schema",
  "deviceId": "device-01",
  "revision": 8
}
```

Action:

```text
cache revision
selected device -> one schema snapshot
```

---

## `feature.state`

```json
{
  "seq": 104,
  "type": "feature.state",
  "deviceId": "device-01",
  "featureId": "power",
  "propertyId": 1,
  "valueType": "bool",
  "value": true,
  "updatedAtMs": 1234567
}
```

---

## `resync.required`

```json
{
  "seq": 110,
  "type": "resync.required",
  "reason": "ring_overflow"
}
```

---

# 34. Frontend realtime state model

Recommended:

```js
const RT_STATE = Object.freeze({
    STOPPED: 'stopped',
    CONNECTING: 'connecting',
    BUFFERING: 'buffering',
    SYNCING: 'syncing',
    LIVE: 'live',
    DEGRADED: 'degraded'
});
```

Transitions:

```text
STOPPED
  |
  | init
  v
CONNECTING
  |
  | WS open
  v
BUFFERING
  |
  | authoritative snapshot
  v
SYNCING
  |
  | replay valid
  v
LIVE
  |
  +-- duplicate ------> ignore
  |
  +-- N+1 ------------> apply
  |
  +-- gap ------------> SYNCING
  |
  +-- resync.required -> SYNCING
  |
  +-- close ----------> DEGRADED
                           |
                           | reconnect
                           v
                       CONNECTING
```

---

# 35. Corrected implementation order

The previous document started with WS handshake. After review, synchronization safety must come first.

## Commit 1 — event-path correctness

```text
fix(events): make realtime producer synchronization non-blocking
```

Files:

```text
components/gateway_events/gateway_events.c
components/device_state/device_state.c
components/web_server/web_event_ws.c
```

Exit:

```text
no unchecked xSemaphoreTake
no 1-second wait in gateway_events publish path
no listener called while event lock held
```

---

## Commit 2 — ESP-IDF WS lifecycle

```text
fix(ws): use ESP-IDF 6.x post-handshake lifecycle
```

Files:

```text
sdkconfig.defaults
test/sdkconfig.defaults
components/web_server/web_event_ws.c
components/web_server/include/web_modules.h
```

Exit:

```text
real upgrade
client registered
normal CLOSE prunes
stale FD validated
reconnect does not exhaust slots
```

---

## Commit 3 — WS recovery/serializer

```text
fix(ws): harden bounded delivery and recovery
```

Files:

```text
components/web_server/web_event_ws.c
components/web_server/web_system_api.c
components/web_server/test/test_event_ws.c
```

Exit:

```text
pure serializer
resync reason/high-watermark
real counters
updatedAtMs
JSON-valid serialization
```

---

## Commit 4 — frontend synchronization state

```text
fix(webui): make snapshot replay authoritative
```

Files:

```text
www_src/dashboard/js/core/events.js
www_src/dashboard/js/core/api.js
www_src/dashboard/js/core/state.js
www_src/dashboard/js/features/devices.js
```

Exit:

```text
no dual startup load
duplicate ignored
replay gap detected
single-flight resync
buffer reset per WS session
explicit close does not reconnect
```

---

## Commit 5 — schema and UI convergence

```text
fix(webui): remove fixed-delay realtime fallbacks
```

Files:

```text
www_src/dashboard/js/core/api.js
www_src/dashboard/js/core/state.js
www_src/dashboard/js/features/devices.js
www_src/dashboard/shell.html
or relevant partial
```

Exit:

```text
schema POST -> WS event -> schema GET
background feature cache
degraded banner
selected detail reconciles after snapshot
```

---

## Commit 6 — status semantic consistency

```text
fix(device): align REST and realtime ready semantics
```

Files:

```text
components/command_dispatcher/gateway_commands.c
www_src/dashboard/js/core/api.js
related tests
```

Exit:

```text
REST online == WS online == BLE READY
```

---

## Commit 7 — qualification

```text
test(ws): replace placeholder phase tests with real integration coverage
```

Exit:

```text
P02/P03/P04/P05 evidence matches what the test name claims
```

Only after this commit should the plan's phase checkboxes be updated.

---

# 36. Required tests

## T01 — real WebSocket upgrade

Must:

```text
start HTTPD
connect to /ws/events
perform RFC6455 upgrade
verify active_clients == 1
```

Not acceptable:

```text
only calling web_event_ws_init()
```

---

## T02 — init idempotency/resource

Call WS init repeatedly.

PASS:

```text
no new mutex/resource allocation
state not reset unexpectedly
```

---

## T03 — graceful CLOSE churn

Repeat > 20 cycles:

```text
connect
close
connect
close
```

PASS:

```text
active_clients returns to 0
no slot leak
```

---

## T04 — abrupt disconnect

Kill TCP/network without CLOSE.

Then reconnect.

PASS:

```text
stale fd does not permanently consume slot
```

---

## T05 — two clients + third client

PASS:

```text
client 1/2 remain functional
client 3 is closed safely
```

---

## T06 — real broadcast

Publish:

```text
device.connection
feature.state
```

PASS:

```text
both real WS clients receive same seq/payload
```

---

## T07 — ring overflow

The test must actually fill the `web_event_ws` ring while HTTPD drain is blocked/slowed.

PASS:

```text
overflow counter increments
resync.required is emitted
```

---

## T08 — queue-work failure

Fault-inject `httpd_queue_work()`.

PASS:

```text
work_pending does not remain stuck
recovery signal is eventually delivered without requiring an unrelated future event
```

---

## T09 — serializer parse validity

For:

```text
max IDs
quote
backslash
allowed UTF-8
control characters according to chosen policy
```

PASS:

```text
output parses as JSON
or input is rejected before serialization
```

Do not duplicate serializer logic inside the test.

Test the actual serializer through an internal/test hook.

---

## T10 — gateway event lock contention

Create concurrent publishers.

PASS:

```text
no 1-second stalls
no duplicate seq
no unlocked mutation
```

Measure duration; do not merely count callbacks.

---

## T11 — duplicate frontend sequence

```text
lastSeq = 20
incoming = 20
```

PASS:

```text
ignored
no resync
```

---

## T12 — replay gap

```text
snapshot seq = 20
buffer = [21, 23]
```

PASS:

```text
21 may apply
23 causes resync
never silently enters LIVE
```

---

## T13 — gateway reboot while browser remains open/reconnects

Scenario:

```text
old stream seq ~5000
gateway restart
new stream seq starts near 0
```

PASS:

```text
old-session buffer is never replayed into new session
fresh snapshot wins
```

---

## T14 — add device delayed READY

PASS:

```text
POST accepted
device.changed adds/reconciles card
device.connection makes card online
no 1-second polling
```

---

## T15 — REST/WS status semantic

Test BLE states:

```text
disconnected
ACL connected but not READY
READY
```

PASS:

```text
UI online only when READY
REST snapshot and WS delta agree
```

---

## T16 — schema refresh

PASS:

```text
POST 202
loading state
device.schema
one schema GET
success toast after GET
no 500 ms/2500 ms fixed fetch
```

---

## T17 — background feature event

```text
selected = A
event = B
```

PASS:

```text
B cache updates
A route stays selected
```

---

## T18 — network reconnect

PASS:

```text
degraded banner
bounded reconnect
new session buffer
snapshot
replay
LIVE
banner hidden only after synchronization
```

---

## T19 — 30 minute soak

Conditions:

```text
2 dashboard clients
BLE reconnect cycles
REST requests
MCP traffic
feature-state bursts
schema refreshes
browser reconnect churn
```

Measure:

```text
internal free/min/largest block
HTTPD stack high watermark
active WS clients
resync_total
send_error_total
socket failures
BLE command latency
```

PASS:

```text
no monotonic resource leak
no reconnect storm
no repeated resync loop
no REST/MCP starvation
```

---

# 37. Definition of Done

Do not mark realtime complete until every item below is true.

## Backend/event path

- [ ] No unchecked `xSemaphoreTake()` in realtime producer path.
- [ ] `gateway_events_publish()` does not wait up to one second.
- [ ] Listener callbacks execute outside synchronization lock.
- [ ] WS ring write is bounded and non-blocking/short-critical-section.
- [ ] Serializer does not mutate global WS state.
- [ ] Queue-work failure has deterministic recovery.

## ESP-IDF WS lifecycle

- [ ] `CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y`.
- [ ] Client registration occurs in `ws_post_handshake_cb`.
- [ ] Data handler does not perform handshake initialization.
- [ ] CLOSE prunes registry.
- [ ] Stale FD is validated with `httpd_ws_get_fd_info()`.
- [ ] FD reuse/duplicate registration is safe.
- [ ] Two clients work.
- [ ] Third client cannot break existing clients.

## Realtime sequence

- [ ] `seq <= lastSeq` ignored.
- [ ] Gap causes one resync.
- [ ] Replay uses the same gap logic as live traffic.
- [ ] Invalid sequence cannot corrupt cursor.
- [ ] WS session change clears old buffered deltas.
- [ ] Gateway reboot converges to new snapshot.

## Frontend/device state

- [ ] One authoritative snapshot function.
- [ ] No legacy `loadFresh()` race.
- [ ] Resync is single-flight/coalesced.
- [ ] Selected-device reference is reconciled after snapshot.
- [ ] `device.changed` is coalesced.
- [ ] `device.connection` patches status directly.
- [ ] REST online semantic equals WS READY semantic.
- [ ] Background feature event updates cache.
- [ ] Feature event uses gateway `updatedAtMs`.

## Schema

- [ ] No fixed 500 ms fetch.
- [ ] No fixed 2500 ms fetch.
- [ ] POST refresh waits for `device.schema`.
- [ ] Selected device performs one schema snapshot after event.
- [ ] Optional long timeout is UX-only.

## UX/recovery

- [ ] Degraded banner exists.
- [ ] Socket OPEN alone does not hide degraded state.
- [ ] Snapshot+replay must finish before LIVE.
- [ ] Explicit `events.close()` does not reconnect.

## Tests

- [ ] Handshake test performs real upgrade.
- [ ] Overflow test fills actual WS ring.
- [ ] Serializer test tests actual serializer.
- [ ] Duplicate/gap tests exercise actual frontend logic.
- [ ] Reboot/new-session test exists.
- [ ] 30-minute soak passes.

---

# 38. Files that actually need modification

## Mandatory

```text
sdkconfig.defaults
test/sdkconfig.defaults

components/gateway_events/gateway_events.c
components/device_state/device_state.c

components/web_server/web_event_ws.c
components/web_server/include/web_modules.h
components/web_server/web_system_api.c
components/web_server/test/test_event_ws.c

components/command_dispatcher/gateway_commands.c

components/web_server/www_src/dashboard/js/core/events.js
components/web_server/www_src/dashboard/js/core/api.js
components/web_server/www_src/dashboard/js/core/state.js
components/web_server/www_src/dashboard/js/features/devices.js
components/web_server/www_src/dashboard/shell.html
```

## Verify but likely no structural change required

```text
main/main.c
components/web_server/web_device_api.c
components/web_server/web_device_schema_api.c
components/web_server/web_server.c
components/web_server/CMakeLists.txt
```

These already contain the main producer/cursor/registration/build structure required by the architecture.

---

# 39. Do not change these working architectural decisions

Keep:

```text
max WS clients = 2
ring depth = 32 initially
JSON max = 512
REST commands
REST snapshots
WS delta only
no full schema over WS
no per-client FreeRTOS task
no second browser WS server
no reuse of outgoing mcp_ws_bridge as browser server
```

Keep manual:

```text
httpd_queue_work()
 -> one drain worker
 -> httpd_ws_send_frame_async() in HTTPD context
```

This is memory-friendlier than allocating an async transfer object for every event/client send.

---

# 40. Final target flow

```text
PAGE LOAD
   |
   +--> register handlers once
   |
   +--> start REST snapshot through one sync function
   |
   +--> open /ws/events
             |
             v
          BUFFERING
             |
             v
       REST cursor snapshot
             |
             v
       replace device cache
             |
             v
       reconcile selected item
             |
             v
       replay buffered seq>N
             |
             v
            LIVE
```

```text
LIVE device.connection
    -> patch status/card/detail
```

```text
LIVE device.changed
    -> coalesced authoritative device snapshot
```

```text
LIVE feature.state
    -> cache device/feature state
    -> update visible control if selected
```

```text
schema refresh
    -> POST accepted
    -> wait device.schema
    -> schema snapshot
    -> render
```

```text
sequence gap / overflow
    -> leave LIVE
    -> buffer
    -> single-flight REST snapshot
    -> replay
    -> LIVE
```

```text
socket close
    -> clear old-session buffer
    -> DEGRADED
    -> reconnect/backoff
    -> new session
    -> snapshot
    -> replay
    -> LIVE
```

---

# 41. Review verdict

The previous implementation guide was directionally correct, but this reviewed version changes the priority and adds missing correctness work.

Most important corrections:

```text
1. producer lock correctness is P0
2. post-handshake callback is P0
3. stale FD/FD reuse must be handled, not only graceful CLOSE
4. send_frame_async does not queue itself
5. serializer must not mutate WS state
6. old WS-session buffer must be discarded on reconnect
7. plan [x] marks are not accepted as fresh completion evidence
8. shell/degraded UI and web_system_api metrics are required files
9. tests must exercise the actual transport/state machine
```

Implement against this reviewed document rather than the unchecked completion state in the current plan.

---

# 42. Web UI implementation scope

Realtime migration is not complete if only `events.js` and backend WS are changed. The visible Web UI must also represent the new synchronization model correctly.

The two pages that require explicit UI behavior changes are:

```text
Scanner / Add Device
Device Detail
```

Relevant source files:

```text
components/web_server/www_src/dashboard/views/scanner.html
components/web_server/www_src/dashboard/views/device_detail.html
components/web_server/www_src/dashboard/views/devices.html
components/web_server/www_src/dashboard/partials/modals.html

components/web_server/www_src/dashboard/js/features/scanner.js
components/web_server/www_src/dashboard/js/features/devices.js
components/web_server/www_src/dashboard/js/core/ui.js
components/web_server/www_src/dashboard/js/core/state.js
components/web_server/www_src/dashboard/js/core/events.js
```

The scanner and detail page do **not** use WebSocket in the same way:

```text
Scanner discovery:
    REST polling is still valid and intentional.

Managed-device state after add:
    WebSocket becomes authoritative for realtime changes.

Device detail:
    WebSocket is the primary realtime delta path.
```

Do not try to force BLE scan results through `/ws/events` in this phase.

---

# 43. Scanner page — current behavior

Current scanner implementation:

```text
POST /api/ble/scan
GET  /api/ble/scan every 1 second
DELETE /api/ble/scan to stop
```

`scanner.js` currently:

```js
this.scanInterval = setInterval(async () => {
    const result = await api.getScanResults();
    ...
}, 1000);
```

This polling is **not** the legacy realtime polling that the WS migration intends to remove.

It is a bounded scan-session fetch loop for an operation whose result set changes continuously and currently has no WS event contract.

Keep it for this phase.

Current scanner also has:

```text
initial state
scanning/loading state
dynamic result rows
empty/no-device state
add-device modal
```

These can remain structurally intact.

---

# 44. Scanner page — required realtime integration

The scanner still needs to react to managed-device realtime changes.

Example:

```text
Scanner shows BLE device X
        |
        | another tab / MCP / same tab adds X
        v
device.changed
        |
        v
/api/devices authoritative snapshot
        |
        v
X is now a managed device
        |
        v
Scanner must remove X from scan results
```

Current `mergeResults()` prevents a **new** scan result from being added when its MAC already exists in `state.connectedDevices`.

It does not reliably remove a row already rendered before the authoritative device snapshot changes.

Add:

```js
reconcileManagedDevices() {
    const managedMacs = new Set(
        state.connectedDevices
            .map(device => device.mac)
            .filter(Boolean)
    );

    state.scannedDevices = state.scannedDevices.filter(device => {
        if (!managedMacs.has(device.mac)) {
            return true;
        }

        const id = `scanned-${device.mac.replace(/:/g, '')}`;
        document.getElementById(id)?.remove();
        return false;
    });

    this.renderEmptyIfNeeded();
}
```

Call it after `_applyDeviceSnapshot()`:

```js
_applyDeviceSnapshot(devicesSnapshot) {
    ...
    scanner.reconcileManagedDevices();
}
```

This creates consistent behavior whether a device is added from:

```text
current browser tab
second browser tab
MCP
future API client
```

---

# 45. Scanner page — state model

Recommended visible states:

```text
IDLE
SCANNING_EMPTY
SCANNING_RESULTS
SCAN_COMPLETE_EMPTY
SCAN_COMPLETE_RESULTS
ERROR
ADDING_DEVICE
```

These are scanner UI states, separate from WebSocket connection state.

## 45.1 IDLE

Display:

```text
Ready to scan
Start Scan
```

Current markup already supports this.

## 45.2 SCANNING_EMPTY

Display:

```text
Scanning nearby...
spinner/radar
Stop Scan
```

Do not display a permanent "No devices found" message while the firmware still reports:

```json
"scanning": true
```

Better text after several empty polls:

```text
No devices found yet.
Scanning is still active…
```

Then show final empty state only when:

```text
result.scanning == false
```

or the bounded UI fallback timeout ends.

## 45.3 SCANNING_RESULTS

Keep result rows visible while scan continues.

Do not replace the result list with the full-page loading overlay after the first result arrives.

RSSI updates may continue through REST polling.

## 45.4 SCAN_COMPLETE_RESULTS

Status:

```text
Scan complete
```

Button:

```text
Scan Again
```

Keep current results visible.

## 45.5 ERROR

Show a small inline scanner error:

```text
BLE scan could not be completed.
```

Do not silently convert every GET failure into the same visual state as a normal completed scan.

A toast can still be used, but the scan card should show the terminal state.

---

# 46. Scanner page — realtime connection badge

Scanning itself can still work through REST while the realtime WS channel is degraded.

Therefore the Scanner page should not say:

```text
Scanner offline
```

when only WebSocket is unavailable.

Recommended small secondary indicator:

```text
Gateway realtime:
Live
Reconnecting
```

Add to `views/scanner.html` near the scan status:

```html
<span id="scan-realtime-status"
      class="hidden text-xs font-medium">
</span>
```

Behavior:

```text
events LIVE
    -> "Realtime: Live"

events DEGRADED/CONNECTING
    -> "Realtime: Reconnecting"
```

This indicator explains why managed-device status updates may be delayed while BLE scanning itself still works.

A global degraded banner remains the main notification; this badge is optional but recommended.

---

# 47. Scanner page — scan result row behavior

Current result row uses:

```text
row click -> open Add Device modal
hover-only Select button
```

Keep the whole row clickable.

For touch/mobile, the current:

```text
opacity-0 group-hover:opacity-100
```

can make the "Select" action visually absent.

Recommended class behavior:

```text
opacity-100 sm:opacity-0 sm:group-hover:opacity-100
```

This is not required by WebSocket protocol, but should be included while touching the scanner UI because the page is a primary add-device workflow.

Each row should show:

```text
Device name
BLE MAC
RSSI
Select/Add action
```

Do not add online/offline state to raw scan results; they are not yet managed devices.

---

# 48. Add Device modal — current problem

Current add flow does:

```text
stop scan
POST add_device
push a local offline device into connectedDevices
close modal
await devices.load()
```

The final `devices.load()` re-enters the legacy load path and can race with WS synchronization.

The local push also creates another writer of the managed-device cache.

---

# 49. Add Device modal — target flow

Use this flow:

```text
Scanner result
    |
    v
Open Add Device modal
    |
    v
Save Device
    |
    +--> stop BLE scan if active
    |
    +--> POST /api/devices
    |
    v
HTTP success: persisted
    |
    +--> remove item from scan results
    +--> close modal
    +--> mark pendingAddDeviceId
    +--> show "Device saved. Connecting..."
    |
    v
device.changed
    |
    v
authoritative /api/devices snapshot
    |
    v
managed device exists in cache
    |
    v
open/reconcile Device Detail
    |
    v
device.connection true
    |
    v
Device Detail becomes Online
```

Do not wait for the BLE connection before returning success from the add modal.

Persistence and BLE READY are separate transitions.

---

# 50. Add Device modal — visible states

Add an explicit modal state:

```js
addDeviceState: 'idle' | 'saving' | 'accepted' | 'error'
```

Or keep it module-local.

## IDLE

Button:

```text
Save Device
```

## SAVING

Button:

```text
Saving...
spinner
disabled
```

Disable:

```text
custom name input
close/save actions that would submit twice
```

Allow Cancel only if aborting the HTTP request is intentionally supported. Otherwise disable it during the short POST.

## ACCEPTED

Modal can close immediately after persistence succeeds.

Toast:

```text
Device saved. Connecting…
```

Do not show:

```text
Added successfully
```

as though the device is already online.

If the product wording wants a success message, use:

```text
Device saved successfully.
Connecting in the background…
```

## ERROR

Restore controls and leave the modal open.

Show the backend message.

---

# 51. Post-add navigation

Recommended behavior:

```text
after device.changed snapshot contains pendingAddDeviceId
    -> open Device Detail for that device
```

Add:

```js
pendingOpenDeviceId: null
```

to state or the devices module.

On successful POST:

```js
state.pendingOpenDeviceId = newDevice.id;
```

During snapshot apply:

```js
if (state.pendingOpenDeviceId) {
    const pending = state.connectedDevices.find(
        d => d.id === state.pendingOpenDeviceId
    );

    if (pending) {
        state.pendingOpenDeviceId = null;
        this.openDetailView(pending);
    }
}
```

This makes the UX deterministic:

```text
Save Device
 -> Device Detail
 -> Connecting
 -> Online
```

If keeping the user on Scanner is preferred, do not auto-navigate; instead show an inline action:

```text
Device saved — View device
```

Choose one behavior and test it consistently.

For the current admin workflow, auto-opening detail is recommended because it makes BLE connection/schema progress visible immediately.

---

# 52. Post-add behavior when WebSocket is degraded

Do not make add-device success depend on WS availability.

Flow:

```text
POST succeeds
WS unavailable
    |
    v
one controlled _syncFromSnapshot('local-add')
    |
    v
open detail from REST snapshot
    |
    v
show realtime degraded state
```

Do not start a loop such as:

```js
setInterval(getDevices, 1000)
```

When WS reconnects:

```text
snapshot
replay
LIVE
```

will recover online/schema/feature state.

---

# 53. Scanner + BLE coexistence

Current add flow correctly stops scanning before requesting the device connection because BLE Central cannot start the desired connection reliably while discovery is active.

Keep:

```js
if (state.isScanning) {
    await scanner.stopScan();
}
```

This is part of the add-device flow and should remain documented.

Do not restart scanning automatically after a successful add when navigation moves to Device Detail.

On add failure, it is acceptable to leave scanning stopped and let the user explicitly start another scan. This avoids unexpected radio activity.

---

# 54. Scanner page — files to update

## `views/scanner.html`

- [ ] add optional realtime status badge;
- [ ] represent final scan-complete state separately from active scanning;
- [ ] keep results visible while scanning;
- [ ] make primary row action visible on mobile.

## `js/features/scanner.js`

- [ ] keep bounded scan polling;
- [ ] add `reconcileManagedDevices()`;
- [ ] distinguish scan error vs normal completion;
- [ ] improve empty-scanning vs completed-empty behavior;
- [ ] expose a small `renderRealtimeStatus()` hook if scanner-specific badge is used.

## `js/features/devices.js`

- [ ] invoke scanner reconciliation after authoritative device snapshot;
- [ ] remove `await this.load()` from add flow;
- [ ] set `pendingOpenDeviceId` or equivalent;
- [ ] use controlled REST recovery if WS is degraded.

## `partials/modals.html`

- [ ] modal button/status wording distinguishes "saved" from "online";
- [ ] prevent double submit;
- [ ] optional accepted/connecting message.

## `core/state.js`

Possible additions:

```js
pendingOpenDeviceId: null,
```

Do not store redundant copies of the entire pending device object unless needed.

---

# 55. Device Detail — target UX model

The Device Detail page should become the primary realtime status page for a managed BLE device.

It must display four independent concepts:

```text
1. BLE connection/readiness
2. WebSocket realtime synchronization status
3. Schema discovery/availability
4. Feature runtime state
```

Do not collapse these into a single "online/offline" indicator.

---

# 56. Device Detail — connection states

Current UI only renders:

```text
online
offline
```

The repository already distinguishes BLE:

```text
connected
ready
```

Recommended visible states:

```text
OFFLINE
CONNECTING
ONLINE
```

Mapping after REST exposes both fields:

```js
function deviceUiConnectionState(device) {
    if (device.ready) return 'online';
    if (device.connected) return 'connecting';
    return 'offline';
}
```

If REST is not changed immediately, a newly added local device may temporarily use:

```text
connecting
```

until the first authoritative snapshot/WS event.

## OFFLINE

Header:

```text
Offline
```

Feature notice:

```text
This device is currently offline.
Commands cannot run until the device reconnects.
```

Controls disabled.

Schema refresh disabled because backend requires BLE READY.

## CONNECTING

Header:

```text
Connecting…
```

Suggested helper:

```text
BLE link is being prepared.
```

Controls disabled.

Do not show the same gray "Offline" state while the gateway is actively establishing a connection.

## ONLINE

Header:

```text
Online
```

Controls enabled according to schema/tool availability.

---

# 57. Device Detail — header changes

Current header contains:

```text
name
MAC
online/offline status
Edit button
```

Add a second, visually smaller realtime synchronization badge.

Suggested HTML:

```html
<div class="flex flex-col items-stretch sm:items-end gap-2">
    <span id="detail-status"></span>

    <span id="detail-realtime-status"
          class="text-xs font-medium text-gray-500">
    </span>

    <button ...>Edit</button>
</div>
```

Values:

```text
Realtime: Live
Realtime: Reconnecting
Realtime: Resyncing
```

Do not use this badge as device connectivity state.

A BLE device can be Online while the browser's WS channel is temporarily reconnecting.

---

# 58. Device Detail — global degraded banner

Add global markup in `shell.html` or a reusable partial:

```html
<div id="realtime-degraded-banner"
     class="hidden mb-4 rounded-lg border border-amber-200
            bg-amber-50 p-3 text-sm text-amber-800"
     role="status">
    Realtime connection unavailable.
    Showing the last synchronized state.
</div>
```

A top-level shell placement is preferable so it also appears on Scanner and Devices pages.

Behavior:

```text
WS close
    -> show

WS open
    -> keep visible

snapshot + replay completes
    -> hide
```

Do not hide it merely on `WebSocket.onopen`.

---

# 59. Device Detail — summary cards

Current summary has:

```text
Connection
Features
```

Keep both, but make the second one explicitly represent schema state.

Recommended labels:

```text
Connection
Feature Schema
```

Connection summary values:

```text
Offline
Connecting
Online
```

Schema summary values:

```text
Unknown
Discovering
Ready
Unsupported
Error
```

Current `renderSchemaState()` maps:

```text
discovering -> loading
```

and does not explicitly style `unsupported`.

Update renderer so the user can distinguish:

```text
still discovering
device does not support schema
actual error
```

---

# 60. Device Detail — schema UI state machine

Recommended:

```text
UNKNOWN
DISCOVERING
READY
UNSUPPORTED
ERROR
```

Transitions:

```text
open detail
    -> GET schema snapshot
    -> state from backend

device READY
    -> schema discovery may run
    -> DISCOVERING

device.schema event
    -> GET schema snapshot
    -> READY / UNSUPPORTED / ERROR

manual refresh
    -> POST 202
    -> DISCOVERING
    -> wait event
    -> GET schema
```

Do not infer schema state from WebSocket connectivity.

---

# 61. Device Detail — Refresh Schema button

Current button:

```html
<button id="btn-refresh-schema">
    Refresh
</button>
```

Required button states:

```text
Refresh
Discovering…
Retry
```

## Refresh

Enabled only when:

```text
device is READY
and
no schema refresh is in progress
```

## Discovering…

After POST accepted:

```text
spinner
disabled
```

Remain in this state until:

```text
device.schema event + successful schema GET
```

or a terminal error/recovery path.

## Retry

Use when schema state is `error` and device is READY.

Do not automatically retry on a timer.

---

# 62. Device Detail — schema refresh during WS degradation

If WS is not LIVE:

```text
POST refresh may still succeed
but completion event may not reach the browser
```

Allowed behavior:

```text
POST accepted
 -> show Discovering…
 -> show realtime degraded warning
 -> do not poll
```

After WS reconnect:

```text
authoritative resync
 -> if a schema refresh is pending for selected device
 -> perform one schema snapshot recovery
 -> clear pending UI based on result
```

This is a recovery snapshot, not polling.

A 10–15 second UX timeout may display:

```text
Discovery is taking longer than expected.
Realtime connection may be unavailable.
```

Do not trigger repeated GETs.

---

# 63. Device Detail — schema snapshot race protection

`loadSchema()` currently clears:

```js
currentFeatures = []
currentTools = []
```

before awaiting the schema GET.

A `feature.state` WS event can arrive during that HTTP request.

Without a background feature cache, that state can be visually lost.

Required flow:

```text
GET schema with event cursor N
        |
        | feature.state seq N+1 arrives during GET
        v
feature event cached by device/feature key
        |
        v
schema response applied
        |
        v
overlay cached feature events where seq > N
        |
        v
render
```

Use:

```js
api.getDeviceSchemaSnapshot(deviceId)
```

returning:

```js
{
    schema,
    eventSeq
}
```

After setting `currentFeatures`, merge:

```text
featureStateByDevice[deviceId]
where cached seq > schema.eventSeq
```

This is the detail-page equivalent of snapshot + delta replay.

---

# 64. Device Detail — feature cards

For a known feature, `feature.state` should update:

```text
state value
display value
toggle button state
slider/input value
updated timestamp if displayed
```

Do not:

```text
reload devices
reload full schema
change route
```

It is acceptable in the first implementation to call:

```js
renderFeatures(...)
```

for the selected device because the schema is small.

Later optimization can update one card.

---

# 65. Device Detail — feature card pending command state

HTTP command completion and realtime feature state are separate.

Example:

```text
user clicks Turn On
    |
    v
POST /api/command
    |
    v
command HTTP result
    |
    v
feature.state event
    |
    v
toggle UI reflects authoritative state
```

Do not immediately force:

```text
isOn = !isOn
```

on button click unless explicitly implementing optimistic UI with rollback.

Recommended initial behavior:

```text
disable clicked control during HTTP request
show spinner/working state
restore enabled state after response
wait for feature.state for authoritative value
```

If the HTTP call succeeds but no feature event arrives, leave the last known value rather than inventing one.

---

# 66. Device Detail — offline transition

On:

```json
{
  "type": "device.connection",
  "connected": false
}
```

Update immediately:

```text
header -> Offline
summary -> Offline
offline notice -> visible
feature controls -> disabled
schema refresh -> disabled
```

Keep last known schema and feature values visible.

Do not clear the page on disconnect.

The values are useful as:

```text
last known state
```

but must not look interactive.

---

# 67. Device Detail — reconnect transition

When `device.connection=true`:

```text
header -> Online
summary -> Online
offline notice -> hidden
controls -> enabled if feature/tool writable
schema refresh -> enabled
```

Do not automatically reload the entire device list.

Schema discovery may independently emit:

```text
device.schema
```

and refresh the schema section.

---

# 68. Device Detail — background events

If detail is open for device A:

```text
device.connection B
device.schema B
feature.state B
```

must not replace A.

Behavior:

```text
A remains selected
B updates cache/state
Devices grid can update if relevant
```

For `device.schema B`:

```text
cache B revision
do not call loadSchema(A)
do not navigate
```

For `feature.state B`:

```text
cache B feature value
do not render A feature cards
```

---

# 69. Device Detail — selected object reconciliation

Current device snapshot replacement can leave:

```js
state.selectedDeviceDetail
```

pointing to the old object.

After every authoritative `/api/devices` snapshot:

```text
selected ID exists
    -> replace selectedDeviceDetail with new object
    -> update header/status

selected ID missing
    -> device was removed
    -> return to Devices page
```

If removed while detail is open from another tab/MCP:

```text
toast: Device was removed.
route -> Managed Devices
```

---

# 70. Device Detail — Edit Device behavior

Current edit success calls:

```js
this.openDetailView(...)
```

which resets features and reloads schema.

Renaming a device does not require schema reload.

Target:

```text
PUT /api/devices
    |
    v
optional immediate local header-name patch
    |
    v
close edit modal
    |
    v
device.changed
    |
    v
authoritative device snapshot
    |
    v
reconcile selected detail/header
```

Do not call `openDetailView()` only to refresh the name.

This avoids unnecessary:

```text
schema GET
MCP tool reload
feature loading flash
route update
```

---

# 71. Device Detail — Delete Device behavior

After successful DELETE:

```text
navigate to Managed Devices immediately
```

It is acceptable to remove the local device optimistically because the HTTP command completed successfully.

Then:

```text
device.changed
 -> authoritative snapshot
```

must remain idempotent.

Clear:

```text
selectedDeviceDetail
currentFeatures
currentTools
feature cache for deleted device
schema revision cache for deleted device
pending schema refresh for deleted device
```

Do not leave a stale detail route.

---

# 72. Device Detail — MCP Tools section

MCP exposure is not a WebSocket feature-state delta.

Keep existing MCP APIs for:

```text
load tool exposure
enable/disable tools
capacity
```

But a device/schema change can invalidate the visible list.

Recommended:

```text
device.schema for selected device
 -> successful schema GET
 -> reload MCP tools once
```

This already matches the current `loadSchema(device, true)` intent.

Do not reload MCP tools for every `feature.state`.

---

# 73. Device Detail — Advanced Tools

Keep Advanced Tools collapsed by default.

When device is not READY:

```text
disable Send command
```

The current generic command form should use the same `deviceUiConnectionState()` source as semantic feature controls.

Do not use a different online check for Advanced Tools.

---

# 74. Device Detail — accessibility/live updates

Current:

```html
<div id="feature-cards" aria-live="polite"></div>
```

is useful but re-rendering all cards on every fast feature update can make screen-reader announcements noisy.

Recommended:

- keep `aria-live="polite"` for schema loading/error state;
- avoid announcing every high-rate feature value unless needed;
- use `role="status"` for realtime/degraded status text;
- status indicators must contain text, not color only.

Connection states should always include:

```text
Online
Connecting
Offline
```

in visible text.

---

# 75. Suggested Device Detail markup additions

Conceptual additions:

```html
<!-- global or shell-level -->
<div id="realtime-degraded-banner"
     class="hidden"
     role="status">
    Realtime connection unavailable.
    Showing the last synchronized state.
</div>
```

```html
<!-- detail header -->
<span id="detail-status"></span>
<span id="detail-realtime-status"
      class="text-xs"
      role="status"></span>
```

```html
<!-- schema section -->
<p id="schema-refresh-note"
   class="hidden text-xs text-amber-700 mt-2"
   role="status">
</p>
```

Optional:

```html
<span id="detail-last-event"
      class="text-xs text-gray-400"></span>
```

Do not show gateway monotonic `updatedAtMs` as a wall-clock date unless the API defines the time base.

It can be used for ordering/freshness internally.

---

# 76. Suggested Scanner markup additions

Conceptual:

```html
<div class="flex items-center gap-2">
    <h3 id="scan-status-text">Ready to scan</h3>
    <span id="scan-realtime-status"
          class="text-xs"
          role="status"></span>
</div>
```

Optional scan terminal message:

```html
<p id="scan-result-status"
   class="text-xs text-gray-500"></p>
```

Use it for:

```text
3 devices found
Scan complete
Scan failed
```

This is clearer than encoding every state only through the full-screen loading layer.

---

# 77. UI state additions in `core/state.js`

Recommended minimum:

```js
const state = {
    ...
    pendingOpenDeviceId: null,

    featureStateByDevice: new Map(),
    schemaRevisionByDevice: new Map()
};
```

Connection/sync state can remain inside `events.js`.

Schema-refresh operation state can remain inside `devices.js`:

```js
_pendingSchemaRefresh: null
```

Scanner operational state can remain inside `scanner.js`.

Avoid making `state` a dumping ground for timers/promises.

---

# 78. UI helper functions to add

## `devices.js`

Recommended helpers:

```js
_applyDeviceSnapshot(devices)
_renderRealtimeStatus()
_applyConnectionEvent(ev)
_applyFeatureEventToCurrentSchema(ev)
_mergeFeatureCacheAfterSchema(deviceId, schemaEventSeq)
_setSchemaRefreshBusy(busy)
_reconcileSelectedDevice()
_scheduleDeviceResync()
```

## `scanner.js`

Recommended helpers:

```js
reconcileManagedDevices()
renderEmptyIfNeeded()
renderRealtimeStatus()
setScanUiState(state, detail)
```

## `ui.js`

Optional helpers:

```js
setRealtimeBanner(state)
setAddDeviceModalState(state, message)
```

Keep business/realtime logic out of `ui.js`; it should manipulate presentation only.

---

# 79. Scanner UI event matrix

| Event/action | Scanner UI |
|---|---|
| Start Scan | clear old results, show radar/loading |
| Scan GET returns results | merge rows/update RSSI |
| Scan still active but empty | show "No devices found yet" |
| Scan ends empty | show completed empty state |
| Scan ends with results | keep rows, show "Scan complete" |
| `device.changed` | authoritative device snapshot then remove newly managed rows |
| `device.connection` | no change to raw scan row; managed device page/detail updates |
| Add POST pending | modal saving/disabled |
| Add POST success | close modal, remove row, show "Saved. Connecting…" |
| Add POST failure | leave modal open, restore controls |
| WS degraded | scan remains usable; show realtime warning |
| WS returns LIVE | clear realtime warning after snapshot/replay |

---

# 80. Device Detail UI event matrix

| Event/action | Device Detail UI |
|---|---|
| Open detail | render identity/connection, fetch schema snapshot |
| REST says ACL only | `Connecting…`, controls disabled |
| `device.connection=true` | Online, enable eligible controls |
| `device.connection=false` | Offline, show notice, disable commands |
| `device.changed` | reconcile selected object; update name/status |
| selected device removed | leave detail route |
| `device.schema` | if selected, fetch schema once |
| Refresh schema POST 202 | Discovering…, button disabled |
| Schema GET success | render state/features, complete refresh |
| Schema error | Error, Retry when READY |
| `feature.state` selected device | update cache + visible control |
| `feature.state` background device | cache only |
| WS closes | show Reconnecting/degraded; keep last state |
| WS opens | show Resyncing; do not claim Live yet |
| Snapshot+replay complete | show Realtime: Live |
| Edit success | update name/reconcile; no schema reload |
| Delete success | navigate to Devices, clear selected caches |

---

# 81. Page-level implementation tests

## UI-SCAN-01 — scan remains bounded REST polling

PASS:

```text
GET /api/ble/scan only while scan session is active
poll interval stops when firmware scan ends or user stops
no permanent polling after leaving operation
```

## UI-SCAN-02 — managed device removed from current scan list

Scenario:

```text
X is visible in Scanner
X is added from another tab
device.changed arrives
```

PASS:

```text
device snapshot updates
X disappears from Scanner without restarting scan
```

## UI-SCAN-03 — add delayed READY

PASS:

```text
Save button -> Saving
POST success -> modal closes
"Saved. Connecting…" visible
Detail opens/reconciles
Online appears only after device.connection
```

## UI-SCAN-04 — add with WS down

PASS:

```text
POST still succeeds
one REST recovery snapshot
detail available
realtime degraded warning visible
no connection polling loop
```

## UI-SCAN-05 — mobile select affordance

PASS:

```text
scan result can clearly be selected without hover
```

---

## UI-DETAIL-01 — three connection states

PASS for:

```text
offline
ACL connected / not READY
READY
```

UI:

```text
Offline
Connecting…
Online
```

## UI-DETAIL-02 — offline preserves schema

PASS:

```text
disconnect does not erase feature cards
controls disabled
last values remain visible
```

## UI-DETAIL-03 — schema refresh no polling

PASS:

```text
POST 202
Discovering…
no 500ms GET
device.schema
one GET
Ready
```

## UI-DETAIL-04 — feature event during schema fetch

Inject:

```text
schema snapshot baseline = N
feature.state N+1 during GET
```

PASS:

```text
final rendered feature value is N+1
```

## UI-DETAIL-05 — edit does not reload schema

PASS:

```text
rename updates header
no unnecessary schema loading flash
no extra schema GET caused only by rename
```

## UI-DETAIL-06 — background device event

PASS:

```text
detail A remains visible while B changes
```

## UI-DETAIL-07 — WS reconnect UX

PASS:

```text
Live
-> Reconnecting
-> Resyncing
-> Live

last device/schema state remains visible throughout
```

## UI-DETAIL-08 — delete from another actor

Scenario:

```text
detail A open
A deleted from another tab/MCP
```

PASS:

```text
device.changed snapshot no longer contains A
detail exits safely to Devices
no stale commands possible
```

---

# 82. Updated mandatory file list for Web UI

The UI portion of the realtime migration should explicitly include:

```text
components/web_server/www_src/dashboard/shell.html

components/web_server/www_src/dashboard/views/devices.html
components/web_server/www_src/dashboard/views/scanner.html
components/web_server/www_src/dashboard/views/device_detail.html

components/web_server/www_src/dashboard/partials/modals.html

components/web_server/www_src/dashboard/js/core/state.js
components/web_server/www_src/dashboard/js/core/events.js
components/web_server/www_src/dashboard/js/core/api.js
components/web_server/www_src/dashboard/js/core/ui.js

components/web_server/www_src/dashboard/js/features/devices.js
components/web_server/www_src/dashboard/js/features/scanner.js
```

`views/devices.html` may need only small markup changes if the device cards remain JS-rendered, but it should be included in page-level QA because it is the navigation bridge between Scanner and Device Detail.

---

# 83. Updated implementation sequence for Web UI

After backend WS lifecycle is correct:

```text
1. events.js
   - session state
   - seq processing
   - reconnect/resync

2. state.js
   - feature cache
   - schema revision cache
   - pending-open device

3. api.js
   - devices snapshot
   - schema snapshot + event cursor

4. devices.js
   - one authoritative snapshot
   - selected reconcile
   - direct connection events
   - feature cache
   - event-driven schema refresh
   - add/edit/delete convergence

5. scanner.js
   - keep bounded scan polling
   - reconcile managed devices
   - add flow integration

6. scanner.html + modals.html
   - scanner states
   - saving/connecting wording
   - mobile action

7. device_detail.html
   - connection/realtime/schema states
   - degraded/recovery messages

8. shell.html
   - global realtime degraded banner

9. full Web UI build
   - generate
   - gzip
   - embed
   - flash

10. browser E2E
```

---

# 84. Updated Web UI Definition of Done

## Scanner

- [ ] BLE scan polling remains bounded to an active scan session.
- [ ] Scanner does not use permanent device-status polling.
- [ ] Already-managed devices are removed after authoritative device snapshots.
- [ ] Add flow stops active scan before BLE connection request.
- [ ] Add POST success is shown as persisted/saved, not falsely Online.
- [ ] New device transitions to Online only through READY state.
- [ ] Add flow has a deterministic path to Device Detail or a visible View action.
- [ ] Add works with WS degraded using one controlled REST recovery snapshot.
- [ ] Mobile users have a visible select/add affordance.
- [ ] Realtime degradation does not incorrectly disable REST BLE scanning.

## Device Detail

- [ ] Distinguishes Offline / Connecting / Online.
- [ ] Displays browser realtime Live / Reconnecting / Resyncing independently.
- [ ] Shows Unknown / Discovering / Ready / Unsupported / Error schema states.
- [ ] Refresh Schema is disabled when device is not READY.
- [ ] Refresh Schema has no fixed-delay GET.
- [ ] Offline keeps last-known schema/features visible but non-interactive.
- [ ] Feature events update visible control without full REST reload.
- [ ] Feature events during schema GET are preserved via cache/cursor merge.
- [ ] Background device events do not change selected detail.
- [ ] Selected device object is reconciled after device snapshots.
- [ ] Edit name does not force schema reload.
- [ ] Delete clears detail caches and route.
- [ ] WS reconnect keeps last state visible and only returns to Live after resync.

## Global Web UI

- [ ] Visible degraded realtime banner exists.
- [ ] Banner remains through WS OPEN until snapshot/replay completes.
- [ ] Generated dashboard uses only `www_src` changes.
- [ ] Browser E2E covers Scanner -> Add -> Detail -> READY -> Schema -> Feature updates.

---

# 85. Final user flow after UI migration

```text
Managed Devices
      |
      | Add New Device
      v
BLE Scanner
      |
      | Start Scan
      | bounded REST scan polling
      v
Scan Results
      |
      | Select
      v
Add Device Modal
      |
      | Save
      v
Stop active scan
      |
      v
POST /api/devices
      |
      v
Saved / Connecting
      |
      +--------------------------+
      |                          |
      | device.changed           | WS degraded
      v                          v
device snapshot            one REST recovery snapshot
      |                          |
      +-------------+------------+
                    |
                    v
             Device Detail
                    |
             Connecting…
                    |
                    | device.connection
                    v
                  Online
                    |
             schema discovery
                    |
                    | device.schema
                    v
             schema snapshot
                    |
                    v
             Feature controls
                    |
                    | feature.state
                    v
             realtime UI updates
```

This Scanner -> Add -> Detail flow must be considered part of the WebSocket migration, not a separate cosmetic task.

