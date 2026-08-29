# ESP32 BLE Gateway — Performance Test Framework & Qualification Plan

**Project:** `esp-ble-gateway`  
**Repository:** `https://github.com/hailp-vn38/esp-ble-gateway`  
**Related device repo:** `esp-ble-device`  
**Target hardware:** ESP32-S3 / 16 MB Flash / 8 MB Octal PSRAM  
**Framework:** ESP-IDF v6.1-rc1  
**Document status:** Implementation specification  
**Version:** 1.0

---

# 1. Mục tiêu

Tài liệu này định nghĩa một framework kiểm thử hiệu năng hoàn chỉnh cho gateway.

Mục tiêu không chỉ là đo:

```text
nhanh / chậm
```

mà phải xác định:

```text
ngưỡng vận hành ổn định
+
điểm saturation
+
memory headroom
+
recovery behavior
+
long-term stability
```

của ESP32-S3 khi các subsystem chạy đồng thời:

```text
BLE Central
+
Web UI / HTTP server
+
Local MCP endpoint
+
Xiaozhi MCP WebSocket bridge
+
Wi-Fi
+
TLS
+
PSRAM / internal SRAM allocator
```

---

# 2. Hiện trạng dự án liên quan

Production configuration hiện dùng:

```text
ESP32-S3
CPU 240 MHz
16 MB Flash
8 MB Octal PSRAM
NimBLE Central
CONFIG_BT_NIMBLE_MAX_CONNECTIONS=9
MTU=256
```

Do đó:

```text
9 BLE links
```

là tải danh định tối đa cho performance qualification.

Không dùng:

```text
10+ BLE links
```

làm throughput qualification nếu firmware hiện chỉ build tối đa 9 connections.

Test >9 chỉ dùng để kiểm tra:

```text
graceful rejection
stability
no crash
```

---

# 3. Performance test architecture

Framework đề xuất:

```text
                    ┌─────────────────────────┐
                    │ PC Performance Runner   │
                    │                         │
                    │ HTTP load               │
                    │ MCP client              │
                    │ Xiaozhi mock            │
                    │ Metrics collector       │
                    │ Report generator        │
                    └───────────┬─────────────┘
                                │ Wi-Fi
                                ▼
                ┌───────────────────────────────┐
                │ ESP32-S3 Gateway              │
                │                               │
                │ perf_metrics                  │
                │ BLE Central                   │
                │ Web Server                    │
                │ MCP Endpoint                  │
                │ Xiaozhi WS Bridge             │
                │ Memory Policy                 │
                └───────────────┬───────────────┘
                                │ BLE
             ┌──────────────────┼──────────────────┐
             ▼                  ▼                  ▼
        PERF Device 1      PERF Device 2      PERF Device N
```

---

# 4. Performance test layers

Chia test thành 5 tầng.

## Layer 1 — Component benchmark

Đo riêng:

```text
command_dispatcher
command_executor
CBOR codec
MCP serialization
tool exposure
memory allocator
```

Mục tiêu:

```text
cost của từng component
```

---

## Layer 2 — Subsystem load

Đo riêng:

```text
BLE
Web HTTP
MCP HTTP
Xiaozhi WSS
```

Mục tiêu:

```text
capacity của từng subsystem
```

---

## Layer 3 — Combined workload

Chạy:

```text
BLE
+
HTTP
+
MCP
+
Xiaozhi
```

đồng thời.

Đây là test quan trọng nhất.

---

## Layer 4 — Soak / endurance

Chạy:

```text
12 h
24 h
72 h
```

để tìm:

```text
memory leak
fragmentation
task leak
reconnect degradation
```

---

## Layer 5 — Regression benchmark

So sánh:

```text
firmware mới
vs
baseline firmware
```

---

# 5. Component mới: `perf_metrics`

Nên tạo:

```text
components/perf_metrics/
    CMakeLists.txt
    Kconfig.projbuild
    include/
        perf_metrics.h
    perf_metrics.c
```

Kconfig:

```text
CONFIG_GW_PERF_METRICS
```

Default:

```text
n
```

Benchmark build:

```text
CONFIG_GW_PERF_METRICS=y
```

---

# 6. Metrics bắt buộc

## 6.1 Memory

```text
total free heap

internal free
internal minimum
internal largest block

DMA free
DMA largest block

PSRAM free
PSRAM minimum
PSRAM largest block
```

DMA metrics:

```c
heap_caps_get_free_size(
    MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);

heap_caps_get_largest_free_block(
    MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
```

---

# 7. Task telemetry

Theo dõi các task quan trọng:

```text
BLE host
BLE reconnect supervisor
command executor
HTTP server
mcp_ws_bridge
mcp_ws_client
```

Metrics:

```text
stack high watermark
task count
task lifetime
```

Đặc biệt kiểm tra:

```text
mcp_ws_client task leak
```

sau repeated reconnect.

---

# 8. BLE metrics

Counters:

```text
connections_active
connections_total
disconnects
reconnects

notifications_rx
notifications_dropped
notifications_out_of_order

commands_tx
commands_completed
commands_timeout
commands_failed
```

Latency:

```text
BLE command RTT
```

---

# 9. Dispatcher / executor metrics

Theo dõi:

```text
submitted
completed
busy
timeout
failed
```

Queue:

```text
current depth
peak depth
```

Latency:

```text
submit → completion
```

---

# 10. HTTP metrics

Theo endpoint:

```text
request_count
error_count
timeout_count
bytes_tx
```

Latency:

```text
p50
p95
p99
max
```

Không chỉ dùng average.

---

# 11. MCP metrics

Theo dõi:

```text
initialize count
tools/list count
tools/call count
MCP errors
```

Đặc biệt:

```text
tool_count
tools_list_json_bytes
tools_list_generation_us
tools_list_tx_us
```

---

# 12. Xiaozhi WebSocket metrics

Theo dõi:

```text
connect_count
disconnect_count
reconnect_count

TLS connect time
MCP handshake time
time_to_ready

WS RX bytes
WS TX bytes

TX failures
RX failures

manual reconnect count
```

---

# 13. Histogram / percentile

Không chỉ lưu:

```text
average
```

Mỗi latency metric cần:

```text
min
mean
p50
p95
p99
max
```

Các metric chính:

```text
HTTP latency
MCP request latency
executor latency
BLE RTT
Xiaozhi connect time
Xiaozhi reconnect time
```

---

# 14. Device performance firmware mode

Trong `esp-ble-device`, thêm:

```text
PERF_TEST_MODE
```

Mục đích:

```text
generate deterministic BLE traffic
```

---

# 15. PERF device configuration

Device cần hỗ trợ:

```text
notify_rate_hz
payload_size
sequence_start
burst_count
response_delay_ms
echo_mode
```

Ví dụ:

```json
{
  "type": "perf.start",
  "rate_hz": 10,
  "payload_size": 128
}
```

---

# 16. Notification payload

Mỗi packet cần:

```text
sequence number
timestamp
payload
```

Ví dụ logical structure:

```json
{
  "seq": 1234,
  "timestamp_us": 812321211,
  "data": "..."
}
```

Gateway dùng `seq` để tính:

```text
received
lost
duplicate
out_of_order
```

---

# 17. BLE load matrix

| Links | Notify rate/device | Payload |
|---:|---:|---:|
| 1 | 1 Hz | 32 B |
| 1 | 10 Hz | 128 B |
| 5 | 1 Hz | 128 B |
| 5 | 10 Hz | 128 B |
| 9 | 1 Hz | 128 B |
| 9 | 5 Hz | 128 B |
| 9 | 10 Hz | 128 B |
| 9 | 10 Hz | near MTU |

---

# 18. Nominal BLE workload

Đề xuất baseline:

```text
9 BLE devices

5 notify/sec/device

128-byte payload
```

Tổng:

```text
45 notifications/sec
```

Đây là workload danh định ban đầu.

---

# 19. BLE saturation test

Tăng:

```text
rate
```

theo:

```text
1
5
10
20
...
```

cho đến khi xuất hiện:

```text
message loss
latency spike
executor queue saturation
BLE disconnect
memory pressure
```

Mục tiêu:

```text
find practical saturation point
```

Không coi saturation point là operating target.

Operating target phải thấp hơn saturation.

---

# 20. BLE command RTT test

Luồng:

```text
PC
 ↓
MCP
 ↓
Gateway
 ↓
BLE
 ↓
Device
 ↓
BLE response
 ↓
Gateway
 ↓
MCP response
 ↓
PC
```

Tách thành:

```text
gateway processing latency

BLE RTT

end-to-end latency
```

---

# 21. Web server benchmark

Profiles:

| ID | BLE | Xiaozhi | HTTP clients |
|---|---:|---|---:|
| W0 | 0 | OFF | 1 |
| W1 | 5 | OFF | 1 |
| W2 | 9 | OFF | 1 |
| W3 | 9 | ON | 1 |
| W4 | 9 | ON | 4 |
| W5 | 9 | ON | 8 |

Test endpoints:

```text
GET /api/status
GET /api/devices
GET /api/settings
device detail
dashboard static assets
```

---

# 22. HTTP result metrics

Ghi:

```text
requests/sec
success
errors
timeouts
p50
p95
p99
max
```

Mục tiêu chính:

```text
HTTP không bị starvation
```

khi:

```text
BLE + TLS + MCP
```

đang tải.

---

# 23. Local MCP benchmark

Test:

```text
initialize
tools/list
tools/call
```

Tool registry matrix:

| Tools | Profile |
|---:|---|
| 0 | baseline |
| 10 | small |
| 25 | normal |
| 50 | stress |
| max actual | qualification |

---

# 24. `tools/list` benchmark

Đo:

```text
tool_count
serialized bytes

generation latency

TX latency

internal heap delta

DMA largest delta

PSRAM delta
```

Đây là metric quan trọng do gateway đã từng gặp TLS/AES memory failure với large TX.

---

# 25. Xiaozhi performance tests

## Connect benchmark

```text
connect
→ TLS ready
→ MCP initialize
→ READY
```

Đo:

```text
TCP/TLS connect
MCP handshake
total time-to-ready
```

---

# 26. Xiaozhi reconnect test

Minimum:

```text
100 manual reconnect cycles
```

Sau:

```text
#1
#10
#25
#50
#100
```

record:

```text
internal free
internal largest
DMA free
DMA largest
PSRAM free
task count
```

---

# 27. Reconnect acceptance

Không được có:

```text
monotonic heap loss
monotonic DMA largest degradation
task leak
TLS session leak
stale WS session
```

---

# 28. Xiaozhi large TX

Test:

```text
tools/list 2 KB
tools/list 4 KB
tools/list 8 KB
tools/list 16 KB
```

nếu protocol/application thực tế cho phép.

Đo:

```text
TX success
TX latency
AES failures
DMA largest
```

---

# 29. Integrated profile P0 — Idle baseline

```text
BLE = 0
Web clients = 0
Local MCP = idle
Xiaozhi = OFF
```

Duration:

```text
10 min
```

Mục tiêu:

```text
memory baseline
```

---

# 30. Profile P1 — Network services

```text
BLE = 0
Web = active
Local MCP = active
Xiaozhi = OFF
```

Duration:

```text
10 min
```

---

# 31. Profile P2 — Single BLE

```text
BLE = 1
Web = active
MCP = active
Xiaozhi = OFF
```

Duration:

```text
15 min
```

---

# 32. Profile P3 — Medium BLE

```text
BLE = 5
Web = active
MCP = active
Xiaozhi = OFF
```

Duration:

```text
30 min
```

---

# 33. Profile P4 — Max BLE without Xiaozhi

```text
BLE = 9
Web = active
MCP = active
Xiaozhi = OFF
```

Duration:

```text
30 min
```

---

# 34. Profile P5 — Nominal maximum

Đây là profile qualification chính.

```text
BLE devices = 9

each:
    5 notify/sec
    128-byte payload

Web:
    1 normal browser

Local MCP:
    1 call/sec

Xiaozhi:
    connected
    normal MCP traffic

Wi-Fi:
    normal LAN
```

Duration:

```text
60 min
```

---

# 35. Profile P6 — Stress

```text
BLE = 9
10 notify/sec/device

HTTP clients = 4

Local MCP = 5 calls/sec

Xiaozhi = ON

tools/list repeated

periodic reconnect
```

Duration:

```text
30 min
```

Stress test goal:

```text
find breaking point
```

PASS không yêu cầu latency đẹp.

Nhưng bắt buộc:

```text
no crash
no watchdog
no heap corruption
no unrecoverable subsystem failure
```

---

# 36. Profile P7 — Soak

```text
BLE = 9 nominal
Web = normal
MCP = normal
Xiaozhi = ON
```

Duration:

```text
24 h
```

Release candidate:

```text
72 h
```

---

# 37. Soak telemetry interval

Không log mỗi packet.

Snapshot mỗi:

```text
10–30 sec
```

Ghi:

```text
timestamp
uptime

internal free
internal min
internal largest

DMA free
DMA largest

PSRAM free
PSRAM largest

BLE links

executor queue

HTTP counters
MCP counters
WS counters
```

---

# 38. Recovery performance tests

Performance qualification phải bao gồm failure recovery.

## Wi-Fi outage

```text
disconnect router 30 sec
```

Đo:

```text
Wi-Fi recovery time
Web recovery time
MCP recovery time
Xiaozhi READY time
```

---

# 39. BLE device outage

Test:

```text
remove 1 device
```

trong 9-link workload.

Đo:

```text
remaining 8 link stability
reconnect supervisor behavior
latency impact
```

---

# 40. BLE reconnect storm

Test:

```text
disconnect all 9 devices
```

sau đó bật lại gần đồng thời.

Đo:

```text
time to recover 9 links
peak memory
CPU load
queue saturation
```

---

# 41. Router reboot

Test:

```text
router reboot
```

Gateway BLE connections có thể tiếp tục local operation.

Đo:

```text
BLE survival
Wi-Fi reconnect
Web recovery
MCP recovery
Xiaozhi recovery
```

---

# 42. External PC test runner

Nên tạo:

```text
tools/perf/
    run.py
    config.py

    gateway_client.py
    mcp_client.py
    xiaozhi_mock.py

    metrics.py
    report.py

    profiles/
        p0_idle.json
        p1_network.json
        p2_ble1.json
        p3_ble5.json
        p4_ble9.json
        p5_nominal_max.json
        p6_stress.json
        p7_soak.json
```

---

# 43. Runner responsibilities

PC runner phải:

```text
load profile
↓
reset gateway counters
↓
wait stabilization
↓
start BLE load
↓
start HTTP load
↓
start MCP load
↓
start Xiaozhi mock traffic
↓
poll gateway metrics
↓
capture serial log
↓
stop workload
↓
generate report
```

---

# 44. Result files

Mỗi run:

```text
results/
    <timestamp>_<profile>/
        metadata.json
        metrics.csv
        summary.json
        uart.log
        report.md
```

---

# 45. Metadata

Lưu:

```text
firmware version
git commit
ESP-IDF version
sdkconfig hash
board
profile
duration
number BLE devices
Wi-Fi RSSI
test runner version
```

Nếu không lưu metadata thì benchmark không reproducible.

---

# 46. Suggested `summary.json`

Ví dụ:

```json
{
  "profile": "P5",
  "duration_sec": 3600,

  "ble": {
    "links": 9,
    "rx": 162000,
    "lost": 0
  },

  "http": {
    "requests": 3600,
    "errors": 0,
    "p95_ms": 32
  },

  "mcp": {
    "calls": 3600,
    "errors": 0,
    "p95_ms": 71
  },

  "memory": {
    "internal_min": 81234,
    "internal_largest_min": 40120,
    "dma_largest_min": 8192
  }
}
```

---

# 47. Xiaozhi mock server

Không phụ thuộc Xiaozhi Internet service cho benchmark cơ bản.

Tạo:

```text
tools/perf/xiaozhi_mock.py
```

Mock:

```text
WebSocket accept
initialize
notifications/initialized
tools/list
tools/call
disconnect
```

---

# 48. Local WebSocket test mode

Benchmark build có thể dùng:

```text
CONFIG_MCP_WS_ALLOW_INSECURE=y
```

để chạy:

```text
ws://PC-IP:PORT
```

Production giữ:

```text
wss://
```

Sau local benchmark phải chạy thêm integration qualification với WSS thật.

---

# 49. Performance API

Có thể thêm development-only endpoint:

```text
GET /api/perf
POST /api/perf/reset
```

Chỉ compile khi:

```text
CONFIG_GW_PERF_METRICS=y
```

---

# 50. `/api/perf` response

Ví dụ:

```json
{
  "memory": {
    "internal_free": 103000,
    "internal_min": 92000,
    "internal_largest": 52000,
    "dma_free": 81000,
    "dma_largest": 32000,
    "psram_free": 7000000
  },

  "ble": {
    "links": 9,
    "notify_rx": 100000,
    "notify_drop": 0
  },

  "executor": {
    "queue_current": 0,
    "queue_peak": 6
  },

  "xiaozhi": {
    "reconnects": 4,
    "tx_failures": 0
  }
}
```

---

# 51. Logging policy

Do not use high-volume UART logging during benchmarks.

Performance build:

```text
INFO:
profile lifecycle
major errors

DEBUG:
disabled by default
```

Counters phải thay log-per-packet.

---

# 52. Memory warning thresholds

Project hiện có memory policy floor:

```text
internal free floor = 64 KB
internal largest floor = 32 KB
```

Có thể dùng làm initial warning:

```text
Internal free < 64 KB → WARN
Internal largest < 32 KB → WARN
```

DMA diagnostic:

```text
DMA largest < 4 KB → WARN
```

Các số này ban đầu chỉ dùng diagnostic.

Không phải final PASS threshold.

---

# 53. PASS criteria — hard failures

Mọi nominal qualification phải có:

```text
0 crash
0 panic
0 watchdog reset
0 heap corruption

0 esp-aes allocation failure

0 unrecovered TLS failure

0 task leak

0 permanent BLE manager failure

0 monotonic memory leak
```

---

# 54. BLE nominal acceptance

Trong RF environment tốt:

```text
BLE application message loss = 0
```

cho P5.

Nếu packet loss xảy ra:

```text
must be measured and explained
```

không được bỏ qua.

---

# 55. Latency acceptance

Không hard-code latency target ngay ở phiên bản đầu.

Flow:

```text
run baseline 3–5 times
↓
calculate stable distribution
↓
define thresholds
```

Sau đó regression gate có thể là:

```text
p95 increase < 20%
p99 increase < 25%
```

so với known-good baseline.

---

# 56. Throughput regression gate

Sau baseline:

```text
throughput decrease < 10%
```

trong cùng profile/hardware/RF condition.

---

# 57. Memory regression gate

So sánh:

```text
internal_min
internal_largest_min
DMA_largest_min
PSRAM_min
```

Firmware mới không được làm giảm headroom đáng kể nếu không có lý do kiến trúc rõ ràng.

---

# 58. RF test consistency

BLE performance phụ thuộc RF.

Benchmark cần:

```text
fixed gateway position
fixed device positions
fixed antenna
fixed Wi-Fi AP
fixed channel if possible
```

Ghi:

```text
Wi-Fi RSSI
BLE RSSI
```

trong metadata.

---

# 59. Baseline runs

Mỗi profile:

```text
minimum 3 runs
recommended 5 runs
```

Không lấy một run duy nhất làm baseline.

---

# 60. Development phases

## Phase 0 — Metric contract

- [ ] Define metrics.
- [ ] Define counter types.
- [ ] Define reset semantics.
- [ ] Define `/api/perf`.
- [ ] Define report JSON schema.

Acceptance:

```text
metric contract frozen
```

---

# 61. Phase 1 — `perf_metrics`

- [ ] Create component.
- [ ] Internal RAM metrics.
- [ ] DMA metrics.
- [ ] PSRAM metrics.
- [ ] Counter API.
- [ ] Histogram API.
- [ ] Reset API.
- [ ] `/api/perf`.

Acceptance:

```text
PC can collect gateway telemetry
```

---

# 62. Phase 2 — BLE PERF device

- [ ] Add PERF mode.
- [ ] Configurable notification rate.
- [ ] Configurable payload size.
- [ ] Sequence number.
- [ ] Echo command.
- [ ] Burst mode.
- [ ] Runtime start/stop.

Acceptance:

```text
deterministic BLE load
```

---

# 63. Phase 3 — PC runner

- [ ] Profile parser.
- [ ] Gateway API client.
- [ ] MCP load client.
- [ ] Metrics collector.
- [ ] UART capture.
- [ ] Report output.

Acceptance:

```text
one command executes a complete benchmark
```

---

# 64. Phase 4 — Xiaozhi mock

- [ ] WebSocket server.
- [ ] MCP initialize.
- [ ] tools/list.
- [ ] tools/call.
- [ ] forced disconnect.
- [ ] large payload.

Acceptance:

```text
Xiaozhi benchmark does not require Internet
```

---

# 65. Phase 5 — Baseline P0–P5

- [ ] P0.
- [ ] P1.
- [ ] P2.
- [ ] P3.
- [ ] P4.
- [ ] P5.

Each:

```text
3–5 runs
```

Acceptance:

```text
stable baseline exists
```

---

# 66. Phase 6 — Saturation / stress

- [ ] BLE rate scaling.
- [ ] HTTP concurrency scaling.
- [ ] MCP request scaling.
- [ ] Large tools/list.
- [ ] Xiaozhi reconnect stress.

Acceptance:

```text
breaking points identified
```

---

# 67. Phase 7 — Recovery

- [ ] Wi-Fi outage.
- [ ] Router reboot.
- [ ] Single BLE outage.
- [ ] BLE reconnect storm.
- [ ] Xiaozhi outage.

Acceptance:

```text
all subsystems recover predictably
```

---

# 68. Phase 8 — Soak

- [ ] 12 h development.
- [ ] 24 h qualification.
- [ ] 72 h release candidate.

Acceptance:

```text
no monotonic resource degradation
```

---

# 69. Phase 9 — Regression gate

Store known-good baseline.

CI / release process:

```text
new firmware
↓
run selected benchmark
↓
compare baseline
↓
PASS / FAIL
```

---

# 70. Recommended CI subset

Full hardware qualification không cần chạy mỗi commit.

Per PR:

```text
component tests
P0
small HTTP/MCP benchmark
```

Nightly:

```text
P2
P3
selected reconnect tests
```

Release candidate:

```text
P0–P7
```

---

# 71. Test results dashboard

Optional future enhancement:

```text
results/*.json
```

có thể generate:

```text
HTML trend dashboard
```

theo:

```text
commit
internal_min
DMA_largest
p95 latency
BLE loss
```

---

# 72. Key performance questions framework must answer

Sau khi triển khai, team phải trả lời được:

```text
9 BLE links có ổn định không?

Mỗi device chịu được notify rate bao nhiêu?

Web UI có bị chậm khi BLE full load không?

MCP tool call p95 là bao nhiêu?

tools/list lớn bao nhiêu trước khi memory pressure xuất hiện?

Xiaozhi reconnect 100 lần có leak không?

DMA largest còn bao nhiêu ở P5?

Gateway chạy 72 giờ có degrade không?

Wi-Fi outage mất bao lâu để recover?

BLE reconnect storm có làm gateway mất ổn định không?
```

---

# 73. Recommended priority

Thứ tự ưu tiên:

```text
1. perf_metrics
2. PERF device
3. PC runner
4. P0–P5 baseline
5. Xiaozhi reconnect / large TX
6. stress
7. recovery
8. soak
```

Không nên bắt đầu từ P6/P7 trước khi telemetry và baseline hoàn chỉnh.

---

# 74. Definition of Done

Performance framework hoàn tất khi:

- [ ] Có `perf_metrics`.
- [ ] Có DMA telemetry.
- [ ] Có task stack telemetry.
- [ ] Có BLE sequence/loss counters.
- [ ] Có dispatcher/executor counters.
- [ ] Có HTTP latency metrics.
- [ ] Có MCP latency metrics.
- [ ] Có Xiaozhi lifecycle counters.
- [ ] Có PERF firmware mode cho device.
- [ ] Có external Python runner.
- [ ] Có Xiaozhi local mock.
- [ ] Có profiles P0–P7.
- [ ] Có reproducible metadata.
- [ ] Có machine-readable `summary.json`.
- [ ] Có baseline firmware result.
- [ ] Có saturation point measurement.
- [ ] Có 24 h soak result.
- [ ] Có 72 h RC qualification procedure.
- [ ] Có performance regression policy.
- [ ] Gateway đạt P5 không crash/watchdog/OOM/AES failure.
- [ ] 9-link BLE operation được qualification thực tế.

---

# 75. Final architecture

```text
PERF devices
      │
      │ deterministic BLE load
      ▼
ESP32-S3 Gateway
      │
      ├── perf_metrics
      ├── BLE metrics
      ├── executor metrics
      ├── HTTP metrics
      ├── MCP metrics
      ├── Xiaozhi metrics
      └── memory metrics
      │
      ▼
PC performance runner
      │
      ├── workload generator
      ├── metrics collector
      ├── UART capture
      ├── baseline compare
      └── report generator
```

Mục tiêu cuối cùng không phải tìm ra con số benchmark đẹp nhất.

Mục tiêu là xác định một **operating envelope có thể chứng minh được** cho gateway:

```text
9 BLE devices
+
Wi-Fi
+
Web UI
+
MCP
+
Xiaozhi TLS/WSS
```

và đảm bảo mọi firmware release mới không làm suy giảm operating envelope đó.
