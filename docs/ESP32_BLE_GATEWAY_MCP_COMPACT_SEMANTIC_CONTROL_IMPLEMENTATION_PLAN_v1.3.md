# ESP32 BLE Gateway - Event-Driven Device Command + MCP Compact Semantic Control Implementation Plan v1.3

**Gateway repository:** `hailp-vn38/esp-ble-gateway`  
**Device repository:** `hailp-vn38/esp-ble-device`  
**Reviewed gateway branch:** `dev-ws`  
**Reviewed gateway baseline commit:** `dc70c95d073cdccb8d55c0e1e7abdc556d63e76b`  
**Target:** ESP32-S3, BLE Protocol v4, Web UI, MCP HTTP/WS, Xiaozhi  
**Date:** 2026-09-03  
**Document version:** v1.3

> **Phase gate rule:** Every phase below has mandatory **Tests** and **Checklist / Exit criteria**. Do not start the next phase until required tests pass and all checklist items are checked. A waived item must have a recorded reason, owner, and follow-up issue in the implementation PR.

---

## Executive summary

The previous compact-MCP plan correctly reduces the default Xiaozhi/MCP surface to three tools:

```text
get_status
list_devices
device_control
```

However, review of the current command path shows a second optimization opportunity that should be implemented **before** compact MCP control.

Current device commands take this path:

```text
MCP / Web / schema-state internal caller
        |
        v
command_executor_submit()
        |
        v
FreeRTOS command worker
        |
        v
command_dispatcher_handle()
        |
        v
device_command_handle()
        |
        +--> device_schema_validate_command()
        +--> device_request_allocate()
        +--> ble_central_send_command()
        |
        +--> BLOCK on binary semaphore waiting for ACK
        |
        v
dispatch_result_t (~4 KB)
        |
        v
caller completion
```

The executor is therefore paying for queueing, worker tasks and large result buffers primarily because `device_command_handle()` blocks while waiting for BLE ACK. The default configuration creates two persistent command workers, each with a 4096-byte stack and a persistent `dispatch_result_t` of roughly 4 KB, in addition to queue/TCB/semaphore bookkeeping.

v1.3 changes the architecture to an **event-driven `device_command_service`**:

```text
submit request
    |
    v
device_command_service
    |
    +--> validate by request origin
    +--> allocate request_id/pending slot
    +--> send BLE
    +--> return immediately

BLE ACK / disconnect / timeout
    |
    v
device_command_service event loop
    |
    +--> correlate request_id
    +--> build small typed result
    +--> invoke completion callback
```

After callers migrate:

- device control no longer flows through `command_executor`;
- device control no longer flows through `command_dispatcher`;
- `device_request_manager` is absorbed into the service;
- `dispatch_result_t` is no longer the device-command result contract;
- `command_executor` remains temporarily for gateway-administration jobs only and can be reduced to one worker;
- `device_schema` remains the capability database/validator, not a queue stage;
- `device_state` remains the runtime state cache;
- compact MCP `device_control set` submits directly to the new service and receives a semantic typed result with no raw-command leakage.

There is **no BLE Protocol v4 wire-format change** in this plan. The gateway must fix the current `read_feature_state` submission shape by including `feature_id` and `property_id`; extending active reads to INT-valued properties on the device remains a separate follow-up and is not required for compact MCP v1.3 because MCP `read` still uses the gateway state cache.

---

## 1. Goals and non-goals

### 1.1 Goals

- Keep compact MCP `tools/list` constant at exactly three tools regardless of devices/capabilities.
- Replace the blocking device-command pipeline with an event-driven service.
- Remove `command_executor` and `command_dispatcher` from the normal device-control path.
- Preserve one-pending-command-per-device behavior and Protocol v4 request-id correlation.
- Preserve Web, MCP/Xiaozhi, schema discovery and state synchronization behavior through typed adapters.
- Reduce persistent task stack, large result-buffer and semaphore overhead.
- Keep `device_schema` focused on committed capability storage, resolution and validation.
- Keep `device_state` focused on runtime feature-state caching.
- Keep raw BLE command names internal to trusted gateway code in compact MCP mode.
- Preserve dynamic per-device MCP tools as a compile-time compatibility mode.
- Preserve destructive-command policy and capability-digest review behavior.
- Keep implementation modular and testable with mock BLE transport.

### 1.2 Non-goals

- No BLE Protocol v4 key/enum/wire-format change.
- No removal of `command_executor` from the whole gateway in the first release; it remains available for potentially blocking gateway-administration jobs.
- No merge of `device_schema` into the device command service.
- No merge of `device_state` into the device command service.
- No active BLE state read for compact MCP `read`; it uses `device_state` cache.
- No requirement to add INT active-read support to `esp-ble-device` in this implementation.
- No new authentication or Internet-exposure model.
- No broad refactor of BLE central connection management.

---

## 2. Baseline observations on `dev-ws`

The plan is pinned to gateway commit:

```text
dc70c95d073cdccb8d55c0e1e7abdc556d63e76b
```

Re-run this review if implementation starts from a newer `dev-ws`.

### 2.1 MCP baseline

- `mcp_registry.c` exposes two static tools: `get_status`, `list_devices`.
- `mcp_tools_list()` appends dynamic catalog bindings.
- `mcp_core.c::handle_tools_call()` sends device commands through `command_executor_submit()`.
- Dynamic bindings are revalidated against schema/exposure before execution.
- `command_dispatcher/device_command.c` includes the raw command in JSON results, so compact semantic MCP must never pass that result through directly.

### 2.2 Command pipeline baseline

`command_executor` currently:

- owns a bounded FreeRTOS queue;
- creates `CONFIG_CMD_EXEC_WORKER_COUNT` persistent worker tasks;
- allocates one persistent `dispatch_result_t` per worker;
- calls `command_dispatcher_handle()` in each worker;
- has a default worker count of 2 and default worker stack of 4096 bytes.

`device_command_handle()` currently:

- calls `device_schema_validate_command()`;
- checks BLE connection;
- allocates a pending request from `device_request_manager`;
- copies the request to a wire `gw_message_t` and adds request-id metadata;
- sends BLE;
- blocks the worker on `device_request_wait()` for up to `DISPATCHER_ACK_TIMEOUT_MS`;
- converts the ACK into a large text/JSON `dispatch_result_t`.

`device_request_manager` currently:

- has a static pending table;
- creates one binary semaphore per pending slot;
- enforces one pending request per device;
- correlates ACK by device id + request id + command;
- wakes the blocked worker by giving the pending semaphore.

### 2.3 Internal command baseline

`device_schema` discovery submits `describe_capabilities` through an injected submitter that currently bridges to `command_executor`.

`device_state::on_schema_committed()` currently submits `read_feature_state` through `command_executor`, but the generated message does not populate `feature_id`/`property_id` even though the device-side read handler expects them. In addition, normal schema validation only special-cases `describe_capabilities`; `read_feature_state` should therefore be handled as a trusted internal command class rather than pretending it is a normal advertised device tool.

### 2.4 Exposure baseline

- Semantic exposure reconcile can currently promote non-enabled records back to enabled.
- `mcp_tool_exposure_disable()` deletes a persisted record, allowing later reconcile to recreate it.
- Compact mode therefore requires durable user intent independent of capability health.
- Compact policy code must not initialize or consult the dynamic executable catalog.

---

## 3. Target architecture

### 3.1 Device-command domain

```text
                         +----------------------+
                         |    device_schema     |
                         | committed capability |
                         | resolve / validate   |
                         +----------+-----------+
                                    ^
                                    | lookup
                                    |
+-------------+              +------+------------------+
| MCP compact |------------->| semantic control       |
+-------------+              | resolver + MCP policy  |
                             +------+------------------+
                                    |
+-------------+                     |
| Web device  |---------------------+
| command API |                     |
+-------------+                     |
                                    v
                         +-------------------------+
                         | device_command_service  |
                         |-------------------------|
                         | origin validation       |
                         | request-id allocation   |
                         | pending table           |
                         | BLE submission          |
                         | ACK/disconnect/timeout  |
                         | typed completion result |
                         +------------+------------+
                                      |
                                      v
                              +---------------+
                              |  BLE central  |
                              +-------+-------+
                                      |
                                      | notify
                  +-------------------+--------------------+
                  |                   |                    |
                  v                   v                    v
          +---------------+   +---------------+   +-------------------+
          | device_schema |   | device_state  |   | device_command    |
          | discovery     |   | cache observer|   | service ACK event |
          +---------------+   +---------------+   +-------------------+
```

### 3.2 Gateway-administration domain

```text
Web / MCP gateway operations
            |
            v
command_dispatcher
            |
            +--> add_device
            +--> delete_device
            +--> edit_device
            +--> list_devices
            +--> get_status
```

The dispatcher no longer owns the device transport path after migration.

### 3.3 Architectural boundaries

`device_schema`

```text
owns:
- committed capability snapshot
- feature/tool mapping
- discovery lifecycle
- command capability/value validation

does not own:
- MCP policy
- BLE ACK pending table
- HTTP/Web response formatting
```

`device_state`

```text
owns:
- cached feature/property state
- state update observers

does not own:
- command scheduling
- MCP exposure policy
```

`device_command_service`

```text
owns:
- outbound device-command transport lifecycle
- origin-specific validation gate
- request id
- pending slots
- BLE send
- ACK correlation
- timeout/disconnect completion

does not own:
- cJSON
- HTTP status codes
- MCP JSON-RPC
- Web response formatting
- semantic feature naming
```

`command_executor`

```text
v1.3 role:
- gateway-admin jobs that should not block HTTP task
- NOT the normal device command transport
```

---

## 4. Core contracts

### 4.1 `device_command_service` public API

Create a dedicated component:

```text
components/device_command_service/
    CMakeLists.txt
    include/device_command_service.h
    device_command_service.c
    test/
```

Recommended request origin:

```c
typedef enum {
    DEVICE_CMD_ORIGIN_CONTROL = 0,
    DEVICE_CMD_ORIGIN_SCHEMA_DISCOVERY,
    DEVICE_CMD_ORIGIN_STATE_READ,
} device_command_origin_t;
```

Recommended typed request:

```c
typedef struct {
    device_command_origin_t origin;

    char device_id[GW_MSG_DEVICE_ID_LEN];
    char command[GW_MSG_COMMAND_LEN];

    bool has_bool_value;
    bool bool_value;

    bool has_int_value;
    int32_t int_value;

    bool has_feature_id;
    char feature_id[GW_FEATURE_ID_LEN];

    bool has_property_id;
    uint8_t property_id;
} device_command_request_t;
```

Recommended result:

```c
typedef enum {
    DEVICE_CMD_STATUS_OK = 0,
    DEVICE_CMD_STATUS_INVALID_ARGUMENT,
    DEVICE_CMD_STATUS_UNSUPPORTED_COMMAND,
    DEVICE_CMD_STATUS_BUSY,
    DEVICE_CMD_STATUS_NOT_CONNECTED,
    DEVICE_CMD_STATUS_TRANSPORT_ERROR,
    DEVICE_CMD_STATUS_TIMEOUT,
    DEVICE_CMD_STATUS_DEVICE_REJECTED,
    DEVICE_CMD_STATUS_INTERNAL_ERROR,
} device_command_status_t;

typedef struct {
    device_command_status_t status;
    uint32_t request_id;

    bool accepted;

    bool has_bool_value;
    bool bool_value;

    bool has_int_value;
    int32_t int_value;

    bool has_feature_value_bool;
    bool feature_value_bool;

    bool has_feature_value_int;
    int32_t feature_value_int;
} device_command_result_t;

typedef void (*device_command_completion_fn)(
    const device_command_result_t *result,
    void *context);
```

Public operations:

```c
esp_err_t device_command_service_init(void);

esp_err_t device_command_service_submit(
    const device_command_request_t *request,
    device_command_completion_fn completion,
    void *context);

bool device_command_service_on_notify(
    const char *device_id,
    const gw_message_t *message);

void device_command_service_on_disconnect(const char *device_id);

void device_command_service_get_stats(device_command_service_stats_t *out);
```

### 4.2 Service event loop

Use one small worker/event-loop task instead of N workers blocking on ACK.

Suggested queue events:

```text
SUBMIT
ACK
DISCONNECT
SHUTDOWN
```

The service task owns mutation of the pending table. Submission should be bounded and non-blocking from callers.

Pseudo-flow:

```text
while running:
    wait until either:
      - queue event arrives, or
      - nearest pending deadline expires

    process queued event
    expire overdue pending entries
```

Do not create one timer per request and do not create one semaphore per pending entry.

### 4.3 Origin-specific validation

`DEVICE_CMD_ORIGIN_CONTROL`

- device exists;
- connected;
- command must pass `device_schema_validate_command()`;
- advertised capability/value rules remain the trust boundary.

`DEVICE_CMD_ORIGIN_SCHEMA_DISCOVERY`

- exact command must be `describe_capabilities`;
- valid device id required;
- no normal advertised-command validation;
- only schema discovery code may use this origin.

`DEVICE_CMD_ORIGIN_STATE_READ`

- exact command must be `read_feature_state`;
- require non-empty `feature_id` and valid `property_id`;
- feature/property must exist in the committed schema;
- normal command advertisement is not required because this is a framework-level internal protocol command.

This replaces scattered reserved-command exceptions.

### 4.4 Pending correlation invariants

Preserve current safety behavior:

```text
- pending table capacity is bounded
- one pending command per device
- request_id 0 is invalid
- request_id wraps while avoiding live collisions
- ACK must match device id + request id + command
- malformed/unmatched ACK never completes another request
```

Add immediate disconnect completion rather than waiting for the normal ACK timeout.

### 4.5 MCP compact contract

Default compact tool surface:

```text
get_status
list_devices
device_control
```

`device_control` input remains semantic:

```json
{
  "type": "object",
  "properties": {
    "device": { "type": "string" },
    "operation": {
      "type": "string",
      "enum": ["describe", "read", "set"]
    },
    "feature": { "type": "string" },
    "bool_value": { "type": "boolean" },
    "int_value": { "type": "integer" }
  },
  "required": ["device", "operation"],
  "additionalProperties": false
}
```

`describe`

- local operation;
- returns trusted semantic feature/property metadata;
- may use committed schema while device is disconnected;
- never returns raw peripheral command or untrusted capability label.

`read`

- local operation;
- reads `device_state` cache only;
- value kind comes from trusted property vocabulary;
- deterministic `state_not_available` when cache is absent.

`set`

```text
MCP semantic resolver
    -> exposure/policy check
    -> resolve writable_tool_index
    -> validate value
    -> build DEVICE_CMD_ORIGIN_CONTROL request
    -> device_command_service_submit()
    -> typed completion
    -> semantic MCP formatter
```

No `dispatch_result_t` and no raw dispatcher JSON are used by compact `set`.

### 4.6 Dynamic MCP compatibility contract

Dynamic mode remains:

```text
get_status
list_devices
+ enabled dynamic device tools
```

Dynamic tool execution also migrates to `device_command_service`; an adapter recreates the current externally visible MCP result behavior as closely as practical. Dynamic mode may continue to expose raw command identities because it is explicitly the legacy compatibility surface.

---

## 5. Branch and delivery strategy

Because v1.3 introduces a foundational transport refactor, use stacked branches/PRs rather than mixing everything into one unreviewable change.

### 5.1 Foundation branch

```bash
git fetch origin
git switch dev-ws
git pull --ff-only
git switch -c feat/device-command-service
```

Contains Phases 0-3 only.

Merge this branch first when possible.

### 5.2 Compact MCP branch

After foundation is merged:

```bash
git switch dev-ws
git pull --ff-only
git switch -c feat/mcp-compact-semantic-control
```

Contains Phases 4-12.

If stacked PRs are required, create the MCP branch from `feat/device-command-service` and retarget it to `dev-ws` after foundation merges.

### 5.3 Recommended document path

```text
docs/ESP32_BLE_GATEWAY_MCP_COMPACT_SEMANTIC_CONTROL_IMPLEMENTATION_PLAN_v1.3.md
```

---

## 6. Phase-by-phase implementation

## Phase 0 - Baseline characterization ✅ DONE (2026-09-03)

### Files

- `components/command_executor/*`
- `components/command_dispatcher/*`
- `components/mcp_endpoint/test/test_mcp_stress.c`
- `components/mcp_endpoint/test/test_mcp_xiaozhi.c`
- `components/device_schema/test/*`
- `components/device_state/test/*`

### Actions

1. Pin the exact implementation baseline SHA.
2. Record current device-command call flow from Web and MCP.
3. Record default executor configuration, worker stack high-water marks and queue stats.
4. Record heap/PSRAM before/after executor initialization.
5. Record current pending-table/semaphore count.
6. Record current device-command latency for success, rejection and timeout.
7. Record current MCP `tools/list` count/size for 0/1/multiple dynamic tools.
8. Characterize current `read_feature_state` state-seeding behavior.

### Tests

- Run all current `command_executor` tests.
- Run all current `command_dispatcher`/device-command tests.
- Run MCP endpoint/conformance/Xiaozhi/stress suites.
- Execute one successful device write end-to-end.
- Execute one disconnected command and one ACK timeout.
- Capture internal free/min-free/largest block and PSRAM metrics before and after `command_executor_init()`.
- Capture worker stack high-water marks after a representative workload.
- Verify one-pending-command-per-device current behavior.
- Log whether state seed currently produces a valid `read_feature_state(feature_id, property_id)` request; this is expected to expose the baseline defect.

### Checklist / Exit criteria

- [x] Baseline SHA recorded.
- [x] Existing tests pass before production changes.
- [x] Executor worker/queue configuration recorded.
- [x] Internal RAM/PSRAM baseline recorded.
- [x] Device command latency baseline recorded.
- [x] Pending correlation behavior recorded.
- [x] MCP tool count/serialized-size baseline recorded.
- [x] State-read baseline behavior recorded.
- [x] No production behavior changed in this phase.

---

## Phase 1 - Introduce event-driven `device_command_service` ✅ DONE (2026-09-03)

### New files

```text
components/device_command_service/CMakeLists.txt
components/device_command_service/include/device_command_service.h
components/device_command_service/device_command_service.c
components/device_command_service/test/test_device_command_service.c
```

### Actions

1. Implement the request/result/status contracts from Section 4.
2. Add one bounded FreeRTOS event queue and one service task.
3. Move request-id generation and pending-slot ownership into the service.
4. Preserve one pending request per device.
5. Add mockable hooks:

```c
typedef struct {
    int (*send_command)(const char *device_id, const gw_message_t *message);
    int (*is_connected)(const char *device_id);
} device_command_transport_hooks_t;
```

6. Convert a typed request to a local `gw_message_t` only at the transport boundary.
7. Add origin validation.
8. Add ACK event ingestion without invoking completion directly from BLE callback context.
9. Add timeout completion based on nearest pending deadline.
10. Add disconnect event that fails pending request immediately.
11. Keep the old executor/dispatcher device path operational during this phase; no production caller migration yet.

### Concurrency rules

- Only the service task mutates pending entries after initialization.
- BLE callback only validates minimally and enqueues an ACK event/copy.
- Completion callback is invoked from service-task context.
- Completion is invoked exactly once per accepted request.
- Queue admission failure returns synchronously from submit and never creates a pending slot.

### Tests

- Init/deinit single-shot behavior.
- Invalid request and null completion rejection.
- Queue-full admission failure.
- Connected CONTROL request sends expected Protocol v4 message.
- CONTROL validation rejects unadvertised command and wrong value type/range.
- SCHEMA_DISCOVERY accepts only `describe_capabilities`.
- STATE_READ accepts only `read_feature_state` with feature/property.
- One pending command per device; second command returns BUSY.
- Commands for different devices may be pending concurrently.
- Request id is non-zero and unique across live entries.
- Correct ACK completes exactly once.
- Wrong request id does not complete.
- Correct request id with wrong command does not complete.
- Duplicate ACK does not invoke completion twice.
- Timeout invokes exactly one TIMEOUT completion.
- Disconnect invokes NOT_CONNECTED promptly and frees slot.
- Send failure frees slot and returns TRANSPORT_ERROR.
- Request-id wrap test avoids zero/live collision.
- Stress repeated submit/ACK cycles and verify pending count returns to zero.
- Service-task stack high-water mark recorded.

### Checklist / Exit criteria

- [x] New service builds independently of MCP/Web layers.
- [x] No cJSON/HTTP/MCP dependency exists in the component.
- [x] No per-pending semaphore exists.
- [x] No worker blocks waiting for BLE ACK.
- [x] Pending table is bounded.
- [x] One-pending-per-device invariant preserved.
- [x] ACK correlation is at least as strict as current implementation.
- [x] Timeout and disconnect complete exactly once.
- [x] Unit tests pass with mock transport.
- [x] Existing old production path still passes regression tests.

---

## Phase 2 - Migrate schema discovery and state synchronization ✅ DONE (2026-09-03)

### Files

- `main/main.c`
- `components/device_schema/*`
- `components/device_state/device_state.c`
- `components/device_state/CMakeLists.txt`
- tests for schema/state

### Actions

#### 2.1 Schema discovery

Replace the current `command_executor` bridge with a service adapter:

```text
device_schema discovery
    -> DEVICE_CMD_ORIGIN_SCHEMA_DISCOVERY
    -> device_command_service
```

The existing injected submitter model may remain so `device_schema` does not depend directly on the service.

Map typed service result to existing `device_schema_submit_result_t`.

#### 2.2 State seed

Replace:

```text
command_executor_submit(read_feature_state)
```

with:

```text
DEVICE_CMD_ORIGIN_STATE_READ
```

For each seed request populate:

```text
feature_id
property_id
```

Do not submit a generic read with missing semantic addressing.

Because the current device implementation only actively reads BOOL properties, v1.3 gateway seed behavior should be explicit:

- seed properties supported by the current device active-read contract;
- unsupported active-read property kinds are skipped/logged and can still be populated by spontaneous events/ACKs;
- do not fake INT support.

#### 2.3 Main notify/disconnect routing

Target ingress:

```c
if (device_schema_on_notify(device_id, msg)) return;
if (device_state_on_notify(device_id, msg)) return;
device_state_on_command_ack(device_id, msg);
device_command_service_on_notify(device_id, msg);
```

On disconnect:

```c
device_schema_on_disconnect(device_id);
device_state_forget(device_id);
device_command_service_on_disconnect(device_id);
```

### Tests

- Schema discovery submit creates `DEVICE_CMD_ORIGIN_SCHEMA_DISCOVERY` request.
- Capability begin/items/end continue to be consumed by schema worker.
- Final capability ACK completes service request and discovery operation.
- Discovery timeout/disconnect maps to the same schema result class as before.
- State seed includes exact `feature_id` and `property_id`.
- BOOL state seed reaches mock device/read handler and updates cache.
- Unsupported active-read property kind is skipped rather than malformed.
- Device ACK still updates `device_state` before command completion formatter reads cache.
- Disconnect clears state and promptly completes any pending internal request.
- No schema/state code path calls `command_executor_submit()` after migration.

### Checklist / Exit criteria

- [x] Schema discovery no longer depends on command executor runtime path.
- [x] State seed no longer depends on command executor runtime path.
- [x] `read_feature_state` requests include feature/property addressing.
- [x] No false claim of INT active-read support is introduced.
- [x] Notify ordering preserves schema/state observers before generic ACK completion.
- [x] Schema and state tests pass.
- [x] Existing capability discovery works on representative hardware/fixture.

---

## Phase 3 - Migrate Web/MCP legacy device control and detach dispatcher ✅ DONE (2026-09-03)

### Files

- `components/web_server/web_command_api.c`
- `components/web_server/web_device_api.c`
- `components/mcp_endpoint/mcp_core.c`
- `components/mcp_endpoint/mcp_tools.c`
- `components/command_dispatcher/command_dispatcher.c`
- `components/command_dispatcher/device_command.c`
- `components/command_dispatcher/device_request_manager.c`
- `components/command_executor/Kconfig.projbuild`
- related tests

### Actions

#### 3.1 Web device command adapter

Convert device-command Web endpoints to `device_command_service_submit()`.

Create a Web formatter from `device_command_result_t` preserving current HTTP status semantics:

```text
OK              -> success response
BUSY            -> 409
NOT_CONNECTED   -> appropriate 4xx/409 current contract
TIMEOUT          -> gateway timeout contract
TRANSPORT_ERROR  -> service unavailable/internal transport contract
DEVICE_REJECTED  -> device-level failure contract
```

Do not introduce `dispatch_result_t` just to preserve the old internal API.

#### 3.2 Dynamic MCP adapter

Before compact mode is introduced, migrate existing dynamic device tool execution to the service while preserving dynamic-mode external behavior.

Add a legacy dynamic result formatter using:

```text
device_command_result_t
+ original binding/message context
```

Do not route through `command_dispatcher` simply to obtain legacy JSON.

#### 3.3 Remove device branch from dispatcher

After all device callers have moved, change `command_dispatcher_handle()` to gateway administration only.

Remove:

```text
device_command_handle()
command_dispatcher_on_device_notify()
device_request_manager
```

Delete `device_command.c` and `device_request_manager.c` when no references remain.

#### 3.4 Reduce executor role

Set the executor default worker count to 1 after device paths are gone.

Update help/docs to state that it serves gateway-administration jobs only.

Do not delete the executor yet because gateway delete/edit flows may include NVS/BLE side effects that should remain off the HTTP task.

### Tests

- Web device command success response matches current public contract.
- Web BUSY/timeout/disconnected/rejected mappings match current behavior.
- Dynamic MCP device tool still executes successfully.
- Dynamic MCP timeout/rejection behavior remains compatible.
- Search/build assertion: no Web/MCP/schema/state device path calls `command_executor_submit()`.
- Search/build assertion: no production reference to `device_command_handle()`.
- Search/build assertion: no production reference to `device_request_manager` after deletion.
- `command_dispatcher_handle()` rejects or no longer accepts `device_command` type according to new gateway-only contract.
- Gateway commands still dispatch correctly.
- Executor with one worker handles gateway-admin API regression tests.
- On-target/device fixture: two different BLE devices can have pending commands without requiring two blocking workers.

### Checklist / Exit criteria

- [x] Web device control uses `device_command_service`.
- [x] Existing dynamic MCP device tools use `device_command_service`.
- [x] `command_dispatcher` is gateway-admin only (device path removed).
- [x] `device_request_manager` has been removed or is no longer linked.
- [x] Device command path does not allocate `dispatch_result_t`.
- [x] Default executor worker count reduced to 1.
- [x] Gateway-admin behavior remains regression-tested.
- [x] Foundation branch is independently mergeable before MCP compact changes.

---

## Phase 4 - Add compact/dynamic MCP surface mode ✅ DONE (2026-09-03)

### Files

- `components/mcp_endpoint/Kconfig.projbuild`
- `components/mcp_tool_exposure/Kconfig.projbuild`
- `test/sdkconfig.defaults`

### Actions

Add explicit surface choice:

```text
choice MCP_TOOL_SURFACE
    default MCP_TOOL_SURFACE_COMPACT

    config MCP_TOOL_SURFACE_COMPACT
        bool "Compact semantic MCP surface"

    config MCP_TOOL_SURFACE_DYNAMIC
        bool "Legacy dynamic per-device MCP tools"
endchoice
```

Retire stale controls after verifying references:

```text
MCP_EXPOSE_FULL_CAPABILITY_TOOL
MCP_KEEP_GENERIC_DEVICE_COMMAND
MCP_DEVICE_COMMAND_ALLOWLIST
```

`MCP_DYNAMIC_TOOL_MAX_ENABLED` is meaningful only in dynamic mode.

Compatibility contract:

```text
COMPACT:
  get_status
  list_devices
  device_control

DYNAMIC:
  get_status
  list_devices
  + current enabled dynamic tools
```

### Tests

- Clean compact build.
- Clean dynamic build.
- Choice is mutually exclusive.
- Retired symbols have no hidden compile/runtime dependency.
- Compact tool count remains static with multiple devices/exposures.
- Dynamic tool discovery matches Phase 0 baseline.

### Checklist / Exit criteria

- [x] Compact is default.
- [x] Dynamic compatibility builds.
- [x] `device_control` is not accidentally added to dynamic mode.
- [x] Stale controls removed/deprecated consistently.
- [x] No BLE/device-service behavior changes in this phase.

---

## Phase 5 - Expand trusted semantic vocabulary ✅ DONE (2026-09-03)

### Files

- `components/device_template/include/device_template.h`
- `components/device_template/device_template.c`
- `components/device_template/test/*`

### Actions

Add:

```c
const char *device_template_feature_name(uint8_t feature_type,
                                         uint16_t schema_version);
const char *device_template_property_name(uint8_t property_id);
uint8_t device_template_property_value_type(uint8_t property_id);
```

Feature mappings:

```text
GENERIC_RELAY          -> relay
ON_OFF_PLUGIN_UNIT     -> outlet
ON_OFF_LIGHT           -> light
DIMMABLE_LIGHT         -> light
FAN                    -> fan
TEMPERATURE_SENSOR     -> temperature
HUMIDITY_SENSOR        -> humidity
CONTACT_SENSOR         -> contact
```

Property mappings:

```text
ON_OFF             -> on_off           -> BOOL
LEVEL              -> level            -> INT
PERCENT_SETTING    -> percent_setting  -> INT
PERCENT_CURRENT    -> percent_current  -> INT
TEMPERATURE        -> temperature      -> INT
HUMIDITY           -> humidity         -> INT
CONTACT            -> contact          -> BOOL
```

Unknown mappings fail closed. Never treat peripheral label text as semantic identity.

### Tests

- Table-driven test for every known feature enum.
- Table-driven test for every known property enum.
- BOOL/INT/NONE value-kind test.
- Unknown feature/property test.
- Unsupported schema-version test.
- Regression: peripheral capability label cannot become trusted semantic alias.

### Checklist / Exit criteria

- [x] Known Protocol v4 features mapped or explicitly unsupported.
- [x] Known properties mapped or explicitly unsupported.
- [x] Unknown values fail closed.
- [x] No label-based semantic trust.
- [x] Existing template APIs remain source-compatible.
- [x] Template tests pass.

---

## Phase 6 - Separate exposure user intent, capability health and dynamic catalog ✅ DONE (2026-09-03)

### Files

- `components/mcp_tool_exposure/include/mcp_tool_exposure.h`
- `components/mcp_tool_exposure/mcp_tool_exposure_internal.h`
- `components/mcp_tool_exposure/mcp_tool_exposure.c`
- `components/mcp_tool_exposure/mcp_tool_catalog.c`
- exposure tests

### Actions

Do **not** add a naive `DISABLED` state.

Use the existing flags byte:

```c
#define MCP_EXP_FLAG_FEATURE_BOUND  (1u << 0)
#define MCP_EXP_FLAG_USER_DISABLED  (1u << 1)
```

Keep `state` as capability health/lifecycle:

```text
ENABLED
NEEDS_REVIEW
ORPHANED
```

Expose explicit user intent:

```c
bool control_enabled;
```

Add:

```c
esp_err_t mcp_tool_exposure_get_feature(...);
esp_err_t mcp_tool_exposure_set_feature_enabled(...);
uint32_t mcp_tool_exposure_get_policy_revision(void);
```

Add independent policy revision:

```c
static uint32_t s_policy_revision;
```

The persisted revision field may retain its binary layout while its semantics become policy revision.

Compact mode:

- no `mcp_tool_catalog_init()`;
- no `s_enabled` allocation;
- no tool-name generation;
- no catalog revision read;
- no dynamic-capacity check.

Reconcile rules:

- never clear `USER_DISABLED` automatically;
- never auto-promote `NEEDS_REVIEW` merely because feature still exists;
- digest change -> `NEEDS_REVIEW` until explicit acceptance;
- dynamic binding exists only when user intent enabled + health enabled + digest valid.

### Tests

- Disabled intent survives save/load/reboot/reconcile.
- Disabled + `NEEDS_REVIEW` survives as two independent dimensions.
- Digest change remains `NEEDS_REVIEW` on later reconcile.
- Explicit enable accepts current digest only after destructive policy succeeds.
- Compact init allocates no executable catalog/enabled array.
- Compact persist reads no catalog revision.
- Dynamic mode publishes only valid enabled grants.
- NVS failure during disable reports failure and runtime revoke behavior is deterministic/documented.
- Device deletion removes policy and dynamic bindings.

### Checklist / Exit criteria

- [x] User intent independent from capability health.
- [x] Disable durable on new firmware.
- [x] `NEEDS_REVIEW` not silently cleared.
- [x] Compact has zero executable-catalog dependency.
- [x] Dynamic compatibility preserved.
- [x] Persistence failures observable.
- [x] Exposure tests pass in both profiles.

---

## Phase 7 - Add `mcp_device_control` ✅ DONE (2026-09-03)

### New file

```text
components/mcp_endpoint/mcp_device_control.c
```

### Files also changed

- `components/mcp_endpoint/CMakeLists.txt`
- `components/mcp_endpoint/mcp_endpoint_internal.h`

### Actions

Use fixed-size semantic context:

```c
typedef enum {
    MCP_DEVICE_OP_DESCRIBE = 0,
    MCP_DEVICE_OP_READ,
    MCP_DEVICE_OP_SET,
} mcp_device_operation_t;

typedef struct {
    mcp_device_operation_t operation;
    char device_id[GW_MSG_DEVICE_ID_LEN];
    char feature_id[GW_FEATURE_ID_LEN];
    uint8_t property_id;
    uint8_t value_type;
} mcp_device_control_context_t;
```

Suggested helpers:

```c
static esp_err_t resolve_device(...);
static esp_err_t resolve_feature(...);
static const device_schema_tool_t *resolve_write_tool(...);
static esp_err_t apply_set_value(...);

mcp_resolve_status_t mcp_device_control_resolve(...);
cJSON *mcp_device_control_execute_local(...);
cJSON *mcp_device_control_format_set_result(...);
```

Device resolution:

1. exact device id;
2. otherwise exact configured name;
3. unique match only;
4. ambiguity fails closed.

Feature resolution:

1. exact feature id;
2. otherwise trusted semantic template name;
3. unique alias only;
4. never peripheral label.

`describe/read` are local.

`set` builds a `device_command_request_t` with:

```text
origin = DEVICE_CMD_ORIGIN_CONTROL
resolved raw command from writable_tool_index
validated bool/int value
```

The MCP formatter consumes `device_command_result_t`; it never parses dispatcher JSON.

### Tests

- Exact/unique/ambiguous device resolution.
- Exact/unique/ambiguous feature resolution.
- Describe hides raw command and untrusted labels.
- Read BOOL from cache.
- Read INT from cache.
- Missing state.
- Unsupported property type.
- Set BOOL/INT.
- Missing/wrong/extra value.
- Range/step validation.
- Read-only set rejection.
- User-disabled rejection.
- `NEEDS_REVIEW` rejection.
- Digest mismatch rejection.
- Internal service request contains correct raw command.
- Public semantic context/result contains no raw command.

### Checklist / Exit criteria

- [x] Semantic resolver isolated from transport code.
- [x] Raw command accepted only from committed schema mapping.
- [x] Describe/read bounded and local.
- [x] Set submits typed service request.
- [x] No `command_executor_submit()` in compact control module.
- [x] No `dispatch_result_t` in compact set formatter.
- [x] No raw command/untrusted label leakage.
- [x] Unit tests pass without HTTP/WS.

---

## Phase 8 - Refactor MCP execution plan around local vs device service ✅ DONE (2026-09-03)

### Files

- `components/mcp_endpoint/mcp_endpoint_internal.h`
- `components/mcp_endpoint/mcp_tools.c`
- `components/mcp_endpoint/mcp_core.c`

### Actions

Replace `gw_message_t + bool is_device_command` routing with explicit execution type:

```c
typedef enum {
    MCP_TOOL_EXEC_GATEWAY_SYNC = 0,
    MCP_TOOL_EXEC_LOCAL,
    MCP_TOOL_EXEC_DEVICE_SERVICE,
} mcp_tool_exec_kind_t;

typedef enum {
    MCP_TOOL_RESPONSE_GATEWAY_DISPATCH = 0,
    MCP_TOOL_RESPONSE_DYNAMIC_DEVICE_LEGACY,
    MCP_TOOL_RESPONSE_DEVICE_CONTROL_SET,
} mcp_tool_response_kind_t;
```

Call plan contains fixed-size request/context only; no owned cJSON crosses async boundary.

Compact resolver order:

```text
get_status/list_devices
device_control
unknown
```

Dynamic resolver order:

```text
get_status/list_devices
dynamic catalog
unknown
```

Routing:

```text
GATEWAY_SYNC   -> existing gateway dispatcher path
LOCAL          -> mcp_device_control_execute_local()
DEVICE_SERVICE -> device_command_service_submit()
```

Async MCP context stores:

- cloned responder;
- JSON-RPC id;
- protocol era;
- response mode;
- fixed semantic/dynamic metadata required by formatter.

### Tests

- Gateway sync routing.
- Local describe/read routing.
- Compact set device-service routing.
- Dynamic device-service routing.
- Responder clone failure.
- Service queue-full admission failure.
- BUSY/timeout/disconnect/device rejection/success mapping.
- Notification executes without response.
- Repeated async operations show no ownership leak/double free.
- Compact semantic formatter never uses legacy dynamic formatter.

### Checklist / Exit criteria

- [x] Routing is explicit.
- [x] No cJSON ownership crosses async service boundary.
- [x] Compact set uses device service.
- [x] Dynamic mode uses legacy-compatible formatter over typed result.
- [x] Notification semantics preserved.
- [x] Routing/error tests pass.

---

## Phase 9 - Register fixed compact MCP surface ✅ DONE (2026-09-03)

### Files

- `components/mcp_endpoint/mcp_registry.c`
- `components/mcp_endpoint/mcp_tools.c`

### Actions

Add `schema_device_control()`.

Compact static registry:

```text
get_status
list_devices
device_control
```

Dynamic static registry:

```text
get_status
list_devices
```

Compact `mcp_tools_list()` must not:

- allocate dynamic binding snapshot;
- call executable catalog;
- depend on number of devices/exposures.

`device_control` annotations must not claim globally read-only/idempotent because `set` mutates device state.

### Tests

- Compact exact tool count = 3.
- Exact tool names.
- Device-control schema contains operation enum describe/read/set.
- No runtime device names/features/raw commands in input schema.
- Dynamic mode exact baseline static+dynamic behavior.
- Add 0/1/8/16 devices and verify compact `tools/list` count/serialized size remains constant.
- Counter/mock proves no catalog snapshot call in compact list.

### Checklist / Exit criteria

- [x] Exactly three compact tools.
- [x] Compact list O(1) versus device count.
- [x] No dynamic catalog access from compact list.
- [x] Dynamic discovery compatibility preserved.
- [x] Xiaozhi can parse compact schema.

---

## Phase 10 - Centralize semantic MCP write policy

### Files

- `components/mcp_endpoint/mcp_policy.c`
- `components/mcp_endpoint/mcp_endpoint_internal.h`

### Actions

Keep legacy raw-command policy only for any remaining legacy administrative/dynamic compatibility needs.

Add semantic feature-control policy:

```c
mcp_policy_result_t mcp_policy_check_feature_control(
    const char *device_id,
    const char *feature_id,
    const device_schema_tool_t *resolved_tool);
```

Checks:

```text
device exists
committed schema exists
feature exists
writable_tool_index valid
exposure control_enabled
health == ENABLED
digest matches
destructive grant allowed
```

Final control request still passes `device_schema_validate_command()` before service submission.

Compact semantic control must not be accidentally blocked by a stale global raw-command allowlist after an explicit semantic grant.

### Tests

- Enabled valid feature allowed.
- User-disabled denied.
- `NEEDS_REVIEW` denied.
- Orphaned denied.
- Digest mismatch denied.
- Read-only denied.
- Invalid writable index denied.
- Destructive default fail-closed.
- Empty stale raw allowlist does not override an explicit valid semantic grant.

### Checklist / Exit criteria

- [ ] One semantic write-policy entry point.
- [ ] Final schema validation remains mandatory.
- [ ] Destructive controls fail closed.
- [ ] Stale raw allowlist is not the compact authority.
- [ ] Policy tests pass.

---

## Phase 11 - Update Web Admin exposure API/UI

### Files

- `components/web_server/web_exposure_api.c`
- `components/web_server/www_src/dashboard/js/features/mcp_exposure.js`
- `components/web_server/www_src/dashboard/js/core/i18n.js`
- `components/web_server/www_src/dashboard/views/device_detail.html`
- Web tests

### Actions

Compact GET response:

- feature-oriented;
- includes `surface_mode`;
- includes `feature_id`;
- includes semantic type/property;
- includes `control_enabled`;
- includes capability health/reason;
- includes policy revision;
- does not require generated tool name or dynamic catalog capacity.

Compact PUT:

- prefer `device_id + feature_id + enabled`;
- require destructive confirmation when necessary;
- keep command-based compatibility only in dynamic mode.

UI:

- display "MCP control" / "AI write control";
- hide generated dynamic tool name in compact mode;
- render health separately from user enable/disable intent.

### Tests

- Compact GET feature-oriented response.
- Compact PUT enable/disable.
- Destructive confirmation required.
- Unknown feature/no committed schema errors.
- NVS failure surfaced.
- Disable survives reconcile/reboot simulation.
- Compact API makes no executable-catalog call.
- Dynamic API regression.
- UI fixture sends feature id on toggle.
- Dashboard asset rebuild succeeds.

### Checklist / Exit criteria

- [ ] Compact Web management is feature-oriented.
- [ ] Policy revision independent from catalog.
- [ ] Disable is visibly durable.
- [ ] Terminology correctly describes write control.
- [ ] Dynamic compatibility retained.
- [ ] Web API/UI tests pass.

---

## Phase 12 - Full qualification and hardware acceptance

### MCP tests

Create/extend:

```text
components/mcp_endpoint/test/test_mcp_device_control.c
components/mcp_endpoint/test/test_mcp_xiaozhi.c
components/mcp_endpoint/test/test_mcp_conformance.c
components/mcp_endpoint/test/test_mcp_stress.c
```

Minimum semantic cases:

1. exact device id;
2. unique configured name;
3. ambiguous device name;
4. exact feature id;
5. unique semantic alias;
6. ambiguous feature alias;
7. describe hides raw commands;
8. read BOOL;
9. read INT;
10. state unavailable;
11. set BOOL;
12. set INT;
13. invalid value type;
14. min/max/step;
15. read-only;
16. user-disabled;
17. needs-review;
18. digest mismatch;
19. service BUSY;
20. service timeout;
21. disconnected device;
22. device rejection;
23. semantic response has no raw command.

### Device-command service tests

Run full Phase 1-3 suite plus prolonged mixed workload:

```text
schema discovery
state seed
Web set
MCP set
multiple devices
ACK loss
disconnect/reconnect
```

### Stress/performance tests

- at least 500 compact `tools/list` calls;
- repeated describe/read;
- repeated async set success/timeout/rejection;
- max supported simultaneous pending devices;
- queue-full behavior;
- no heap drift beyond documented allocator variance;
- record service task high-water mark;
- compare internal RAM/PSRAM against Phase 0 baseline;
- verify executor now has one gateway worker and no device workload.

### Hardware smoke sequence

Using reference LED device:

```text
1. boot gateway
2. add/connect device
3. capability schema reaches READY
4. state seed/request contains led_main + on_off
5. enable led_main MCP write control
6. Xiaozhi tools/list returns exactly 3
7. device_control describe -> led_main/light/on_off
8. device_control set true -> BLE command -> ACK -> LED ON
9. device_state reflects ACK/event
10. device_control read -> cached true
11. disable led_main from Web UI
12. same MCP set -> feature_disabled
13. reboot gateway
14. feature remains disabled
15. disconnect during an accepted command -> prompt semantic not-connected completion
```

### Tests

- Clean compact production build.
- Clean dynamic compatibility build.
- All component/Unity tests.
- Xiaozhi compact integration.
- Dynamic MCP regression.
- Web API/UI regression.
- On-target memory measurements.
- On-target command latency measurements.
- Rollback procedure from Section 14.

### Checklist / Exit criteria

- [ ] All Phase 0-11 tests pass.
- [ ] Compact clean build passes.
- [ ] Dynamic clean build passes.
- [ ] Xiaozhi reports exactly three compact tools.
- [ ] Device command path has no blocking ACK worker.
- [ ] No normal device command uses `command_executor`.
- [ ] No normal device command uses `command_dispatcher`.
- [ ] No raw command appears in compact MCP schema/result/errors.
- [ ] Durable disable/review behavior passes reboot/reconcile tests.
- [ ] Memory results compared against Phase 0 baseline.
- [ ] Service task stack headroom recorded and acceptable.
- [ ] Hardware smoke sequence passes.
- [ ] Any waiver has explicit owner/reason/follow-up.

---

## 7. File-by-file change matrix

| File / component | Required change |
|---|---|
| `components/device_command_service/CMakeLists.txt` | New component registration and dependencies |
| `components/device_command_service/include/device_command_service.h` | Typed request/result/origin/service APIs |
| `components/device_command_service/device_command_service.c` | Event loop, pending table, request id, BLE send, ACK/timeout/disconnect |
| `components/device_command_service/test/*` | Correlation, timeout, concurrency, origin-validation tests |
| `main/main.c` | Init service; route notify/disconnect; schema submit adapter |
| `components/device_schema/*` | Preserve capability/discovery ownership; migrate submit transport adapter only |
| `components/device_state/device_state.c` | Use service for internal state read; include feature/property |
| `components/device_state/CMakeLists.txt` | Replace executor dependency with service where applicable |
| `components/command_dispatcher/command_dispatcher.c` | Remove device-command branch after migration |
| `components/command_dispatcher/device_command.c` | Delete after all callers migrate |
| `components/command_dispatcher/device_request_manager.c` | Delete; pending ownership moves to service |
| `components/command_dispatcher/CMakeLists.txt` | Remove deleted device transport sources/dependencies |
| `components/command_executor/Kconfig.projbuild` | Default one worker; describe gateway-admin-only role |
| `components/command_executor/*` | Retain for gateway jobs only; update docs/tests |
| `components/web_server/web_command_api.c` | Device calls -> service typed adapter |
| `components/web_server/web_device_api.c` | Device calls -> service typed adapter |
| `components/mcp_endpoint/Kconfig.projbuild` | Compact/dynamic surface choice |
| `components/mcp_endpoint/CMakeLists.txt` | Add `device_command_service`, `device_state`, `mcp_device_control.c` |
| `components/mcp_endpoint/mcp_endpoint_internal.h` | Explicit execution/response plan types |
| `components/mcp_endpoint/mcp_registry.c` | Compact 3-tool registry; dynamic 2-static registry |
| `components/mcp_endpoint/mcp_tools.c` | Constant compact list; service routing for dynamic calls |
| `components/mcp_endpoint/mcp_core.c` | Async completion over typed service result |
| `components/mcp_endpoint/mcp_device_control.c` | New semantic resolver/local execution/set formatter |
| `components/mcp_endpoint/mcp_policy.c` | Semantic feature write policy |
| `components/device_template/*` | Feature/property/value-kind semantic vocabulary |
| `components/mcp_tool_exposure/include/mcp_tool_exposure.h` | Explicit `control_enabled`, feature APIs, policy revision |
| `components/mcp_tool_exposure/mcp_tool_exposure_internal.h` | Persist `USER_DISABLED` in existing flags byte |
| `components/mcp_tool_exposure/mcp_tool_exposure.c` | Separate intent/health/catalog; compact no catalog |
| `components/mcp_tool_exposure/mcp_tool_catalog.c` | Dynamic-only runtime; compact guard tests |
| `components/web_server/web_exposure_api.c` | Feature-oriented compact policy API |
| `components/web_server/www_src/.../mcp_exposure.js` | Feature toggle and compact rendering |
| `components/web_server/www_src/.../i18n.js` | Write-control terminology |
| `test/sdkconfig.defaults` | Select compact profile; remove stale MCP flags |
| `docs/MCP_API.md` | Document service-backed compact semantic surface |
| relevant READMEs | Remove outdated dispatcher/executor/device-tool architecture claims |

---

## 8. Components retained vs removed

## 8.1 Retain

```text
device_schema
device_state
device_template
ble_central
mcp_tool_exposure
mcp_tool_catalog (dynamic build only)
command_dispatcher (gateway admin)
command_executor (gateway admin, one worker initially)
```

## 8.2 Remove after Phase 3

```text
command_dispatcher/device_command.c
command_dispatcher/device_request_manager.c
```

If build structure makes immediate deletion disruptive, keep temporary compatibility wrappers for one commit only, then remove them before foundation PR completion.

## 8.3 Intentionally unchanged at wire level

```text
GW_PROTOCOL_VERSION
Protocol v4 request_id semantics
capabilities_begin/item/feature_item/end
feature_state event format
read_feature_state command identifier
```

---

## 9. Error contracts

## 9.1 Device service status

| Condition | Service status |
|---|---|
| malformed request | `INVALID_ARGUMENT` |
| unsupported CONTROL command | `UNSUPPORTED_COMMAND` |
| pending command already exists for device | `BUSY` |
| pending table/queue cannot admit | synchronous submit error / resource exhausted mapping |
| device disconnected before send | `NOT_CONNECTED` |
| BLE send failure | `TRANSPORT_ERROR` |
| no matching ACK before deadline | `TIMEOUT` |
| device ACK rejects command | `DEVICE_REJECTED` |
| accepted ACK | `OK` |

## 9.2 Compact MCP semantic errors

Recommended stable error identifiers/messages:

```text
device_not_found
ambiguous_device
capabilities_not_ready
feature_not_found
ambiguous_feature
unsupported_property
feature_read_only
feature_disabled
capability_changed
state_not_available
invalid_value
busy
not_connected
timeout
device_rejected
```

Malformed MCP arguments remain JSON-RPC invalid params (`-32602`) where appropriate.

Do not include the internal raw command in compact errors.

## 9.3 Web mapping

Web adapters map typed service status to existing public HTTP semantics. The service itself must not know HTTP status codes.

---

## 10. Memory and performance budget

## 10.1 Current executor cost to measure

Default current configuration includes approximately:

```text
2 x command worker stack @ 4096 bytes
2 x persistent dispatch_result_t (~4 KB each)
executor queue
2 worker TCBs
pending table
1 binary semaphore per pending entry
```

Exact allocator placement and structure alignment must be measured on the ESP32-S3 build; do not use estimates as acceptance data.

## 10.2 v1.3 target

Device path:

```text
1 x device command service task
small bounded event queue
small typed pending table
no per-pending semaphore
no per-device-command 4 KB result buffer
```

Gateway admin path:

```text
1 x command executor worker
1 x persistent dispatch_result_t
```

Expected qualitative savings compared with baseline:

- at least one 4096-byte persistent executor stack removed;
- at least one ~4 KB persistent executor result buffer removed;
- pending binary semaphores removed;
- fewer context switches under multiple in-flight BLE devices;
- no blocked executor worker per pending BLE ACK.

If later qualification proves gateway-admin operations can execute safely without executor, a separate cleanup can remove the remaining worker/result buffer. That is outside v1.3 DoD.

## 10.3 Acceptance metrics

Record before/after:

```text
internal free heap
internal minimum free heap
internal largest free block
PSRAM free
PSRAM minimum free
PSRAM largest free block
service task stack high-water mark
executor task stack high-water mark
average/p95 command completion latency
max pending count
queue high-water mark
```

No phase may claim memory improvement without on-target numbers.

---

## 11. Security, safety and concurrency invariants

1. Compact MCP never accepts a raw peripheral command from user arguments.
2. Semantic write mapping comes from committed schema `writable_tool_index` only.
3. CONTROL requests pass `device_schema_validate_command()` at the final trust boundary before BLE send.
4. SCHEMA_DISCOVERY origin accepts only `describe_capabilities`.
5. STATE_READ origin accepts only `read_feature_state` with validated feature/property addressing.
6. MCP write requires durable control grant + valid health + matching digest.
7. Destructive capability fails closed unless explicit configured policy and confirmation allow it.
8. Device/feature name ambiguity fails closed.
9. One pending command per device remains enforced.
10. ACK must match live request id/device/command.
11. Completion callback runs exactly once.
12. BLE callback does not directly perform HTTP/MCP response formatting.
13. Device service has no cJSON/HTTP/MCP dependency.
14. Compact result/error never leaks raw command or untrusted label.
15. Disconnect should fail pending command promptly rather than waiting full timeout.
16. Dynamic compatibility mode may preserve legacy raw identities but must not weaken compact-mode policy.

---

## 12. Recommended commit sequence

Foundation branch:

```text
1. test(command): characterize blocking device command baseline
2. feat(device-command): add event-driven device command service
3. feat(schema-state): migrate internal device commands to service
4. refactor(web-mcp): migrate legacy device callers to service
5. refactor(dispatcher): remove device transport and pending manager
6. config(executor): reduce executor to one gateway-admin worker
```

Compact MCP branch:

```text
7. config(mcp): add compact and dynamic surface modes
8. feat(template): expand trusted semantic feature/property vocabulary
9. refactor(exposure): separate user intent, health and executable catalog
10. feat(mcp): add semantic device_control resolver
11. refactor(mcp): route device calls through device command service
12. feat(mcp): expose fixed three-tool compact surface
13. feat(mcp): centralize semantic write policy
14. feat(web): manage compact feature write-control grants
15. test(mcp): add Xiaozhi/conformance/stress/memory qualification
16. docs: document event-driven command path and compact semantic MCP
```

Keep each commit buildable whenever practical.

---

## 13. Build and test matrix

## 13.1 Compact production profile

```text
CONFIG_MCP_TOOL_SURFACE_COMPACT=y
CONFIG_MCP_TOOL_SURFACE_DYNAMIC=n
CONFIG_CMD_EXEC_WORKER_COUNT=1
```

Expected:

```text
MCP tools = 3
device command -> device_command_service
executor -> gateway admin only
no dynamic executable catalog allocation
```

## 13.2 Dynamic compatibility profile

```text
CONFIG_MCP_TOOL_SURFACE_COMPACT=n
CONFIG_MCP_TOOL_SURFACE_DYNAMIC=y
CONFIG_CMD_EXEC_WORKER_COUNT=1
```

Expected:

```text
MCP tools = 2 static + enabled dynamic
dynamic device command -> device_command_service
dynamic executable catalog enabled
executor -> gateway admin only
```

## 13.3 Required suites

```text
device_command_service tests
device_schema tests
device_state tests
command_dispatcher gateway-admin tests
command_executor gateway-admin tests
mcp_tool_exposure compact tests
mcp_tool_exposure dynamic tests
mcp_endpoint tests
mcp_conformance tests
mcp_xiaozhi tests
mcp_stress tests
web API tests
Web asset build
on-target smoke/memory tests
```

---

## 14. Migration and rollback

## 14.1 Runtime migration

The foundation refactor changes internal execution architecture only. Protocol v4 remains unchanged, so existing devices do not require firmware upgrade merely to use `device_command_service`.

The compact exposure policy migration reuses the persisted flags byte for `USER_DISABLED` and should preserve blob layout where possible.

## 14.2 Rollback warning

Rollback from firmware that persists `USER_DISABLED` to an older firmware that does not understand/preserve that intent is **not policy-safe**. Older semantic reconcile code may recreate/re-enable a feature.

Before rollback to pre-v1.3 firmware, use one of:

```text
- administratively disable external MCP/Xiaozhi control;
- erase/migrate MCP exposure policy knowingly;
- use a preparatory compatibility firmware that understands the new flag.
```

Do not document old-firmware rollback as fail-closed unless that exact old build has been tested to honor the new persisted semantics.

## 14.3 Foundation rollback

The event-driven service foundation should be independently revertible before compact MCP changes. This is another reason to deliver it as a separate PR/branch.

---

## 15. Definition of done

Foundation:

- [ ] Device commands do not block a command-executor worker waiting for ACK.
- [ ] Normal device command path bypasses `command_executor`.
- [ ] Normal device command path bypasses `command_dispatcher`.
- [ ] `device_request_manager` removed/absorbed.
- [ ] One-pending-per-device and strict ACK correlation preserved.
- [ ] Schema discovery uses service.
- [ ] State read seed includes feature/property and uses service.
- [ ] Disconnect completes pending request promptly.
- [ ] Executor reduced to one gateway-admin worker.
- [ ] Foundation memory/latency measurements recorded.

Compact MCP:

- [ ] Exactly three compact tools.
- [ ] Compact `tools/list` size/count independent of device count.
- [ ] Compact mode allocates no executable dynamic catalog.
- [ ] Describe returns trusted semantic metadata only.
- [ ] Read returns typed cached state or deterministic error.
- [ ] Set resolves semantic feature to schema command and uses device service.
- [ ] Compact result/error never includes raw command.
- [ ] Durable user-disabled intent survives reboot/reconcile.
- [ ] Digest change forces review.
- [ ] Destructive command remains fail-closed.
- [ ] Xiaozhi integration passes.
- [ ] Dynamic compatibility passes.
- [ ] Web feature-control UI/API passes.
- [ ] On-target ESP32-S3 memory and stack qualification passes.
- [ ] Documentation updated.

---

## Appendix A - Target compact set call flow

```text
Xiaozhi
  |
  | tools/call device_control
  | device="Living Room Lamp"
  | operation="set"
  | feature="led_main"
  | bool_value=true
  v
mcp_core
  |
  v
mcp_device_control
  |
  +--> resolve device_store
  +--> resolve committed device_schema feature
  +--> resolve writable_tool_index
  +--> check exposure policy/digest
  +--> validate BOOL/range rules
  |
  v
device_command_request_t
  origin=CONTROL
  device_id=...
  command=set_led       [internal only]
  bool_value=true
  |
  v
device_command_service_submit
  |
  +--> final schema validation
  +--> allocate request_id
  +--> pending slot
  +--> BLE send
  +--> return immediately

... BLE ACK ...

main.on_device_notify
  |
  +--> device_state_on_command_ack
  |
  +--> device_command_service_on_notify
             |
             v
       service event loop
             |
             +--> correlate request_id/device/command
             +--> free pending slot
             +--> typed completion
             |
             v
       MCP semantic formatter
             |
             v
       { device_id, feature_id, success, value? }

No raw command is returned to Xiaozhi.
```

---

## Appendix B - What was removed from the old device path

```text
OLD:
MCP/Web
 -> command_executor queue
 -> worker task
 -> command_dispatcher
 -> device_command_handle
 -> pending semaphore
 -> block worker
 -> dispatch_result_t 4 KB
 -> formatter

NEW:
MCP/Web
 -> device_command_service event
 -> BLE send
 -> ACK event
 -> small typed result
 -> formatter
```

`device_schema` and `device_state` remain modules; they are no longer conceptual "pipeline hops" for every command.

---

## Appendix C - Review hotspots during implementation

Review these areas especially carefully:

1. ACK event copy lifetime: never retain a pointer to BLE callback-owned message memory.
2. Completion callback lifetime/context ownership for Web and MCP responders.
3. Service queue saturation and pending-slot rollback on send failure.
4. Timeout vs ACK race: exactly one completion must win.
5. Disconnect vs ACK race: exactly one completion must win.
6. Request-id wrap/collision behavior.
7. `device_state_on_command_ack()` ordering before semantic completion reads cached state.
8. Schema discovery messages must still be consumed by schema before generic ACK processing.
9. Compact policy disable must not be undone by reconcile.
10. Dynamic compatibility output must not force compact mode to reintroduce raw-command leakage.
11. Do not create a circular component dependency between `device_schema` and `device_command_service`; preserve submitter/adaptor direction.
12. Do not move HTTP/cJSON formatting into the new service merely for convenience.
13. Re-measure service task stack instead of assuming a smaller stack is safe.

---

## Appendix D - Optional follow-up after v1.3

Not part of v1.3 acceptance:

- Generalize `esp-ble-device` active `read_feature_state` from BOOL-only to typed BOOL/INT reads.
- Remove the remaining gateway-admin executor if profiling proves all admin operations can be safely redesigned asynchronously.
- Replace generic gateway `gw_message_t` admin dispatcher with narrower typed gateway service APIs.
- Add per-read-feature privacy policy if sensor read visibility needs separate authorization from write control.
