# ESP32 BLE Gateway – Internal RAM Optimization & PSRAM Migration Plan

**Project:** `esp-ble-gateway`  
**Repository:** https://github.com/hailp-vn38/esp-ble-gateway  
**Target MCU:** ESP32-S3  
**ESP-IDF:** **v6.1-rc1**  
**Flash:** 16 MB  
**PSRAM:** 8 MB Octal PSRAM  
**Document version:** **v1.1**  
**Previous version:** v1.0  
**Date:** 2026-08-30  
**Status:** Reviewed against current project implementation

---

# 1. Mục tiêu

Tài liệu này định nghĩa kế hoạch tối ưu bộ nhớ cho `esp-ble-gateway` với mục tiêu chính:

1. Tăng đáng kể **internal SRAM headroom**.
2. Tận dụng đúng 8 MB PSRAM đang còn dư nhiều.
3. Không làm giảm độ ổn định của:
   - NimBLE Central.
   - Wi-Fi / LwIP.
   - HTTP Web UI.
   - MCP HTTP endpoint.
   - Dynamic MCP tools.
   - Xiaozhi MCP WebSocket/TLS.
4. Không giảm:
   - 9 BLE links danh định.
   - 16 device records.
   - 12 capabilities/device.
   - MCP dynamic tool capacity chỉ để tiết kiệm RAM.
5. Giữ cố định **ESP-IDF v6.1-rc1**.
6. Mọi thay đổi phải:
   - đo được;
   - test được;
   - có acceptance criteria;
   - rollback được.

---

# 2. Hiện trạng bộ nhớ

Theo số đo runtime hiện tại:

```text
Internal RAM free       ~ 6 KB
Minimum free            ~ 3.4 KB
PSRAM free              ~ 7.8 MB
```

Đây là trạng thái không phù hợp cho production.

Vấn đề hiện tại không phải thiếu tổng RAM mà là:

> Application state, cache, catalog, response buffer và queue payload đang chiếm quá nhiều internal SRAM trong khi PSRAM gần như trống.

Internal RAM chỉ còn vài KB khiến gateway rất dễ lỗi tại các peak allocation như:

```text
TLS handshake
WebSocket reconnect
HTTP request
MCP tools/list
MCP tools/call
BLE notify burst
Wi-Fi / LwIP TX/RX
cJSON tree / serialization
NVS temporary blob
FreeRTOS task / queue creation
```

---

# 3. Kết quả review v1.0

Sau khi đối chiếu tài liệu v1.0 với code hiện tại, các kết luận chính:

## 3.1 Các nhận định vẫn đúng

Các đối tượng lớn cần ưu tiên chuyển khỏi internal SRAM:

```text
device_capabilities records
MCP exposure tables
MCP tool catalog
command executor result buffers
MCP sync dispatch result
large MCP / WS temporary buffers
```

Ngoài ra:

```text
board_status_sync
```

là một task polling có thể loại bỏ.

---

## 3.2 Các quyết định của v1.0 cần sửa

### Không bật mặc định:

```ini
CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y
```

làm giải pháp chính.

Lý do:

- option này ảnh hưởng rộng hơn application variables;
- có thể thay đổi placement của một số Wi-Fi/LwIP/network library BSS;
- mâu thuẫn với chiến lược của project:

```text
application state -> PSRAM
critical network/BLE state -> internal RAM
```

### Không giảm capability queue:

```text
32 -> 8
```

theo cách cũ.

Capability discovery có thể tạo burst nhiều event cho một snapshot nên queue depth 8 có nguy cơ drop event.

### Không giảm HTTP stack:

```text
12 KB -> 8 KB
```

trước khi dọn các local variable lớn đang nằm trên HTTP task stack.

### Không tạo thêm một hệ thống memory telemetry độc lập

Project đã có memory telemetry trong `gateway_status`.

Cần chuẩn hóa thành một nguồn chung thay vì duplicate.

### Existing heap corruption phải trở thành P0

Test repository hiện có baseline:

```text
TLSF block_trim_free assertion
mcp_endpoint failures
test suite chưa hoàn tất sạch
```

Không nên thay đổi allocator layout lớn trước khi isolate hoặc fix corruption hiện tại.

---

# 4. Mục tiêu RAM sau tối ưu

## 4.1 Gateway bình thường, Xiaozhi OFF

```text
Internal free steady state      >= 80 KB
Minimum internal free           >= 50 KB
Largest internal block          >= 40 KB
```

## 4.2 9 BLE + Web + MCP

```text
Internal free steady state      >= 50 KB
Minimum internal free           >= 32 KB
Largest internal block          >= 32 KB
```

## 4.3 9 BLE + Web + MCP + Xiaozhi WSS/TLS

```text
Internal free steady state      >= 40 KB
Minimum internal free           >= 24 KB
Largest internal block          >= 24 KB
```

Đây là **release engineering targets** của project.

---

# 5. Chiến lược kiến trúc được chọn

Architecture target:

```text
Large application state
cache
catalog
snapshots
large payloads
large response buffers
temporary serialization buffers
              |
              v
            PSRAM
```

Trong khi:

```text
FreeRTOS control state
BLE connection state
NimBLE host/controller
Wi-Fi critical memory
mutex/semaphore handles
small FSM state
ISR/DMA-sensitive memory
              |
              v
        Internal SRAM
```

---

# 6. Quy tắc allocation mới

## 6.1 Không dùng global BSS migration làm mặc định

Không bật mặc định:

```ini
CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y
```

trong production plan v1.1.

Application modules tự quyết định placement bằng `memory_policy`.

---

## 6.2 Runtime allocation thay static large BSS

Thay:

```c
static large_type_t s_data[N];
```

bằng:

```c
static large_type_t *s_data;
```

và allocate trong init:

```c
s_data = gw_mem_calloc(
    N,
    sizeof(*s_data),
    GW_MEM_EXTERNAL_PREFERRED
);
```

---

## 6.3 Vì sao dùng `GW_MEM_EXTERNAL_PREFERRED`

Production:

```text
CONFIG_SPIRAM=y
large allocation > fallback max
```

=> block lớn thực tế phải vào PSRAM.

Test build hiện có thể không bật PSRAM.

Do đó `EXTERNAL_PREFERRED` giúp:

```text
production -> PSRAM
unit test without PSRAM -> internal fallback
```

tránh phá test architecture.

---

## 6.4 Không dùng `GW_MEM_EXTERNAL_REQUIRED` rộng rãi ngay

Chỉ dùng khi:

- production path thực sự yêu cầu PSRAM;
- test build đã được cấu hình PSRAM tương ứng;
- failure semantics được xử lý rõ.

---

# 7. PHASE 0 – Fix / isolate existing heap corruption

Đây là prerequisite trước memory relocation lớn.

Repository hiện đã ghi nhận:

```text
assert failed:
block_trim_free
tlsf_control_functions.h:548
```

và test MCP chưa có baseline 0 fail.

---

## 7.1 Mục tiêu

Trước khi tối ưu RAM cần xác định:

```text
double free?
use-after-free?
buffer overwrite?
wrong allocator/free pair?
async lifetime issue?
test-only fixture issue?
```

---

## 7.2 Việc cần làm

- [ ] Reproduce TLSF assertion ổn định.
- [ ] Xác định test/component tạo corruption.
- [ ] Kiểm tra:
  - [ ] async MCP responder lifetime.
  - [ ] queue full cleanup.
  - [ ] cloned responder ownership.
  - [ ] malloc/free pairing.
  - [ ] cJSON ownership.
  - [ ] async HTTP request lifecycle.
- [ ] Bật heap poisoning/debug nếu cần.
- [ ] Có baseline mới không heap corruption.
- [ ] Lưu test result trước khi refactor RAM.

---

## Acceptance criteria

Ít nhất phải đạt một trong hai:

### Preferred

```text
0 TLSF assertions
0 heap corruption
```

### Minimum acceptable before RAM refactor

Root cause đã isolate rõ, có test riêng reproduce và refactor RAM không che mất bug.

---

# 8. PHASE 1 – Chuẩn hóa memory telemetry

Project hiện đã có trong `gateway_status`:

```text
internal free
internal minimum free
internal largest block

PSRAM free
PSRAM minimum free
PSRAM largest block
```

Không tạo một hệ thống thứ hai.

---

## 8.1 Refactor đề xuất

Tách low-level memory snapshot thành helper chung:

```c
typedef struct {
    size_t internal_free;
    size_t internal_min_free;
    size_t internal_largest;

    bool psram_ready;
    size_t psram_free;
    size_t psram_min_free;
    size_t psram_largest;
} gw_memory_snapshot_t;
```

API:

```c
void gw_memory_snapshot(gw_memory_snapshot_t *out);
```

Đặt trong:

```text
components/memory_policy/
```

hoặc module memory telemetry riêng nếu sau này có lý do rõ ràng.

---

## 8.2 Consumers

```text
gateway_status
perf_metrics
Xiaozhi diagnostics
boot diagnostics
performance tests
```

đều dùng cùng helper.

---

## 8.3 Không duplicate metric implementation

Không để:

```text
gateway_status -> tự query heap
memory_policy -> tự query heap
perf_metrics   -> tự query heap
```

thành ba implementation riêng.

---

# 9. PHASE 2 – `device_capabilities` records -> PSRAM

Đây vẫn là mục tiêu số 1.

Current model:

```c
static capability_record_t
    s_records[DEVICE_STORE_MAX_DEVICES];
```

Với:

```text
DEVICE_STORE_MAX_DEVICES = 16
DEVICE_CAP_MAX_PER_DEVICE = 12
```

Mỗi record chứa:

```text
committed snapshot
+
staging snapshot
+
operation state
+
refresh state
```

Ước lượng:

```text
~39–40 KB internal BSS
```

---

## 9.1 Refactor

Thay:

```c
static capability_record_t
    s_records[DEVICE_STORE_MAX_DEVICES];
```

bằng:

```c
static capability_record_t *s_records;
```

Trong init:

```c
s_records = gw_mem_calloc(
    DEVICE_STORE_MAX_DEVICES,
    sizeof(*s_records),
    GW_MEM_EXTERNAL_PREFERRED
);

if (s_records == NULL) {
    return ESP_ERR_NO_MEM;
}
```

---

## 9.2 CMake

`device_capabilities` hiện chưa require `memory_policy`.

Update:

```cmake
REQUIRES
    cbor_codec
    device_store
    nvs_flash
    esp_timer
    freertos
    memory_policy
```

---

## 9.3 Deinit / test reset

Component hiện thiên về single-shot init.

Cần đảm bảo test reset hoặc future deinit xử lý:

```c
gw_mem_free(s_records);
s_records = NULL;
```

nếu lifecycle cho phép.

Nếu production deliberately lifetime-to-reboot:

- document ownership;
- test reset vẫn cần cleanup để tránh test leak.

---

## 9.4 Giữ internal

Không chuyển:

```text
mutex handle
queue handle
task handle
owner FSM
generation counters
listener pointers
```

---

## Expected gain

```text
~35–40 KB internal SRAM
```

---

## Acceptance criteria

- [ ] Init OK.
- [ ] Load capability cache từ NVS OK.
- [ ] Initial discovery OK.
- [ ] Manual refresh OK.
- [ ] Reconnect device OK.
- [ ] Capability revision handling OK.
- [ ] No use-after-free.
- [ ] Internal RAM tăng ít nhất 30 KB.

---

# 10. PHASE 3 – Redesign capability queue payload

## 10.1 Không giảm depth 32

Giữ:

```c
#define CAP_EVENT_QUEUE_DEPTH 32
```

Lý do:

Một snapshot 12 capability có thể tạo khoảng:

```text
1 begin
12 item
1 end
1 completion
```

chưa tính READY / REFRESH / DISCONNECT.

Queue 8 là quá aggressive.

---

## 10.2 Vấn đề thật sự

Current event chứa:

```c
gw_message_t message;
```

ngay trong mỗi queue item.

Do đó 32-slot queue giữ rất nhiều internal RAM dù phần lớn event không dùng message.

---

## 10.3 Thiết kế mới

Đề xuất:

```c
typedef struct {
    capability_event_type_t type;
    char device_id[GW_MSG_DEVICE_ID_LEN];

    uint32_t operation_id;
    uint32_t refresh_generation;

    gw_message_t *message;

    device_cap_submit_result_t completion;
} capability_event_t;
```

---

## 10.4 Với `CAP_EVENT_NOTIFY`

Allocate:

```c
gw_message_t *copy =
    gw_mem_alloc(
        sizeof(*copy),
        GW_MEM_EXTERNAL_PREFERRED
    );
```

copy message rồi queue pointer.

Worker:

```c
handle_...(...);
gw_mem_free(event.message);
```

---

## 10.5 Event không cần message

Các event:

```text
READY
REFRESH
DISCONNECT
COMPLETION
```

không allocate message.

---

## 10.6 Queue full handling

Nếu queue full:

```text
free payload ngay
increment dropped metric
rate-limit log
return
```

Không leak.

---

## 10.7 Metrics

Thêm:

```text
cap_queue_enqueued
cap_queue_dropped
cap_queue_high_watermark
cap_message_alloc_fail
```

---

## Expected gain

```text
~8–10 KB internal SRAM
```

mà vẫn giữ depth 32.

---

## Acceptance criteria

- [ ] 0 drop trong normal operation.
- [ ] 0 leak khi queue full.
- [ ] 0 double free.
- [ ] 12-capability snapshot hoàn chỉnh.
- [ ] Burst connect nhiều device không corruption.
- [ ] Disconnect giữa discovery không leak message.

---

# 11. PHASE 4 – MCP exposure state -> PSRAM

Current large arrays:

```c
s_enabled[]
s_persisted[]
s_entries[]
```

Với:

```text
CONFIG_MCP_DYNAMIC_TOOL_MAX_ENABLED = 32
CONFIG_MCP_EXPOSURE_RECORD_MAX = 96
```

Estimated footprint:

```text
s_enabled       ~6–7 KB
s_persisted     ~8 KB
s_entries       ~9 KB

total           ~23–24 KB
```

---

## 11.1 `mcp_tool_exposure.c`

Thay:

```c
static enabled_entry_t s_enabled[...];
static mcp_exposure_persisted_record_t s_persisted[...];
```

bằng:

```c
static enabled_entry_t *s_enabled;
static mcp_exposure_persisted_record_t *s_persisted;
```

Allocate trong init:

```c
s_enabled = gw_mem_calloc(
    CONFIG_MCP_DYNAMIC_TOOL_MAX_ENABLED,
    sizeof(*s_enabled),
    GW_MEM_EXTERNAL_PREFERRED
);

s_persisted = gw_mem_calloc(
    CONFIG_MCP_EXPOSURE_RECORD_MAX,
    sizeof(*s_persisted),
    GW_MEM_EXTERNAL_PREFERRED
);
```

---

## 11.2 `mcp_tool_catalog.c`

Thay:

```c
static catalog_entry_t s_entries[...];
```

bằng:

```c
static catalog_entry_t *s_entries;
```

Allocate tại catalog init.

---

## 11.3 CMake

Update `mcp_tool_exposure` dependency:

```text
memory_policy
```

---

## 11.4 Init failure cleanup

Nếu:

```text
mutex OK
enabled OK
persisted FAIL
queue...
task...
```

phải unwind đúng thứ tự.

Không để partial-init leak.

---

## 11.5 Behavior không đổi

Giữ nguyên:

```text
NVS schema
catalog revision
tool naming
tool digest
enable/disable behavior
needs_review/orphaned state
device rename behavior
```

---

## Expected gain

```text
~20–24 KB internal SRAM
```

---

# 12. PHASE 5 – Command executor result buffers -> PSRAM

Current worker:

```c
typedef struct {
    TaskHandle_t task;
    dispatch_result_t result;
    uint32_t jobs_processed;
} command_worker_t;
```

Trong đó:

```text
dispatch_result_t payload ~4 KB
```

Với 2 workers:

```text
~8 KB internal BSS
```

---

## 12.1 Phương án được chọn

Không đưa toàn bộ worker control state sang PSRAM.

Tách result buffer:

```c
typedef struct {
    TaskHandle_t task;
    dispatch_result_t *result;
    uint32_t jobs_processed;
} command_worker_t;
```

Allocate mỗi result:

```c
worker->result =
    gw_mem_calloc(
        1,
        sizeof(*worker->result),
        GW_MEM_EXTERNAL_PREFERRED
    );
```

---

## 12.2 Lý do tốt hơn move whole worker struct

Giữ:

```text
TaskHandle_t
counter
worker control
```

internal.

Chỉ đưa 4 KB response storage sang PSRAM.

---

## 12.3 CMake

Add:

```text
memory_policy
```

dependency.

---

## Expected gain

```text
~8 KB internal SRAM
```

---

# 13. PHASE 6 – MCP synchronous dispatch result -> PSRAM

Ngoài executor workers, MCP endpoint hiện có thêm:

```c
static dispatch_result_t s_dispatch_result;
```

trong MCP tools path.

Footprint:

```text
~4 KB internal BSS
```

---

## 13.1 Refactor

Thay static object bằng pointer:

```c
static dispatch_result_t *s_dispatch_result;
```

Allocate khi MCP tool subsystem init.

Hoặc lazy-init dưới mutex.

---

## 13.2 Ownership

Một mutex hiện serialize access.

Giữ model:

```text
single shared result
+
mutex
```

nhưng backing storage ở PSRAM.

---

## Expected gain

```text
~4 KB internal SRAM
```

---

# 14. PHASE 7 – MCP / WS temporary allocation policy

## 14.1 MCP exposure store

Current NVS blob temporary dùng:

```c
malloc(blob_size);
```

Đổi sang:

```c
gw_mem_alloc(
    blob_size,
    GW_MEM_EXTERNAL_PREFERRED
);
```

---

## 14.2 MCP HTTP request body

MCP request max:

```text
4096 bytes
```

Current body:

```c
malloc(content_len + 1);
```

Nên cân nhắc:

```c
GW_MEM_EXTERNAL_PREFERRED
```

cho request lớn.

Không bắt buộc đối với request nhỏ.

---

## 14.3 Xiaozhi WS RX completed message

Current:

```c
char *message =
    malloc(s_bridge.rx_length + 1);
```

Đổi sang:

```c
char *message =
    gw_mem_alloc(
        s_bridge.rx_length + 1,
        GW_MEM_EXTERNAL_PREFERRED
    );
```

---

## 14.4 Lưu ý về gain

Production đã có:

```ini
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=1024
```

nên standard `malloc()` >1024 bytes thường đã ưu tiên PSRAM.

Do đó thay đổi này chủ yếu mang ý nghĩa:

```text
explicit policy
predictable fallback
consistent allocation contract
```

Không được cộng toàn bộ temporary buffer vào "guaranteed RAM reclaimed".

---

# 15. PHASE 8 – Harden `memory_policy`

Current fallback kiểm tra:

```text
free internal after theoretical subtraction
largest internal before allocation
```

Trong khi Kconfig mô tả largest block floor sau fallback.

---

## 15.1 Không dùng approximation đơn giản

Không chỉ:

```c
largest_internal - size
```

vì fragmentation không đảm bảo block được lấy từ largest block.

---

## 15.2 Phương án enforcement tốt hơn

Pseudo:

```c
void *p = heap_caps_malloc(
    size,
    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
);

if (p == NULL) {
    return NULL;
}

size_t free_after = ...;
size_t largest_after = ...;

if (free_after < CONFIG_GW_MEM_INTERNAL_FLOOR_BYTES ||
    largest_after < CONFIG_GW_MEM_INTERNAL_LARGEST_FLOOR_BYTES) {
    free(p);
    return NULL;
}

return p;
```

---

## 15.3 Preflight vẫn giữ

Trước allocation vẫn nên check:

```text
size <= fallback max
free internal > size
largest internal >= size
```

để tránh allocation chắc chắn fail.

---

## 15.4 Metrics

Thêm:

```text
external_alloc_success
external_alloc_fail
internal_fallback_attempt
internal_fallback_success
internal_fallback_rejected_floor
```

---

# 16. PHASE 9 – Remove `board_status_sync`

Current task:

```text
stack = 3072
poll every 300 ms
```

chỉ map Wi-Fi state -> board status.

---

## 16.1 Kiến trúc mới

Thay polling bằng event-driven:

```text
Wi-Fi state transition
        |
        v
board_io_set_status(...)
```

---

## 16.2 Mapping

```text
BOOT_CONNECTING   -> WIFI_CONNECTING
RECONNECTING      -> WIFI_CONNECTING
PROVISIONING      -> PROVISIONING
TESTING           -> PROVISIONING
RESTART_PENDING   -> PROVISIONING
CONNECTED         -> READY
FAILED            -> ERROR
UNINITIALIZED     -> BOOTING
```

---

## Expected gain

```text
~3 KB internal task stack
1 task removed
less scheduler wakeups
```

---

# 17. PHASE 10 – HTTP stack cleanup trước khi tune

Current HTTP gateway task:

```text
WEB_GATEWAY_STACK_SIZE = 12288
```

Không giảm ngay.

---

## 17.1 Existing large stack locals

Ví dụ capability handler có:

```c
dispatch_result_t result;
```

~4 KB trên HTTP stack.

Một số handler khác còn có:

```text
body[1024]
device_capability_snapshot_t
query buffers
cJSON call frames
```

---

## 17.2 Việc phải làm trước

Audit tất cả web handlers:

```text
web_capability_api.c
web_command_api.c
web_device_api.c
web_exposure_api.c
web_settings_api.c
web_wifi_api.c
web_system_api.c
```

Search:

```text
dispatch_result_t local
device_capability_snapshot_t local
large char arrays
large arrays
```

---

## 17.3 Refactor rule

Không để object > ~1 KB nằm lâu trên HTTP task stack nếu có thể:

```text
shared serialized buffer
PSRAM temporary allocation
streaming response
```

---

## 17.4 Sau cleanup mới tune

Step:

```text
12288
  ↓
10240
  ↓
8192
```

mỗi bước phải test riêng.

---

## Safety target

Worst-case HTTP workload:

```text
remaining stack >= 1.5 KB
```

---

# 18. PHASE 11 – Command worker stack tuning

Project đã có:

```c
uxTaskGetStackHighWaterMark()
```

trong command executor stats.

Do đó không cần tạo telemetry mới.

---

## 18.1 Test workloads

```text
gateway command
BLE device command
ACK timeout
large result
parallel Web + MCP
queue full
```

---

## 18.2 Tune

Current:

```text
4096 bytes
```

Candidate:

```text
4096 -> 3584
```

sau đó nếu còn dư:

```text
3584 -> 3072
```

Không nhảy thẳng nếu chưa đo.

---

# 19. PHASE 12 – BLE auxiliary stack tuning

Current approximate:

```text
ble_notify       4096
ble_reconnect    4096
ble_ident        3072
```

---

## 19.1 Không đụng NimBLE memory placement

Giữ:

```ini
CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL=y
```

---

## 19.2 Không chuyển task stacks global sang PSRAM

Không bật:

```ini
CONFIG_FREERTOS_PLACE_TASK_STACKS_IN_EXT_RAM=y
```

---

## 19.3 Tune từng task

Sau watermark:

```text
ble_notify       4096 -> 3584 / 3072
ble_reconnect    4096 -> 3072
ble_ident        3072 -> 2560
```

chỉ nếu measurement chứng minh an toàn.

---

# 20. PHASE 13 – Xiaozhi lifecycle memory

Current startup behavior đã đúng:

```text
enabled = false
   ->
skip mcp_ws_bridge_init()
skip mcp_ws_bridge_start()
```

Giữ nguyên.

---

## 20.1 Khi disabled phải đảm bảo

```text
no bridge lifecycle task
no bridge queue
no RX buffer
no WS client task
no TLS session
```

---

## 20.2 Khi enabled

Track RAM tại:

```text
before bridge init
after bridge init
before WS connect
after WS client create
after TLS handshake
READY
after reconnect
```

---

# 21. PHASE 14 – sdkconfig tuning

## 21.1 IDF

Giữ cố định:

```text
ESP-IDF v6.1-rc1
```

---

## 21.2 Không bật external BSS global mặc định

Giữ:

```ini
# CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY is not set
```

trong v1.1.

Có thể benchmark riêng sau này nếu muốn.

---

## 21.3 Internal reserve

Giữ:

```ini
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=65536
```

Không giảm.

---

## 21.4 Allocation threshold

Current:

```ini
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=1024
```

Sau khi structural refactor pass qualification:

```ini
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=512
```

A/B test.

---

## 21.5 Wi-Fi/LwIP PSRAM

Không bật trong early phase.

Chỉ benchmark sau cùng:

```ini
CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y
```

và chỉ giữ nếu:

```text
internal headroom cải thiện
Wi-Fi latency ổn định
HTTP không regression
TLS reconnect ổn định
BLE 9-link không regression
```

---

# 22. Test-build compatibility

Current test project không mặc định bật PSRAM.

Do đó:

```text
GW_MEM_EXTERNAL_REQUIRED
```

không nên dùng làm default cho các module đang được unit test nếu chưa cập nhật test target.

---

## 22.1 Preferred strategy

Application caches:

```text
GW_MEM_EXTERNAL_PREFERRED
```

Production:

```text
large block -> PSRAM
```

Test without PSRAM:

```text
fallback internal
```

---

## 22.2 Alternative future strategy

Nếu muốn production enforce PSRAM tuyệt đối:

```text
CONFIG_GW_PRODUCTION_REQUIRE_PSRAM=y
```

và test build disable option này.

Không cần trong v1.1.

---

# 23. Estimated reclaimed internal RAM

Sau review thực tế:

| Refactor | Estimated gain |
|---|---:|
| `device_capabilities.s_records` -> PSRAM | ~35–40 KB |
| MCP enabled/persisted/catalog -> PSRAM | ~20–24 KB |
| command executor results -> PSRAM | ~8 KB |
| MCP shared dispatch result -> PSRAM | ~4 KB |
| capability queue payload redesign | ~8–10 KB |
| remove `board_status_sync` | ~3 KB |
| HTTP stack cleanup/tuning | ~2–4 KB |
| command/BLE stack tuning | ~2–4 KB |
| malloc threshold 1024 -> 512 | workload dependent |

---

## Conservative engineering estimate

Không tính temporary `malloc()` vào guaranteed gain:

```text
>= 65 KB internal SRAM reclaimed
```

---

## Likely practical result

Sau full structural optimization:

```text
~80–95 KB improvement
```

tùy linker layout, task stack use và runtime allocation.

---

# 24. Revised implementation order

## Stage 0 – Stability prerequisite

```text
heap corruption
TLSF assertion
test baseline
```

Checklist:

- [ ] isolate TLSF bug
- [ ] no silent allocator bug masking
- [ ] baseline report

---

## Stage 1 – Biggest deterministic gains

1. `device_capabilities.s_records` -> runtime PSRAM.
2. MCP `s_enabled` -> PSRAM.
3. MCP `s_persisted` -> PSRAM.
4. MCP catalog `s_entries` -> PSRAM.
5. command executor result buffers -> PSRAM.
6. MCP shared dispatch result -> PSRAM.

Expected:

```text
~65–75 KB gain
```

---

## Stage 2 – Queue architecture

7. Keep capability queue depth 32.
8. Move notify message payload out of queue item.
9. Put notify message copies in PSRAM.
10. Add queue metrics.

Expected:

```text
~8–10 KB gain
```

---

## Stage 3 – Dynamic allocation policy

11. WS RX completed message -> memory policy.
12. MCP exposure NVS blob -> memory policy.
13. Audit MCP request/response buffers.
14. Harden internal fallback post-allocation checks.

---

## Stage 4 – Remove unnecessary task

15. Replace `board_status_sync` polling with event-driven state update.

---

## Stage 5 – Stack cleanup/tuning

16. Remove large HTTP stack-local objects.
17. Measure HTTP stack.
18. 12K -> 10K.
19. Test.
20. 10K -> 8K only if safe.
21. Tune command workers.
22. Tune BLE helper tasks.

---

## Stage 6 – Allocator/global policy

23. A/B:

```text
MALLOC_ALWAYSINTERNAL 1024 -> 512
```

24. Full qualification.

25. Optional A/B:

```text
SPIRAM_TRY_ALLOCATE_WIFI_LWIP
```

---

# 25. Memory qualification matrix

## 25.1 Boot profile

```text
Wi-Fi configured
0 BLE devices
MCP HTTP enabled
Xiaozhi disabled
```

Record:

```text
internal free
internal min
internal largest
PSRAM free
PSRAM largest
```

---

## 25.2 BLE scaling

```text
1 BLE
3 BLE
6 BLE
9 BLE
```

---

## 25.3 Web workload

With 9 BLE:

```text
dashboard refresh
settings
BLE scan
device detail
capability refresh
MCP exposure enable/disable
command execution
```

---

## 25.4 MCP HTTP

```text
initialize
tools/list
tools/call
invalid request
large tools/list
parallel requests
queue full
timeout
```

---

## 25.5 Xiaozhi

With 9 BLE:

```text
enable
reboot
WSS connect
TLS handshake
initialize
tools/list
tools/call
manual reconnect
Wi-Fi disconnect/reconnect
catalog update
tools refresh
```

---

# 26. Regression tests bắt buộc

## `device_capabilities`

- [ ] NVS load
- [ ] begin/item/end
- [ ] duplicate sequence
- [ ] invalid sequence
- [ ] disconnect while running
- [ ] disconnect while queued
- [ ] refresh
- [ ] unchanged revision
- [ ] changed content same revision warning
- [ ] queue-full cleanup
- [ ] alloc failure cleanup

## `mcp_tool_exposure`

- [ ] load store
- [ ] enable tool
- [ ] disable tool
- [ ] device rename
- [ ] device delete
- [ ] capability changed
- [ ] orphan handling
- [ ] persistence failure
- [ ] partial init allocation failure

## `command_executor`

- [ ] init allocation failure
- [ ] queue full
- [ ] timeout
- [ ] parallel workers
- [ ] deinit
- [ ] result storage ownership

## `mcp_endpoint`

- [ ] synchronous tool call
- [ ] async device call
- [ ] large tools/list
- [ ] OOM handling
- [ ] responder cleanup
- [ ] queue full
- [ ] no heap corruption

---

# 27. Release gate

Reject release candidate nếu:

```text
minimum internal free < 24 KB
largest internal block < 20 KB
ESP_ERR_NO_MEM trong normal qualified workload
TLS handshake fail vì heap
task creation fail
heap corruption
TLSF assertion
normal capability event drop
memory leak qua reconnect cycles
watchdog reset
BLE host reset do memory pressure
```

---

# 28. Soak test

Minimum:

```text
24h
```

Production qualification:

```text
72h
```

Scenario:

```text
9 BLE
Wi-Fi active
Web active
MCP active
Xiaozhi active
periodic tools/list
periodic tools/call
periodic capability refresh
periodic BLE reconnect
periodic Xiaozhi reconnect
```

Track:

```text
internal free
internal min
internal largest
PSRAM free
PSRAM min
queue drop
BLE reconnect failure
WS reconnect
MCP errors
HTTP 5xx
heap corruption
reset count
```

---

# 29. Developer checklist

## P0 – Stability

- [ ] Fix/isolate current TLSF assertion.
- [ ] Establish test baseline.
- [ ] Keep ESP-IDF v6.1-rc1.

## P1 – Deterministic PSRAM relocation

- [ ] Refactor memory telemetry helper.
- [ ] Move capability records.
- [ ] Move MCP enabled state.
- [ ] Move MCP persisted records.
- [ ] Move MCP catalog.
- [ ] Move executor result buffers.
- [ ] Move MCP shared dispatch result.
- [ ] Confirm >= 50 KB reclaimed before queue tuning.

## P2 – Queue & dynamic allocations

- [ ] Keep capability depth 32.
- [ ] Shrink queue item.
- [ ] Move notify payload copies to PSRAM.
- [ ] WS RX allocation via memory policy.
- [ ] MCP NVS blob via memory policy.
- [ ] Harden allocator fallback.

## P3 – Task/stack

- [ ] Remove `board_status_sync`.
- [ ] Audit HTTP stack locals.
- [ ] Measure HTTP watermark.
- [ ] Tune HTTP stack gradually.
- [ ] Measure executor workers.
- [ ] Tune worker stack.
- [ ] Measure BLE helper tasks.
- [ ] Tune BLE helper stacks.

## P4 – Global allocator policy

- [ ] A/B threshold 1024 -> 512.
- [ ] Run 9-link combined workload.
- [ ] Run Xiaozhi TLS workload.
- [ ] Run 24h soak.
- [ ] Run 72h soak.
- [ ] Optional Wi-Fi/LwIP PSRAM A/B.

---

# 30. Definition of Done

Memory optimization hoàn tất khi:

- [ ] ESP-IDF v6.1-rc1 build sạch.
- [ ] Unit/integration tests không có heap corruption.
- [ ] Không còn TLSF assertion.
- [ ] Normal boot internal free >= 80 KB.
- [ ] 9 BLE + Web + MCP internal free >= 50 KB.
- [ ] Xiaozhi TLS combined workload internal free >= 40 KB.
- [ ] Minimum internal free >= 24 KB.
- [ ] Largest internal block >= 24 KB trong production workload.
- [ ] Không `ESP_ERR_NO_MEM` trong qualified workload.
- [ ] Xiaozhi disabled không tạo WS/TLS runtime resources.
- [ ] Capability queue không drop trong normal operation.
- [ ] MCP tool refresh/reconnect hoạt động.
- [ ] 24h soak pass.
- [ ] 72h soak pass trước production release.

---

# 31. Những phương án KHÔNG được chọn

## Không giảm chức năng

Không giảm:

```text
9 BLE links
16 device records
12 capabilities/device
dynamic MCP tools
```

chỉ để cứu RAM.

---

## Không chuyển NimBLE memory sang PSRAM

Giữ:

```ini
CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL=y
```

---

## Không bật global task stacks trong PSRAM

Không dùng:

```ini
CONFIG_FREERTOS_PLACE_TASK_STACKS_IN_EXT_RAM=y
```

làm giải pháp chung.

---

## Không bật global external BSS làm bước đầu

Giữ:

```ini
# CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY is not set
```

trong implementation mặc định v1.1.

---

## Không giảm queue capability xuống 8

Giữ depth 32 và giảm item footprint.

---

## Không giảm HTTP stack trước cleanup

Thứ tự bắt buộc:

```text
remove large locals
-> measure
-> tune
```

---

# 32. Kết luận

Sau review thực tế với code hiện tại, vấn đề RAM của gateway được xác định rõ:

```text
Internal SRAM gần cạn
PSRAM gần như trống
```

nhưng phần lớn pressure đến từ **application-owned large state**, không phải bắt buộc từ NimBLE/Wi-Fi.

Hướng triển khai ưu tiên:

```text
1. Stabilize heap baseline
2. Move large application state to PSRAM
3. Keep capability queue depth but shrink item
4. Move large result buffers
5. Harden allocator policy
6. Remove unnecessary task
7. Clean HTTP stack
8. Tune stacks
9. Tune global allocator last
```

Expected deterministic structural gain:

```text
>= 65 KB internal SRAM
```

Likely after full optimization:

```text
~80–95 KB improvement
```

mà không cần giảm tính năng.

Đây là phương án được khuyến nghị để triển khai tiếp cho `esp-ble-gateway` trên **ESP-IDF v6.1-rc1**.
