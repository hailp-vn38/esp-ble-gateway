# ESP32 BLE Gateway – Performance Test Development Plan

**Project:** `esp-ble-gateway`  
**Repository:** `https://github.com/hailp-vn38/esp-ble-gateway`  
**Related device repository:** `https://github.com/hailp-vn38/esp-ble-device`  
**Target:** ESP32-S3 / 16 MB Flash / 8 MB Octal PSRAM  
**Framework:** **ESP-IDF v6.1-rc1**  
**Document version:** **v1.1**  
**Previous version:** v1.0  
**Date:** 2026-08-30  
**Status:** Reviewed implementation specification

---

# 1. Mục tiêu

Tài liệu này định nghĩa framework kiểm thử hiệu năng có thể triển khai trực tiếp cho `esp-ble-gateway`.

Mục tiêu là qualification:

```text
Memory headroom
Memory fragmentation trend
HTTP performance
MCP performance
Command execution
Capability pipeline
Dynamic MCP tool catalog
Xiaozhi WebSocket/TLS
BLE lifecycle
Recovery behavior
Long-term stability
```

Trong điều kiện hiện tại chỉ có:

```text
1 BLE device vật lý
```

framework phải cho phép mô phỏng:

```text
3 logical devices
6 logical devices
9 logical devices
```

mà vẫn đi qua phần lớn architecture thực của gateway.

---

# 2. Scope và giới hạn qualification

## 2.1 Application-load qualification

Có thể thực hiện bằng:

```text
1 BLE thật
+
N simulated devices
```

Test được:

```text
device_store
device_capabilities
capability queue
command_dispatcher
command_executor
MCP tool exposure
MCP tool catalog
Web API serialization/load
MCP HTTP
Xiaozhi MCP WebSocket
JSON/CBOR processing
PSRAM allocator
task concurrency
queue saturation
timeout/recovery
```

---

## 2.2 Real BLE-link qualification

Simulator không tạo đầy đủ:

```text
NimBLE connection state
controller connection state
L2CAP state
ATT/GATT state
per-link security state
ACL buffers
RF scheduling
real notify traffic
real reconnect behavior
```

Do đó:

```text
1 real + 8 simulated
```

không được báo cáo là:

```text
9 BLE links qualified
```

Mà phải ghi:

```text
Logical devices: 9
Real BLE links: 1
```

Production qualification cuối cùng vẫn cần:

```text
3 real BLE
6 real BLE
9 real BLE
```

khi có đủ hardware.

---

# 3. Naming convention cho profile

Dùng ký hiệu:

```text
R = real BLE links
L = logical devices
H = performance harness state
```

Ví dụ:

```text
PERF-R1-L9
```

nghĩa là:

```text
1 real BLE link
9 logical devices total
```

Baseline performance firmware:

```text
PERF-R1-L1-H0
```

nghĩa là:

```text
performance firmware compiled
simulator idle
0 fake device active
1 real BLE
```

---

# 4. Baseline hiện tại

Sau tối ưu RAM gần nhất, production firmware với:

```text
1 device
1 connected device
1 BLE link
```

đã đo:

```text
Internal free                95,635 B
Internal minimum free        39,828 B
Internal largest block       55,296 B

PSRAM free                7,744,304 B
PSRAM minimum free        7,736,040 B
PSRAM largest block       7,733,248 B
```

Đây là:

```text
PROD-R1-L1
```

Không dùng trực tiếp số này để tính memory cost của fake devices.

---

# 5. Baseline methodology mới

Performance firmware sẽ thêm:

```text
perf_device_sim
perf_metrics
perf API
sim worker
sim queue
extra counters
```

Do đó phải đo riêng:

```text
PERF-R1-L1-H0
```

sau khi compile performance features nhưng chưa start simulator.

---

## 5.1 Perf harness overhead

Tính:

```text
internal_harness_overhead =
PROD-R1-L1.internal_free
-
PERF-R1-L1-H0.internal_free
```

Tương tự:

```text
psram_harness_overhead
largest_block_delta
```

---

## 5.2 Logical-device memory cost

Dùng:

```text
PERF-R1-L1-H0
vs
PERF-R1-L3
PERF-R1-L6
PERF-R1-L9
```

Không so trực tiếp production binary với performance binary để suy ra per-device cost.

---

# 6. Hiện trạng hỗ trợ performance trong project

Project đã có:

```text
/api/status
```

trả:

```text
device_count
connected_count
ble_link_count

internal.free
internal.min_free
internal.largest_free_block

psram.free
psram.min_free
psram.largest_free_block

executor.submitted
executor.completed
executor.queue_full
executor.queue_timeout
executor.dispatch_timeout
executor.max_queue_depth
executor.max_queue_wait_ms
executor.worker_stack_min_bytes
```

Không duplicate các metric này.

Project cũng đã có:

```text
docs/ESP32_BLE_GATEWAY_PERFORMANCE_TEST_FRAMEWORK.md
```

Tài liệu v1.1 này là implementation plan mở rộng framework đó.

---

# 7. Architecture tổng thể

```text
                         PC
              ┌──────────────────────┐
              │ Performance Runner   │
              │                      │
              │ HTTP load            │
              │ MCP client           │
              │ WS mock              │
              │ Xiaozhi integration  │
              │ Metrics collector    │
              │ Scenario controller  │
              │ Report generator     │
              └──────────┬───────────┘
                         │ Wi-Fi
                         ▼
        ┌────────────────────────────────────┐
        │ ESP32-S3 Gateway                  │
        │                                    │
        │ Web / MCP / Xiaozhi               │
        │                                    │
        │ command_executor                  │
        │ command_dispatcher                │
        │ device_capabilities               │
        │ MCP exposure/catalog              │
        │                                    │
        │ ┌───────────────────────────────┐  │
        │ │ perf_device_sim               │  │
        │ │ fake-02 ... fake-09           │  │
        │ └───────────────────────────────┘  │
        │                                    │
        │ BLE Central                        │
        └────────────────┬───────────────────┘
                         │
                         ▼
                    Real Device #1
```

---

# 8. Component mới: `perf_device_sim`

Tạo:

```text
components/perf_device_sim/
    CMakeLists.txt
    Kconfig.projbuild

    include/
        perf_device_sim.h

    perf_device_sim.c
    perf_device_sim_transport.c
    perf_device_sim_capabilities.c
    perf_device_sim_metrics.c
```

Component chỉ dùng trong performance build.

---

# 9. Kconfig

```text
menu "Gateway Performance Test"

config GW_PERF_DEVICE_SIM
    bool "Enable simulated gateway devices"
    default n

config GW_PERF_SIM_MAX_DEVICES
    int "Maximum simulated devices"
    range 1 8
    default 8
    depends on GW_PERF_DEVICE_SIM

config GW_PERF_SIM_CAPABILITY_COUNT
    int "Capabilities per simulated device"
    range 1 12
    default 12
    depends on GW_PERF_DEVICE_SIM

config GW_PERF_METRICS
    bool "Enable extended performance metrics"
    default n

config GW_PERF_API
    bool "Enable performance control API"
    default n
```

Production:

```text
CONFIG_GW_PERF_DEVICE_SIM=n
CONFIG_GW_PERF_METRICS=n
CONFIG_GW_PERF_API=n
```

Performance:

```text
CONFIG_GW_PERF_DEVICE_SIM=y
CONFIG_GW_PERF_SIM_MAX_DEVICES=8
CONFIG_GW_PERF_SIM_CAPABILITY_COUNT=12
CONFIG_GW_PERF_METRICS=y
CONFIG_GW_PERF_API=y
```

---

# 10. Production isolation

Khi:

```text
CONFIG_GW_PERF_DEVICE_SIM=n
```

phải đảm bảo:

```text
no simulator task
no simulator queue
no simulator buffers
no simulator device records
no simulator API
no mutable perf transport seam
```

Mục tiêu:

```text
production firmware không chịu runtime overhead
```

---

# 11. Performance build profile riêng

Tạo:

```text
sdkconfig.perf.defaults
```

Không thay production defaults chỉ để benchmark.

---

## 11.1 Build directories

Production:

```text
build/
```

Performance:

```text
build-perf/
```

Không dùng chung cache.

---

## 11.2 Build script

Update `build_flash.sh`:

```text
--perf
```

Ví dụ:

```bash
./build_flash.sh --perf --build-only
./build_flash.sh --perf --port /dev/ttyACM0 --monitor
```

Khi `--perf`:

```text
SDKCONFIG_DEFAULTS =
sdkconfig.defaults;sdkconfig.perf.defaults
```

và build dir:

```text
build-perf/
```

---

## 11.3 Metadata

Report phải ghi:

```text
build_profile = production | perf
sdkconfig hash
git SHA
```

---

# 12. Simulated device identity

Reserved prefix:

```text
perf-sim-
```

IDs:

```text
perf-sim-02
perf-sim-03
...
perf-sim-09
```

Names:

```text
Perf Device 02
...
Perf Device 09
```

Type:

```text
perf_device
```

Không dùng prefix này cho thiết bị thật.

---

# 13. Device Store integration

Phase đầu dùng:

```text
device_store_add()
```

vì đây là path hiện có và ít thay đổi architecture.

Không set BLE identity cho fake device.

Như vậy fake device không tham gia reconnect scheduler của BLE runtime.

---

# 14. Device Store capacity preflight

Current max:

```text
DEVICE_STORE_MAX_DEVICES = 16
```

Trước start simulator:

```text
existing = current device count
available = 16 - existing
```

Nếu:

```text
requested_simulated > available
```

thì reject:

```text
409 / resource exhausted
```

Không truncate silently.

---

# 15. Cleanup lifecycle bắt buộc

Không được chỉ:

```text
device_store_delete(perf-sim-X)
```

vì còn:

```text
MCP exposure
capability state
capability NVS
catalog state
```

---

## 15.1 Tạo orchestration API dùng chung

Đề xuất:

```c
esp_err_t gateway_device_forget(
    const char *device_id
);
```

Thứ tự:

```text
mcp_tool_exposure_forget_device
        ↓
device_capabilities_forget
        ↓
ble_central_forget_peer
        ↓
device_store_delete
```

Web delete và simulator cleanup nên dùng cùng lifecycle này.

---

# 16. Simulator lifecycle

State:

```text
STOPPED
STARTING
RUNNING
QUIESCING
FAULTED
```

---

# 17. Start sequence

```text
cleanup stale perf-sim-*
        |
        v
capacity preflight
        |
        v
create fake store records
        |
        v
mark simulator connected state
        |
        v
trigger capability lifecycle
        |
        v
wait readiness barrier
        |
        v
RUNNING
```

---

# 18. Stop sequence

Không delete ngay.

```text
RUNNING
   ↓
QUIESCING
   ↓
reject new fake commands
   ↓
drain/cancel sim queue
   ↓
wait pending commands bounded time
   ↓
stop capability activity
   ↓
gateway_device_forget() per fake device
   ↓
clear simulator state
   ↓
STOPPED
```

Nếu bounded cleanup timeout:

```text
forced_cleanup_count++
scenario FAIL
```

---

# 19. Public command transport seam

Current project đã có test hook:

```text
send_command
is_connected
```

v1.1 giữ abstraction nhưng không mở mutable runtime setter trong production.

---

## 19.1 Config-gated interface

Ví dụ:

```c
#if CONFIG_GW_PERF_DEVICE_SIM
esp_err_t device_command_install_perf_transport(
    const device_command_transport_t *transport
);
#endif
```

Unit test build có seam riêng theo test config.

Production build:

```text
BLE transport fixed
```

---

# 20. Hybrid transport

```c
static int perf_is_connected(const char *device_id)
{
    if (perf_device_sim_is_device(device_id)) {
        return perf_device_sim_is_connected(device_id);
    }

    return ble_central_is_connected(device_id);
}
```

Send:

```c
static int perf_send_command(
    const char *device_id,
    const gw_message_t *msg
)
{
    if (perf_device_sim_is_device(device_id)) {
        return perf_device_sim_submit_command(device_id, msg);
    }

    return ble_central_send_command(device_id, msg);
}
```

---

# 21. Simulator command path

Không ACK synchronous trong `send_command()`.

Phải:

```text
command_dispatcher
       |
       v
perf_send_command()
       |
       v
SIM COMMAND QUEUE
       |
       v
sim worker
       |
       v
delay / fault injection
       |
       v
command_dispatcher_on_device_notify()
```

Như vậy vẫn test:

```text
pending request
executor concurrency
timeout behavior
ACK correlation
```

---

# 22. Simulator queue phải low-overhead

Không dùng:

```c
xQueueCreate(
    16,
    sizeof(perf_sim_command_t)
);
```

nếu `perf_sim_command_t` chứa nguyên `gw_message_t`.

Điều này làm simulator tự tiêu tốn internal RAM và làm méo memory benchmark.

---

## 22.1 Pointer queue

Queue chỉ giữ pointer:

```c
perf_sim_command_t *
```

Job allocate qua:

```c
gw_mem_alloc(
    sizeof(perf_sim_command_t),
    GW_MEM_EXTERNAL_PREFERRED
);
```

Queue depth:

```text
16
```

nhưng storage nội bộ chỉ khoảng:

```text
16 × pointer
```

---

# 23. Simulator task

Initial recommendation:

```text
priority = tskIDLE_PRIORITY + 2
stack = 3072
```

Không để simulator priority cao hơn executor hoặc BLE critical tasks.

Sau đó đo watermark.

---

# 24. ACK contract – bắt buộc đúng protocol

ACK phải có:

```text
type = "device_ack"
has_device_id = true
device_id = pending device
has_request_id = true
request_id = pending request ID
command = pending command
```

Ví dụ:

```c
gw_message_t ack = {
    .protocol_version = GW_PROTOCOL_VERSION,
    .has_device_id = true,
    .has_request_id = true,
    .request_id = request.request_id,
    .has_bool_value = true,
    .bool_value = true,
};

strlcpy(
    ack.type,
    "device_ack",
    sizeof(ack.type)
);

strlcpy(
    ack.device_id,
    device_id,
    sizeof(ack.device_id)
);

strlcpy(
    ack.command,
    request.command,
    sizeof(ack.command)
);
```

Sau đó:

```c
command_dispatcher_on_device_notify(
    device_id,
    &ack
);
```

---

# 25. Simulator behavior profiles

## `SIM_PROFILE_STABLE`

```text
success        100%
delay          20–50 ms
```

---

## `SIM_PROFILE_NORMAL`

Không có random BUSY.

```text
success         96%
device_error     2%
timeout          1%
disconnect       1%
delay           20–100 ms
```

---

## `SIM_PROFILE_STRESS`

```text
success         85%
device_error     5%
timeout          5%
disconnect       5%
delay           20–500 ms
```

---

## `SIM_PROFILE_LATENCY`

Fixed delay:

```text
20
50
100
250
500
1000 ms
```

---

# 26. BUSY semantics

`BUSY` không phải fake ACK outcome.

BUSY xảy ra khi:

```text
request A pending cho device X
+
request B gửi vào cùng device X
```

Runner phải tạo workload này chủ động.

Expected:

```text
request B -> DISPATCH_STATUS_BUSY
```

---

# 27. Disconnect semantics

Tách thành hai case.

## 27.1 `DISCONNECT_PRE_SEND`

Simulator state:

```text
connected = false
```

trước dispatch.

Expected:

```text
DISPATCH_STATUS_NOT_CONNECTED
```

---

## 27.2 `DISCONNECT_IN_FLIGHT`

Command đã gửi.

Sau đó fake device disconnect trước ACK.

Expected behavior phụ thuộc current dispatcher contract.

Nếu pending request không được actively failed:

```text
ACK timeout
```

Runner phải ghi đúng expected result, không gọi chung là "disconnect error".

---

# 28. Deterministic seed

Mọi profile có fault injection phải có seed.

Ví dụ:

```text
seed = 0x20260830
```

Report lưu:

```text
scenario
seed
firmware
git SHA
```

---

# 29. Capability simulation

Không inject snapshot trực tiếp.

Flow:

```text
READY
  ↓
describe_capabilities
  ↓
capabilities_begin
  ↓
capability_item × N
  ↓
capabilities_end
  ↓
device_ack
```

---

# 30. Capability set

Default:

```text
12 capabilities/device
```

Ví dụ:

```text
set_power
toggle
set_level
set_mode
set_speed
set_temperature
set_brightness
set_color_temp
set_timer
set_target
reset
ping
```

Requirement:

```text
valid schema
unique command names
full snapshot
```

---

# 31. Capability delivery

Dùng public APIs:

```c
device_capabilities_on_ready(device_id);
device_capabilities_on_notify(device_id, &msg);
```

Không truy cập trực tiếp:

```text
s_records
catalog internals
NVS blobs
```

---

# 32. Capability burst modes

## Normal

```text
begin
10–20 ms
item
...
end
```

## Burst

```text
begin
item × 12
end
```

gần như không delay.

## Fault

```text
missing item
duplicate sequence
wrong sequence
disconnect before end
wrong total
```

---

# 33. Readiness barrier

Không dùng:

```text
sleep(5 sec)
```

để chờ simulator sẵn sàng.

`/api/perf/status` phải trả:

```json
{
  "simulator": {
    "expected_devices": 8,
    "ready_devices": 8,
    "capability_ready": 8
  }
}
```

Runner chờ:

```text
capability_ready == expected_devices
```

với timeout.

---

# 34. MCP readiness barrier

Thêm:

```text
catalog_revision
enabled_tool_count
```

Runner chỉ bắt đầu MCP benchmark khi trạng thái đạt expected.

---

# 35. Gateway status semantics

Không sửa production semantics.

Với:

```text
1 real + 8 fake
```

`/api/status` có thể đúng khi trả:

```json
{
  "device_count": 9,
  "connected_count": 1,
  "ble_link_count": 1
}
```

vì `connected_count` hiện phản ánh BLE runtime thật.

---

# 36. Performance status riêng

`/api/perf/status` thêm:

```json
{
  "perf": {
    "real_ble_links": 1,
    "real_connected_devices": 1,

    "simulated_devices": 8,
    "simulated_connected": 8,

    "logical_devices": 9,
    "logical_connected": 9
  }
}
```

Không giả mạo field production.

---

# 37. Web UI interpretation

Fake devices đủ để test:

```text
device list serialization
device detail serialization
capability APIs
exposure APIs
command APIs
load/latency
```

Nhưng không hoàn toàn test:

```text
production BLE-connected-state UI semantics
```

vì fake devices không tồn tại trong BLE runtime.

Document/report phải ghi rõ:

```text
Web load tested: YES
Web real-BLE connection-state semantics: NO
```

---

# 38. Component `perf_metrics`

Tạo:

```text
components/perf_metrics/
    CMakeLists.txt
    Kconfig.projbuild

    include/
        perf_metrics.h

    perf_metrics.c
```

Default:

```text
disabled
```

---

# 39. Memory metrics

Reuse existing:

```text
internal_free
internal_min_free
internal_largest

psram_free
psram_min_free
psram_largest
```

---

# 40. Scenario-local heap minimum

Global `min_free` tính từ boot không đủ chính xác cho từng scenario.

Performance build nên dùng ESP-IDF local minimum monitoring nếu API khả dụng trong target version.

Flow:

```text
reset scenario metrics
        ↓
start local minimum monitor
        ↓
run scenario
        ↓
capture scenario-local min
        ↓
stop monitor
```

Report:

```text
boot_min_free
scenario_min_free
```

---

# 41. Executor metrics

Existing:

```text
submitted
completed
queue_full
queue_timeout
dispatch_timeout
max_queue_depth
max_queue_wait_ms
worker_stack_min_bytes
```

---

# 42. Simulator metrics

```text
sim_device_count
sim_connected_count

sim_commands_rx
sim_commands_success
sim_commands_error
sim_commands_timeout
sim_commands_dropped

sim_queue_depth
sim_queue_high_water
sim_queue_full

sim_disconnect_count
sim_reconnect_count

sim_worker_jobs
sim_worker_stack_min
forced_cleanup_count
```

---

# 43. Capability metrics

```text
cap_events_enqueued
cap_events_dropped
cap_queue_high_water
cap_message_alloc_fail

cap_discovery_started
cap_discovery_completed
cap_discovery_failed
```

---

# 44. MCP metrics

```text
mcp_requests
mcp_errors
mcp_tools_list_count
mcp_tools_call_count
mcp_active_async
mcp_peak_async
```

Latency chính đo từ PC runner.

---

# 45. Xiaozhi / WS metrics

```text
ws_connect_count
ws_disconnect_count
ws_reconnect_count

ws_rx_messages
ws_tx_messages

ws_rx_bytes
ws_tx_bytes

ws_send_fail
ws_alloc_fail
```

---

# 46. Performance API

Compile only khi:

```text
CONFIG_GW_PERF_API=y
```

Routes:

```text
GET  /api/perf/status

POST /api/perf/sim/start
POST /api/perf/sim/stop

POST /api/perf/sim/profile
POST /api/perf/sim/disconnect
POST /api/perf/sim/reconnect

POST /api/perf/reset-metrics
POST /api/perf/scenario/start
POST /api/perf/scenario/stop
```

---

# 47. Performance API production rule

Performance API không được tồn tại trong production firmware.

Yêu cầu:

```text
compile-time disabled
```

Không chỉ runtime toggle.

---

# 48. Simulator start request

```json
{
  "count": 8,
  "profile": "normal",
  "capabilities": 12,
  "seed": 539363376
}
```

Response:

```json
{
  "success": true,
  "real_ble_links": 1,
  "simulated_devices": 8,
  "logical_devices": 9
}
```

---

# 49. PC Performance Runner

Tạo:

```text
tools/perf/
    README.md
    requirements.txt

    perf_runner.py
    scenarios.py
    collector.py
    reporter.py
    load_models.py

    scenarios/
        baseline.json
        logical_3.json
        logical_6.json
        logical_9.json
        logical_9_mcp.json
        logical_9_xiaozhi.json
        reconnect_stress.json
        saturation.json
        soak.json
```

---

# 50. Runner responsibilities

1. Đọc `/api/status`.
2. Đọc `/api/perf/status`.
3. Xác nhận firmware version.
4. Xác nhận IDF version.
5. Xác nhận build profile.
6. Xác nhận git SHA.
7. Capacity preflight.
8. Start simulator.
9. Chờ readiness barriers.
10. Start scenario-local metrics.
11. Chạy workload.
12. Poll metrics.
13. Thu latency.
14. Stop workload.
15. Stop scenario-local metrics.
16. Quiesce simulator.
17. Stop simulator.
18. Ghi raw JSON.
19. Ghi CSV.
20. Tạo Markdown report.
21. Tính PASS/FAIL.

---

# 51. Load models

Runner phải hỗ trợ rõ hai kiểu.

---

## 51.1 Closed-loop

```text
send request
wait response
send next
```

Dùng để đo:

```text
latency
single-client responsiveness
```

Scenario:

```json
{
  "load_model": "closed_loop",
  "concurrency": 1
}
```

---

## 51.2 Open-loop

```text
schedule requests at fixed rate
regardless of outstanding responses
```

Dùng:

```text
saturation
queue pressure
throughput limit
```

Scenario:

```json
{
  "load_model": "open_loop",
  "rate_rps": 5,
  "concurrency": 4
}
```

---

# 52. Report metadata

Mỗi run:

```text
timestamp
git SHA
firmware version
ESP-IDF version
build profile
sdkconfig hash

board
flash
PSRAM

Wi-Fi RSSI

real BLE links
simulated devices
logical devices

scenario
seed
duration
load model
```

---

# 53. Output structure

```text
perf-results/
    2026-08-30T010000/
        metadata.json
        metrics.csv
        latency.csv
        errors.json
        summary.json
        report.md
```

---

# 54. Sampling

Default:

```text
1 sample/sec
```

Burst:

```text
250 ms
```

Không sample quá nhanh gây self-load.

---

# 55. Test profile A – Production baseline

```text
PROD-R1-L1
```

Production firmware.

Duration:

```text
5 min
```

Dùng làm reference production memory.

---

# 56. Test profile B – Performance harness baseline

```text
PERF-R1-L1-H0
```

Performance firmware compiled.

Simulator:

```text
STOPPED
```

Duration:

```text
5 min
```

Tính perf harness overhead.

---

# 57. Test profile C – Logical 3

```text
PERF-R1-L3
```

```text
1 real
2 simulated
```

Mỗi fake:

```text
12 capabilities
```

---

# 58. Test profile D – Logical 6

```text
PERF-R1-L6
```

```text
1 real
5 simulated
```

---

# 59. Test profile E – Logical 9

```text
PERF-R1-L9
```

```text
1 real
8 simulated
```

Đây là nominal upper-layer capacity test.

---

# 60. MCP capability/tool scaling

9 logical × 12 capabilities có thể tạo:

```text
108 capabilities
```

nhưng MCP enabled catalog hiện có capacity riêng.

Do đó test phải tách:

```text
CAP-108 / MCP-0
CAP-108 / MCP-8
CAP-108 / MCP-16
CAP-108 / MCP-24
CAP-108 / MCP-32
```

"Full MCP tools" trong v1.1 nghĩa là:

```text
32 enabled dynamic tools
```

không phải 108.

---

# 61. Command load

Phân bố command qua nhiều device.

Closed-loop:

```text
1 client
```

Open-loop rates:

```text
1 req/s
5 req/s
10 req/s
20 req/s
```

Normal profile không cố tình gửi nhiều concurrent request cùng device.

BUSY test làm riêng.

---

# 62. BUSY test

Flow:

```text
send A -> perf-sim-02
hold ACK
send B -> perf-sim-02
```

Expected:

```text
B = BUSY
A eventually completes
```

---

# 63. MCP load

R1-L9.

```text
tools/list
tools/call
```

Measure:

```text
p50
p95
p99
max
error rate
queue wait
memory delta
```

---

# 64. Dynamic tools/list scaling

Measure:

```text
0
8
16
24
32
```

enabled tools.

Record:

```text
response size
serialization latency
internal RAM
PSRAM
```

---

# 65. Web workload

Test:

```text
/api/status
/api/devices
/api/capabilities
/api/exposure
/api/command
settings APIs
```

Fake devices test serialization/load nhưng không substitute full real-BLE UI semantics.

---

# 66. WebSocket test – deterministic mock

Tạo local/mock MCP WebSocket server trên PC.

Profile:

```text
WS-MOCK-R1-L9
```

Test:

```text
connect
initialize
tools/list
tools/call
reconnect
catalog refresh
```

Dùng cho:

```text
gateway latency regression
RAM
protocol throughput
reconnect behavior
```

---

# 67. Xiaozhi real integration

Profile:

```text
XIAOZHI-REAL-R1-L9
```

Test endpoint thật.

Dùng để qualification:

```text
TLS
compatibility
real tools/list
real tools/call
manual reconnect
```

Không dùng WAN-dependent p95 latency làm hard gateway regression threshold.

---

# 68. Reconnect stress

Simulated reconnect:

```text
100 cycles
```

Flow:

```text
disconnect fake
reconnect
capability rediscovery
catalog stabilization
```

---

# 69. Xiaozhi reconnect stress

```text
50 cycles
```

Each:

```text
disconnect WSS
reconnect WSS
handshake
tools/list
```

Pass:

```text
no memory leak
no task leak
no WS client leak
no progressive reconnect slowdown
```

---

# 70. Network reconnect test

PC runner hiện không tự nhiên có quyền điều khiển AP.

Do đó network outage test phải được đánh dấu một trong:

```text
EXTERNAL HARNESS
```

hoặc nếu sau này tạo test API:

```text
PERF WIFI ACTUATOR
```

Không ghi là fully automated cho tới khi có actuator cụ thể.

---

# 71. Soak

Minimum:

```text
24 h
```

Production target:

```text
72 h
```

Workload:

```text
R1-L9
Web polling
MCP tools/list
MCP tools/call
WS mock hoặc Xiaozhi
simulated command traffic
periodic simulated reconnect
```

---

# 72. Memory qualification

## PROD-R1-L1

Target:

```text
internal free >= 80 KB
largest block >= 40 KB
```

Safety:

```text
boot min_free >= 24 KB
```

---

## PERF-R1-L9

Target:

```text
internal free >= 50 KB
largest block >= 32 KB
scenario_min_free >= 24 KB
```

---

## PERF-R1-L9 + WS/TLS

Target:

```text
internal free >= 40 KB
largest block >= 24 KB
scenario_min_free >= 24 KB
```

---

# 73. Leak gate

Sau warm-up:

```text
T0 = steady-state after 10 min
```

Compare:

```text
T+1h
T+6h
T+24h
```

Initial gate:

```text
internal steady-state drift <= 4 KB / 24h
PSRAM steady-state drift <= 16 KB / 24h
```

Không dùng global min_free để suy leak.

---

# 74. Fragmentation trend

Record:

```text
free
largest_free_block
free - largest
```

Không coi `free-largest` là fragmentation metric tuyệt đối.

Reject nếu:

```text
largest internal block < 20 KB
```

hoặc largest block giảm liên tục qua reconnect cycles mà không recover.

---

# 75. Latency metrics

Runner thu:

```text
HTTP latency
MCP tools/list latency
MCP tools/call latency
WS reconnect latency
capability discovery duration
```

Report:

```text
count
min
mean
p50
p95
p99
max
```

---

# 76. Initial latency targets

## `/api/status`

```text
p95 <= 250 ms
```

## MCP `tools/list`

R1-L9:

```text
p95 <= 500 ms
```

## Simulated tools/call

Nếu simulator ACK delay = D:

```text
p95 <= D + 300 ms
```

---

# 77. Error gates

Normal workload:

```text
executor.queue_full = 0
cap_event_drop = 0
sim_command_drop = 0
heap corruption = 0
TLSF assertion = 0
watchdog = 0
unexpected reboot = 0
ESP_ERR_NO_MEM = 0
forced_cleanup_count = 0
```

---

# 78. Saturation test

Tăng open-loop load:

```text
1
2
5
10
20
50 req/s
```

đến khi:

```text
queue_full > 0
latency tăng mạnh
timeout tăng
```

Record:

```text
maximum sustainable rate
saturation point
failure mode
recovery time
```

---

# 79. Recovery after overload

Sau overload:

```text
stop load
wait bounded recovery window
```

Gateway phải:

```text
queue depth -> 0
pending requests -> 0
latency -> normal band
memory -> near pre-load steady state
no stuck task
```

---

# 80. Real BLE device workload

Trong R1-L9, thiết bị thật vẫn phải hoạt động.

Periodically:

```text
1 real command / 5 sec
```

Verify ACK.

Mục tiêu:

```text
fake load không được starve real BLE path
```

---

# 81. Real BLE pass criterion

Combined workload:

```text
real command success >= 99%
```

không tính physical disconnect do môi trường test.

---

# 82. Simulator overhead warning

Simulator worker chạy trên chính gateway.

Do đó `PERF-R1-L9` có thêm CPU cost không tồn tại khi 8 peripheral chạy trên MCU riêng.

Kết quả:

```text
memory/queue/application stress -> useful
CPU throughput -> conservative but not equal real 9-device system
```

Report phải ghi:

```text
simulator CPU overhead present
```

---

# 83. CPU telemetry

Nếu runtime stats được bật trong perf build mà overhead chấp nhận được:

```text
task CPU %
idle %
```

Không bắt buộc production.

---

# 84. Task stack telemetry

Track:

```text
HTTP server
command workers
device capabilities
MCP exposure worker
BLE notify
BLE reconnect
Xiaozhi bridge
WS client
sim worker
```

Gate:

```text
>= 1024 B remaining
```

Preferred:

```text
>= 1536 B
```

---

# 85. NVS considerations

Simulator dùng `device_store_add()` và capability flow thực nên có thể ghi NVS.

Start:

```text
cleanup stale perf-sim-*
```

Stop:

```text
gateway_device_forget() each fake
```

Boot performance mode:

```text
cleanup stale perf-sim-*
```

---

# 86. Auto exposure rule

Simulator không auto-enable MCP tools mặc định.

Runner điều khiển:

```text
0
8
16
24
32
```

enabled tools qua API thực.

---

# 87. Test reproducibility

Scenario hợp lệ khi report có:

```text
git SHA
sdkconfig hash
build profile
scenario JSON
seed
firmware version
IDF version
```

---

# 88. Regression comparison

So sánh:

```text
baseline
candidate
```

Metrics:

```text
internal free
largest block
scenario_min_free
p95 latency
error rate
queue depth
reconnect latency
```

Warning thresholds ban đầu:

```text
>10% latency regression
>8 KB internal RAM regression
>8 KB largest block regression
```

---

# 89. Phase triển khai v1.1

## Phase 0 – Perf build profile & methodology

- [ ] `sdkconfig.perf.defaults`
- [ ] `build-perf/`
- [ ] `build_flash.sh --perf`
- [ ] PROD-R1-L1 baseline
- [ ] PERF-R1-L1-H0 baseline
- [ ] harness overhead report

---

## Phase 1 – Transport seam

- [ ] Config-gated transport abstraction
- [ ] BLE default unchanged
- [ ] Existing tests migrated
- [ ] No mutable production override

---

## Phase 2 – Simulator core

- [ ] Component
- [ ] IDs
- [ ] state machine
- [ ] hybrid transport
- [ ] pointer queue
- [ ] low-priority worker
- [ ] correct ACK contract
- [ ] deterministic seed

---

## Phase 3 – Cleanup lifecycle

- [ ] `gateway_device_forget()`
- [ ] stale fake cleanup
- [ ] QUIESCING state
- [ ] pending drain
- [ ] bounded stop
- [ ] forced cleanup metric

---

## Phase 4 – Capability simulation

- [ ] READY
- [ ] describe_capabilities
- [ ] BEGIN
- [ ] ITEM × 12
- [ ] END
- [ ] ACK
- [ ] burst
- [ ] fault cases

---

## Phase 5 – Metrics

- [ ] existing status reuse
- [ ] scenario-local minimum
- [ ] simulator metrics
- [ ] capability metrics
- [ ] MCP metrics
- [ ] WS metrics
- [ ] stack watermarks

---

## Phase 6 – Performance API

- [ ] status
- [ ] start
- [ ] stop
- [ ] profile
- [ ] disconnect
- [ ] reconnect
- [ ] reset metrics
- [ ] readiness barriers
- [ ] compile-time gating

---

## Phase 7 – PC runner

- [ ] scenario loader
- [ ] open-loop
- [ ] closed-loop
- [ ] status polling
- [ ] HTTP load
- [ ] MCP load
- [ ] WS mock
- [ ] result collector
- [ ] Markdown report
- [ ] PASS/FAIL engine

---

## Phase 8 – Logical scaling

- [ ] PERF-R1-L3
- [ ] PERF-R1-L6
- [ ] PERF-R1-L9

---

## Phase 9 – MCP scaling

- [ ] CAP-108 / MCP-0
- [ ] MCP-8
- [ ] MCP-16
- [ ] MCP-24
- [ ] MCP-32

---

## Phase 10 – WebSocket

- [ ] WS-MOCK-R1-L9
- [ ] XIAOZHI-REAL-R1-L9

---

## Phase 11 – Stress/recovery

- [ ] BUSY
- [ ] timeout
- [ ] pre-send disconnect
- [ ] in-flight disconnect
- [ ] saturation
- [ ] overload recovery
- [ ] 100 fake reconnects
- [ ] 50 WS reconnects

---

## Phase 12 – Soak

- [ ] 24h
- [ ] 72h

---

# 90. Unit tests

## Simulator

- [ ] reserved ID detection
- [ ] real ID delegates BLE
- [ ] fake ID uses simulator
- [ ] queue command
- [ ] correct ACK type
- [ ] correct device_id
- [ ] correct request_id
- [ ] correct command
- [ ] timeout profile
- [ ] disconnect profile
- [ ] queue full
- [ ] cleanup queue
- [ ] safe restart

---

## Capability simulator

- [ ] begin
- [ ] 12 items
- [ ] end
- [ ] sequence
- [ ] wrong sequence
- [ ] missing item
- [ ] disconnect
- [ ] reconnect
- [ ] rediscovery

---

## Cleanup

- [ ] exposure removed
- [ ] capability state removed
- [ ] device store removed
- [ ] stale fake cleanup
- [ ] stop while pending
- [ ] bounded QUIESCING

---

# 91. Integration tests

Flow:

```text
create 8 fake
wait READY barrier
verify device count
verify capabilities
enable 32 tools
tools/list
tools/call
BUSY test
disconnect one fake
verify result
reconnect
verify recovery
stop simulator
verify cleanup
start again
verify clean state
```

---

# 92. Result classification

Examples:

```text
APP-R1-L9: PASS
```

Nghĩa:

```text
9 logical-device upper-layer workload pass
1 physical BLE link validated
```

Chỉ được ghi:

```text
BLE-R9: PASS
```

khi:

```text
ble_link_count == 9
```

---

# 93. Immediate recommended sequence

Sau implementation:

```text
1. PROD-R1-L1
2. PERF-R1-L1-H0
3. PERF-R1-L3
4. PERF-R1-L6
5. PERF-R1-L9
6. CAP-108 / MCP-32
7. R1-L9 + 5 tools/call/s
8. WS-MOCK-R1-L9
9. XIAOZHI-REAL-R1-L9
10. 100 simulated reconnects
11. 50 WS reconnects
12. saturation/recovery
13. 24h soak
14. 72h soak
```

Sau đó mới tiếp tục:

```text
R3
R6
R9 real BLE qualification
```

---

# 94. Definition of Done

Framework v1.1 hoàn thành khi:

- [ ] Production build không chứa simulator runtime.
- [ ] Performance build riêng hoạt động.
- [ ] Harness overhead được đo.
- [ ] 8 fake devices được tạo.
- [ ] 1 real + 8 fake chạy đồng thời.
- [ ] Fake command đi qua real executor/dispatcher.
- [ ] ACK contract đúng.
- [ ] Capability discovery đi qua real capability pipeline.
- [ ] Dynamic MCP tools chạy với fake devices.
- [ ] Web load chạy với fake devices.
- [ ] Xiaozhi gọi được simulated tools.
- [ ] `/api/status` vẫn báo BLE thật.
- [ ] `/api/perf/status` báo logical/simulated riêng.
- [ ] Stop simulator dùng QUIESCING.
- [ ] Cleanup không để stale state.
- [ ] Runner hỗ trợ open-loop/closed-loop.
- [ ] Scenario-local heap minimum có trong report.
- [ ] JSON + CSV + Markdown report được sinh tự động.
- [ ] R1-L3/R1-L6/R1-L9 tự động.
- [ ] MCP 0/8/16/24/32 profile tự động.
- [ ] WS mock profile pass.
- [ ] Xiaozhi real integration pass.
- [ ] Overload không crash gateway.
- [ ] Recovery pass.
- [ ] No heap corruption.
- [ ] No TLSF assertion.
- [ ] No unexpected reboot.
- [ ] 24h soak pass.
- [ ] 72h soak pass trước production qualification.
- [ ] Framework sẵn sàng mở rộng lên real BLE R3/R6/R9.

---

# 95. Kết luận

Với chỉ một BLE device vật lý, mô hình phù hợp nhất là:

```text
1 real BLE
+
8 simulated devices
```

nhưng simulator phải ở đúng tầng.

Không mock MCP trực tiếp.

Không fake `ble_link_count`.

Không ghi trực tiếp capability/catalog state.

Không ACK đồng bộ.

Không dùng simulator queue lớn trong internal RAM.

Architecture đúng:

```text
device_store
    ↓
device_capabilities
    ↓
command_executor
    ↓
command_dispatcher
    ↓
hybrid transport
    ↓
sim worker
    ↓
correct ACK / capability notifications
    ↓
MCP exposure/catalog
    ↓
Web + MCP + Xiaozhi
```

v1.1 bổ sung các điều kiện quan trọng để performance result có ý nghĩa:

```text
separate perf binary baseline
correct ACK contract
full cleanup lifecycle
low-overhead simulator
scenario-local memory watermark
explicit load models
readiness barriers
deterministic WS mock
real Xiaozhi integration separated
```

Sau khi `APP-R1-L9` pass ổn định, phần còn thiếu duy nhất cho qualification đầy đủ là:

```text
real NimBLE multi-link testing
```

với 3/6/9 BLE devices thật.
