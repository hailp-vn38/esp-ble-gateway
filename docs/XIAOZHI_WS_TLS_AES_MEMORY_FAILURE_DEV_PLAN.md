# Xiaozhi WebSocket TLS TX Memory Failure — Development & Remediation Plan

**Project:** `esp-ble-gateway`  
**Repository:** `https://github.com/hailp-vn38/esp-ble-gateway`  
**Primary component:** `components/mcp_ws_bridge`  
**Related components:** `memory_policy`, `mcp_core`, `mcp_tool_exposure`, `web_server`  
**Target:** ESP32-S3 / 16 MB Flash / 8 MB Octal PSRAM  
**Framework:** ESP-IDF v6.1-rc1  
**Document status:** Implementation specification  
**Version:** 1.0

---

# 1. Problem statement

Khi user bấm **Reconnect Xiaozhi** trong Web Settings, gateway có thể reconnect thành công, hoàn tất MCP handshake và chuyển sang READY, nhưng sau đó thất bại khi TX qua TLS:

```text
I (...) mcp_ws_bridge: External MCP session ready (2024-11-05)
E (...) esp-aes: Failed to allocate memory
E (...) esp-tls-mbedtls: write error :-0x0084
E (...) transport_base: esp_tls_conn_write error, errno=Success
W (...) mcp_ws_bridge: WebSocket TX failed
```

Điểm quan trọng:

- TLS handshake đã thành công.
- WebSocket đã connected.
- MCP `initialize` đã thành công.
- MCP session đã vào `READY`.
- Lỗi xuất hiện khi gateway gửi WebSocket TX sau READY.

Do đó lỗi không phải MCP handshake failure mà là **TLS TX / AES memory allocation failure**.

---

# 2. Current architecture relevant to the issue

`mcp_ws_bridge` hiện sử dụng:

```c
esp_websocket_client
    ↓
esp-tls
    ↓
mbedTLS
    ↓
ESP32-S3 AES hardware
```

WebSocket client config hiện có các giá trị đáng chú ý:

```c
.task_stack = 6144,
.buffer_size = 2048,
```

Bridge worker sử dụng:

```text
CONFIG_MCP_WS_TASK_STACK=8192
CONFIG_MCP_WS_MAX_RX_MESSAGE=8192
CONFIG_MCP_WS_MAX_TX_MESSAGE=32768
CONFIG_MCP_WS_EVENT_QUEUE_DEPTH=8
```

TX hiện được thực hiện bằng:

```c
esp_websocket_client_send_text(
    client,
    event->payload,
    event->payload_len,
    ...
);
```

Tức toàn bộ JSON MCP response có thể được gửi trong một lần gọi.

---

# 3. Root cause hypothesis

## 3.1 Primary hypothesis

AES hardware TX cần một block:

```text
internal
+
DMA-capable
+
contiguous
```

nhưng allocator không thể cấp block phù hợp tại thời điểm TLS record được mã hóa.

Điều này có thể xảy ra ngay cả khi PSRAM vẫn còn nhiều MB.

Ví dụ:

```text
PSRAM free             = 6 MB
Internal free          = 40 KB
DMA free               = 25 KB
DMA largest free block = 1.5 KB
```

Trong trường hợp này AES allocation vẫn có thể fail.

---

## 3.2 Why reconnect increases the probability

Reconnect flow hiện tại:

```text
POST /api/settings/xiaozhi/reconnect
        ↓
mcp_ws_bridge_reload()
        ↓
BRIDGE_EVENT_RELOAD
        ↓
destroy_client()
        ↓
esp_websocket_client_stop()
        ↓
esp_websocket_client_destroy()
        ↓
queue CONNECT
        ↓
new WebSocket client
        ↓
new TLS session
        ↓
MCP initialize
        ↓
READY
```

Reconnect tạo một giai đoạn memory churn lớn:

```text
TLS old session cleanup
+
new WebSocket task/buffer
+
new TLS allocation
+
new TCP/lwIP allocation
+
new MCP JSON allocations
```

Ngay sau đó Xiaozhi thường gọi lại:

```text
tools/list
```

để lấy registry mới.

Nếu tool registry vừa tăng do:

```text
add device
+
enable MCP command exposure
```

thì response `tools/list` cũng lớn hơn.

---

# 4. Most likely failure sequence

```text
User adds device
        ↓
MCP tool registry grows
        ↓
User presses Reconnect Xiaozhi
        ↓
old WS client destroyed
        ↓
new WS/TLS connection
        ↓
MCP initialize
        ↓
notifications/initialized
        ↓
READY
        ↓
Xiaozhi sends tools/list
        ↓
gateway builds large JSON response
        ↓
bridge queues TX_MESSAGE
        ↓
esp_websocket_client_send_text(...)
        ↓
TLS tries to encrypt record
        ↓
AES requests internal DMA memory
        ↓
allocation fails
        ↓
esp_tls_conn_write fails
```

---

# 5. Current memory configuration

Production defaults currently include:

```ini
CONFIG_SPIRAM=y
CONFIG_SPIRAM_USE_MALLOC=y

CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=1024
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=65536

CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y
```

This is generally correct.

`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y` moves many mbedTLS allocations to PSRAM.

However it does **not guarantee** that every AES/DMA-related allocation can use PSRAM.

Therefore the current failure must be treated as:

> Internal DMA-capable memory pressure or fragmentation.

---

# 6. Development goals

This work must achieve all of the following:

1. Prove the exact memory condition at TX failure.
2. Determine whether the trigger is:
   - large TX;
   - reconnect fragmentation;
   - memory leak;
   - insufficient internal reserve;
   - combination of the above.
3. Make WebSocket TX robust for large `tools/list`.
4. Make TX failure invalidate the current MCP session.
5. Ensure reconnect does not progressively fragment/leak memory.
6. Preserve stable BLE operation with up to the project target connection count.
7. Avoid blindly increasing internal memory reservation without measurement.

---

# 7. Phase 0 — Reproduce and establish baseline

## Objective

Create a reproducible test before changing memory behavior.

## Test scenario

Use:

```text
Wi-Fi connected
Xiaozhi enabled
BLE devices connected
MCP tools exposed
Web UI open
```

Test:

```text
1. Boot gateway.
2. Wait until Xiaozhi READY.
3. Add/expose device tools.
4. Press Reconnect Xiaozhi.
5. Wait for READY.
6. Observe tools/list TX.
7. Record failure.
```

## Checklist

- [ ] Record boot free internal heap.
- [ ] Record boot largest internal block.
- [ ] Record boot DMA free.
- [ ] Record boot DMA largest block.
- [ ] Record PSRAM free.
- [ ] Record number of BLE links.
- [ ] Record number of MCP tools.
- [ ] Record serialized `tools/list` response size.
- [ ] Reproduce failure at least 3 times.
- [ ] Record whether failure occurs on first reconnect or only after repeated reconnect.

## Acceptance

A reproducible baseline exists.

---

# 8. Phase 1 — Memory instrumentation

Add instrumentation to `components/mcp_ws_bridge/mcp_ws_bridge.c`.

Include:

```c
#include "esp_heap_caps.h"
```

Add helper:

```c
static void log_memory_snapshot(const char *tag)
{
    ESP_LOGI(TAG,
             "[MEM:%s] INT free=%u largest=%u | "
             "DMA free=%u largest=%u | "
             "PSRAM free=%u largest=%u",
             tag,
             (unsigned)heap_caps_get_free_size(
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(
                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(
                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}
```

Development builds only.

---

# 9. Required instrumentation points

Capture snapshots at:

```text
bridge init completed
before destroy_client
after destroy_client
before connect_client
after WS connected
after MCP READY
before TX
after successful TX
after failed TX
```

Before TX log:

```c
ESP_LOGI(TAG,
         "WS TX len=%u generation=%u state=%s",
         (unsigned)event->payload_len,
         (unsigned)event->generation,
         mcp_ws_bridge_state_name(current_state));
```

---

# 10. Diagnostic classifications

## Case A — large TX failure

Example:

```text
TX len=9100
DMA free=30000
DMA largest=1400
```

Classification:

```text
large TLS TX
+
DMA fragmentation
```

Primary remediation:

```text
fragment WebSocket message
```

---

## Case B — reconnect degradation

Example after repeated reconnect:

```text
Reconnect 1: DMA largest=12000
Reconnect 2: DMA largest=9000
Reconnect 3: DMA largest=6000
Reconnect 4: DMA largest=2500
```

Classification:

```text
reconnect leak or fragmentation
```

Primary remediation:

```text
audit destroy lifecycle
+
delay reconnect
+
verify task/client cleanup
```

---

## Case C — low baseline DMA headroom

Example:

```text
before first reconnect:
DMA largest=1800
```

Classification:

```text
baseline internal memory policy insufficient
```

Primary remediation:

```text
memory policy tuning
+
move eligible consumers to PSRAM
```

---

# 11. Phase 2 — TX failure handling

Current behavior:

```c
if (sent != (int)event->payload_len) {
    ESP_LOGW(TAG, "WebSocket TX failed");
}
```

This is insufficient.

After `esp_tls_conn_write()` fails, the current TLS/WebSocket connection must be considered unhealthy.

New behavior:

```text
TX failure
    ↓
record status.last_error
    ↓
invalidate MCP generation
    ↓
destroy WebSocket client
    ↓
schedule reconnect/backoff
```

---

# 12. Proposed TX error path

Pseudo implementation:

```c
static void handle_tx_failure(int sent, size_t expected)
{
    ESP_LOGW(TAG,
             "WebSocket TX failed: sent=%d expected=%u",
             sent,
             (unsigned)expected);

    log_memory_snapshot("tx_fail");

    xSemaphoreTake(s_bridge.lock, portMAX_DELAY);

    s_bridge.status.last_error = ESP_FAIL;
    invalidate_connection_locked();

    xSemaphoreGive(s_bridge.lock);

    destroy_client();
    schedule_reconnect();
}
```

Do not keep:

```text
state = READY
```

after TLS write failure.

---

# 13. Avoid duplicate disconnect processing

Important:

`destroy_client()` may cause WebSocket callbacks such as:

```text
DISCONNECTED
CLOSED
ERROR
```

These callbacks can enqueue `BRIDGE_EVENT_WS_DISCONNECTED`.

The bridge must ignore stale events by:

```text
client identity
+
generation
```

The existing generation model should remain the source of truth.

Required test:

```text
TX fail
→ destroy
→ callback queued
→ reconnect
→ old callback must not destroy new connection
```

---

# 14. Phase 3 — Large WebSocket TX fragmentation

This is the recommended primary fix if `tools/list` is confirmed large.

## Goal

Avoid sending a large MCP JSON payload as one TLS/WebSocket TX operation.

---

# 15. Protocol requirement

One MCP JSON-RPC response must remain:

```text
one WebSocket MESSAGE
```

It may be fragmented into multiple WebSocket FRAMES.

Do not do:

```text
JSON part 1 = WS message 1
JSON part 2 = WS message 2
```

That would corrupt MCP transport semantics.

Correct:

```text
TEXT frame, FIN=0
CONT frame, FIN=0
CONT frame, FIN=0
CONT frame, FIN=1
```

---

# 16. Proposed TX fragment size

Start A/B testing with:

```text
1536 bytes
```

and:

```text
2048 bytes
```

Recommended first implementation:

```text
CONFIG_MCP_WS_TX_FRAGMENT_SIZE=1536
```

Kconfig:

```text
config MCP_WS_TX_FRAGMENT_SIZE
    int "WebSocket TX fragment size"
    range 512 4096
    default 1536
    depends on MCP_WS_BRIDGE
```

Do not assume 1536 is final until tested.

---

# 17. Fragment threshold

Suggested:

```text
payload <= fragment_size
    → current send_text path

payload > fragment_size
    → fragmented WebSocket send
```

This keeps small MCP responses simple.

---

# 18. ESP WebSocket API verification requirement

Before implementation, verify the ESP-IDF v6.1-rc1 `esp_websocket_client` API available in this build supports:

```text
partial text frame
continuation frame
final continuation frame
```

Do not invent private WebSocket framing.

If public client APIs do not support correct fragmentation, evaluate:

1. upgrading managed `esp_websocket_client`;
2. using supported raw/partial send API;
3. reducing TLS record size as an alternative.

---

# 19. TX buffer ownership

Current responder creates:

```text
copy = malloc(len + 1)
```

then places the whole response into queue.

For large `tools/list`, this itself contributes memory pressure.

Long-term option:

```text
PSRAM-backed TX payload
```

Use `memory_policy`:

```c
gw_mem_alloc(
    len + 1,
    GW_MEM_EXTERNAL_PREFERRED
);
```

for large TX payloads.

Recommended rule:

```text
len <= 1024
    → normal/default allocation

len > 1024
    → external-preferred
```

This should be tested, not blindly applied to every allocation.

---

# 20. Phase 4 — Use memory_policy for bridge buffers

The project already contains:

```text
components/memory_policy
```

Use it consistently for memory that does not require internal RAM.

Candidate allocations:

```text
mcp_ws_bridge RX buffer
large RX message copy
large TX payload copy
temporary serialized MCP response
```

Do not move to PSRAM:

```text
FreeRTOS objects
DMA-required buffers
driver-required structures
AES internal DMA allocations
```

---

# 21. RX buffer migration

Current:

```c
s_bridge.rx_buffer =
    malloc(CONFIG_MCP_WS_MAX_RX_MESSAGE + 1);
```

Candidate:

```c
s_bridge.rx_buffer =
    gw_mem_alloc(
        CONFIG_MCP_WS_MAX_RX_MESSAGE + 1,
        GW_MEM_EXTERNAL_PREFERRED
    );
```

This can free approximately:

```text
8 KB
```

of potentially useful internal heap depending on allocator behavior.

Required tests:

- [ ] fragmented RX still correct;
- [ ] parsing works from PSRAM buffer;
- [ ] no regression in latency;
- [ ] no DMA requirement exists for RX buffer.

---

# 22. TX copy migration

Current:

```c
char *copy = malloc(len + 1);
```

Candidate:

```c
char *copy =
    gw_mem_alloc(
        len + 1,
        len > CONFIG_MCP_WS_PSRAM_TX_THRESHOLD
            ? GW_MEM_EXTERNAL_PREFERRED
            : GW_MEM_DEFAULT
    );
```

Add Kconfig only if needed:

```text
CONFIG_MCP_WS_PSRAM_TX_THRESHOLD=1024
```

---

# 23. Phase 5 — Reconnect cooldown

Current reload:

```text
destroy client
→ queue CONNECT immediately
```

Add a short reconnect cooldown.

Recommended initial value:

```text
250 ms
```

A/B test:

```text
0 ms
250 ms
500 ms
```

Goal:

- allow WebSocket task teardown;
- allow TCP/lwIP cleanup;
- allow TLS allocations to be released;
- reduce immediate heap fragmentation peak.

---

# 24. Recommended implementation

Do not use:

```c
vTaskDelay(...)
```

inside generic event callbacks.

Prefer:

```text
esp_timer
```

or reuse the existing reconnect timer.

For manual reload:

```text
destroy_client()
    ↓
schedule reconnect timer after RELOAD_COOLDOWN_MS
```

Add:

```text
CONFIG_MCP_WS_RELOAD_COOLDOWN_MS
```

Recommended:

```text
default 250
range 0 2000
```

---

# 25. Phase 6 — mbedTLS memory tuning

Only after Phase 1 metrics exist.

Evaluate:

```ini
CONFIG_MBEDTLS_DYNAMIC_BUFFER=y
CONFIG_MBEDTLS_DYNAMIC_FREE_CONFIG_DATA=y
CONFIG_MBEDTLS_DYNAMIC_FREE_CA_CERT=y
```

Purpose:

```text
reduce retained TLS memory after handshake
```

Do not enable without:

- WSS reconnect test;
- certificate bundle validation;
- OTA HTTPS validation if OTA also uses mbedTLS;
- repeated connection test.

---

# 26. Phase 7 — Wi-Fi/lwIP PSRAM A/B test

Candidate:

```ini
CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y
```

Goal:

```text
free more internal RAM for BLE + DMA + TLS/AES
```

This must be treated as an A/B experiment.

Test matrix:

```text
9 BLE links
+
Web UI traffic
+
Xiaozhi WSS
+
repeated tools/list
+
OTA / HTTP if applicable
```

Do not make production default until stability is proven.

---

# 27. Do not immediately increase internal reserve

Current:

```ini
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=65536
```

Do not immediately change to:

```text
96 KB
128 KB
```

without measurement.

Increasing the reserve may help AES but can starve:

```text
NimBLE
FreeRTOS
Wi-Fi
application stacks
```

Internal reserve tuning is Phase 8, not Phase 1.

---

# 28. Phase 8 — Internal reserve tuning

Only if previous fixes are insufficient.

A/B test:

```text
64 KB
80 KB
96 KB
```

Measure:

```text
DMA largest block
internal largest block
BLE stability
WSS stability
Web UI latency
```

Production setting must be selected by measured stability.

---

# 29. Required tool-size instrumentation

The problem likely correlates with `tools/list`.

Add metrics:

```text
tool_count
tools_list_json_bytes
```

Example:

```text
MCP tools/list: tools=17 json=6842 bytes
```

This log should exist in debug/development builds.

Goal:

```text
correlate TX size with AES failure
```

---

# 30. Suggested warning thresholds

Development diagnostics:

```text
TX > 2048 bytes
    → DEBUG/INFO large TX

TX > 8192 bytes
    → WARN large MCP response
```

Do not reject large valid MCP responses solely because of the warning.

---

# 31. Phase 9 — Reconnect leak audit

Run:

```text
100 manual reconnect cycles
```

Record after every cycle:

```text
free internal
largest internal
free DMA
largest DMA
free PSRAM
task count
```

Acceptance:

Values may fluctuate but must not show a monotonic downward trend.

---

# 32. Task leak checks

Ensure reconnect does not create multiple permanent:

```text
mcp_ws_client
```

tasks.

The bridge itself should remain one:

```text
mcp_ws_bridge
```

task.

After each client destroy:

```text
old websocket client task must terminate
```

Add development-only task count diagnostics if required.

---

# 33. Heap integrity checks

Development builds may use:

```c
heap_caps_check_integrity_all(true);
```

at selected points:

```text
after destroy_client
after reconnect READY
after TX failure
```

Do not leave heavy integrity checking enabled in production hot paths.

---

# 34. Memory leak detection mode

For dedicated test builds, enable heap tracing if supported by the current ESP-IDF configuration.

Use it around:

```text
connect
READY
tools/list
disconnect
reconnect
```

Goal:

identify allocations that survive a complete connection cycle unexpectedly.

---

# 35. Phase 10 — Functional regression matrix

## MCP

- [ ] initialize succeeds.
- [ ] notifications/initialized succeeds.
- [ ] tools/list small registry succeeds.
- [ ] tools/list large registry succeeds.
- [ ] tools/call succeeds.
- [ ] large tool response succeeds.
- [ ] reconnect updates tools.

---

## WebSocket

- [ ] normal connect.
- [ ] manual reconnect.
- [ ] network loss reconnect.
- [ ] repeated reconnect.
- [ ] TX failure recovery.
- [ ] stale disconnect event ignored.

---

## BLE

- [ ] 1 BLE link.
- [ ] 5 BLE links.
- [ ] target maximum BLE links.
- [ ] BLE notifications while tools/list is sent.
- [ ] BLE command execution while Xiaozhi connected.

---

## Web UI

- [ ] Settings remains responsive during WSS reconnect.
- [ ] dashboard static assets still load.
- [ ] reconnect button does not cause HTTP server starvation.

---

# 36. Stress test matrix

Minimum:

| Test | Repetitions |
|---|---:|
| Manual reconnect | 100 |
| `tools/list` | 500 |
| Network disconnect/reconnect | 50 |
| Large MCP TX | 500 |
| BLE command while WSS active | 500 |
| Web Settings refresh | 200 |

---

# 37. Acceptance memory criteria

The exact numerical threshold should be finalized from measured baseline.

However the following qualitative criteria are mandatory:

```text
No AES allocation failure
No monotonic internal heap loss
No monotonic DMA largest-block degradation
No WebSocket task leak
No MCP responder leak
No BLE instability introduced
```

---

# 38. Suggested implementation order

```text
Phase 0
Reproduce baseline
    ↓
Phase 1
Memory + TX instrumentation
    ↓
Phase 2
TX failure recovery
    ↓
Phase 3
Fragment large WebSocket TX
    ↓
Phase 4
Move eligible bridge buffers to PSRAM
    ↓
Phase 5
Reconnect cooldown
    ↓
Phase 6
mbedTLS dynamic memory A/B
    ↓
Phase 7
Wi-Fi/lwIP PSRAM A/B
    ↓
Phase 8
Internal reserve tuning
    ↓
Phase 9
100-cycle leak audit
    ↓
Phase 10
Regression + stress tests
```

Do not start with sdkconfig tuning before obtaining Phase 1 measurements.

---

# 39. Recommended first implementation batch

The first PR/implementation batch should contain only:

1. Memory snapshot helper.
2. TX payload length diagnostics.
3. Tool-list response-size diagnostics.
4. Proper TX failure recovery.
5. Reconnect cooldown.
6. Tests for stale disconnect events.

Do **not** combine all memory-policy changes in the first batch.

Reason:

```text
measurement must remain attributable
```

---

# 40. Recommended second implementation batch

After confirming the failure correlates with large TX:

1. Add WebSocket fragmentation support.
2. Add TX fragment-size Kconfig.
3. Add large TX tests.
4. Verify `tools/list` > 8 KB.
5. Run 100 reconnect cycles.

---

# 41. Recommended third implementation batch

Only if still needed:

1. Move RX buffer to `memory_policy`.
2. Move large TX copies to PSRAM.
3. Enable/test mbedTLS dynamic buffer.
4. A/B `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`.
5. Re-evaluate internal reserve.

---

# 42. Code areas expected to change

## Primary

```text
components/mcp_ws_bridge/mcp_ws_bridge.c
components/mcp_ws_bridge/Kconfig.projbuild
components/mcp_ws_bridge/CMakeLists.txt
```

---

## Memory policy integration

```text
components/memory_policy/memory_policy.c
components/memory_policy/include/memory_policy.h
```

Only if existing API is insufficient.

Prefer reusing the current API.

---

## MCP diagnostics

Likely:

```text
components/mcp_core/*
components/mcp_tool_exposure/*
```

Only add lightweight size/count diagnostics.

Do not move transport-specific logic into MCP core.

---

# 43. Architectural boundary

Keep responsibilities:

```text
mcp_core
    = JSON-RPC / MCP behavior

mcp_tool_exposure
    = tool registry

mcp_ws_bridge
    = WebSocket transport lifecycle

memory_policy
    = memory placement policy
```

Do not solve TLS transport memory problems inside:

```text
mcp_tool_exposure
```

or:

```text
command_dispatcher
```

---

# 44. New Kconfig options proposed

Potential final options:

```text
CONFIG_MCP_WS_TX_FRAGMENT_SIZE=1536
CONFIG_MCP_WS_RELOAD_COOLDOWN_MS=250
```

Optional only if needed:

```text
CONFIG_MCP_WS_PSRAM_TX_THRESHOLD=1024
```

Avoid excessive tuning knobs.

---

# 45. Logging policy

Development builds:

```text
INFO:
connect/reconnect lifecycle
TX size
tools count
memory snapshots at major events

WARN:
large TX
TX failure
low DMA largest block

ERROR:
unrecoverable connection failures
```

Production:

Do not continuously log memory snapshots.

Keep:

```text
TX failure
reconnect reason
last_error
```

---

# 46. Suggested low-memory warning

Development helper:

```text
if DMA largest block < threshold:
    log warning
```

Start observation threshold:

```text
4096 bytes
```

This is a diagnostic threshold, not yet a production safety limit.

---

# 47. Reconnect behavior after remediation

Target behavior:

```text
User presses reconnect
        ↓
bridge marks current generation stale
        ↓
old client destroyed
        ↓
short cooldown
        ↓
new client connects
        ↓
MCP initialize
        ↓
READY
        ↓
tools/list
        ↓
large response fragmented
        ↓
TLS TX succeeds
```

---

# 48. TX failure behavior after remediation

Target:

```text
esp_websocket_client_send_* fails
        ↓
log TX size + memory state
        ↓
mark MCP connection invalid
        ↓
destroy current client
        ↓
backoff
        ↓
reconnect
```

Never remain falsely in:

```text
MCP_WS_READY
```

after TLS write failure.

---

# 49. Definition of Done

This issue is complete only when:

- [ ] Original error is reproducible in baseline build.
- [ ] TX payload size at failure is known.
- [ ] DMA free/largest at failure is known.
- [ ] Reconnect no longer produces `esp-aes: Failed to allocate memory`.
- [ ] Large `tools/list` succeeds.
- [ ] TX failure invalidates current session.
- [ ] Reconnect uses controlled cooldown.
- [ ] 100 reconnect cycles show no monotonic memory loss.
- [ ] No stale WebSocket callback destroys a new client.
- [ ] No duplicate WebSocket client task remains.
- [ ] BLE target connection count remains stable.
- [ ] Web UI remains responsive.
- [ ] Memory policy changes are benchmarked before production default.
- [ ] No valid MCP response is arbitrarily truncated.
- [ ] MCP WebSocket message semantics remain correct.
- [ ] Production logs do not leak endpoint/token data.

---

# 50. Final technical direction

The preferred solution is not:

```text
increase heap blindly
```

but:

```text
measure
    ↓
identify exact TX pressure
    ↓
make TX memory-bounded
    ↓
move non-DMA buffers to PSRAM
    ↓
ensure reconnect cleanup
    ↓
tune sdkconfig only if still necessary
```

Expected root issue:

```text
large post-reconnect MCP TX
+
TLS/AES internal DMA requirement
+
fragmented internal memory
```

Therefore the highest-value remediation sequence is:

```text
instrumentation
→ fragmented WebSocket TX
→ TX failure recovery
→ reconnect cooldown
→ PSRAM placement
→ optional sdkconfig tuning
```

This preserves the architecture of the gateway while addressing the actual constrained resource: **contiguous internal DMA-capable memory during TLS transmission**.
