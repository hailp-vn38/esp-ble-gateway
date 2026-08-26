# Tài liệu hướng dẫn refactor `components/log_buffer` — v2

> Implementation specification đã hiệu chỉnh sau review v1.
>
> Mục tiêu của v2 là giữ `log_buffer` nhỏ, deterministic và dễ audit, đồng thời loại bỏ các điểm còn mơ hồ trong v1 về lifecycle của `esp_log_set_vprintf()`, error semantics của read API, compatibility của `/api/logs`, và failure policy trong `app_main()`.
>
> Baseline đối chiếu: code hiện tại của `components/log_buffer`, `main/main.c` và `components/web_server/web_system_api.c` trên branch `main`.

---

# 1. Mục tiêu

`log_buffer` là một component nhỏ nhưng nằm trực tiếp trên global ESP-IDF logging path. Refactor phải ưu tiên an toàn hệ thống hơn tính năng.

Sau refactor, component phải đáp ứng:

- recent gateway logs chỉ nằm trong RAM;
- memory footprint cố định;
- không cấp phát heap trên capture hot path;
- circular buffer giữ đúng chronological order;
- capture path không chờ mutex;
- mọi API copy dữ liệu ra caller đều có explicit capacity;
- read API vẫn phân biệt được buffer rỗng với failure;
- init chỉ cài global log hook một lần trong lifetime firmware;
- không có production `deinit()` có thể race với concurrent logging;
- timestamp nội bộ dùng fixed-width monotonic uptime;
- HTTP `/api/logs` giữ backward compatibility;
- lỗi init của log buffer không được làm gateway fail boot;
- có dropped-log observability;
- callback hook phải re-entrant;
- test đủ wrap-around, bounded-copy, lifecycle, contention và concurrent `ESP_LOGx()`.

---

# 2. Phạm vi hiện tại

Component hiện gồm:

```text
components/log_buffer/
├── CMakeLists.txt
├── include/
│   └── log_buffer.h
├── log_buffer.c
└── test/
    ├── CMakeLists.txt
    └── test_log_buffer.c
```

Public API hiện tại:

```c
void log_buffer_init(void);
void log_buffer_push(const char *text);
int log_buffer_get_all(log_entry_t *out_entries);
int log_buffer_get_recent(log_entry_t *out_entries, int max_entries);
```

Cấu hình hiện tại:

```c
#define LOG_BUFFER_CAPACITY 64
#define LOG_ENTRY_MAX_LEN   192
```

Data model hiện tại:

```c
typedef struct {
    char text[LOG_ENTRY_MAX_LEN];
    long timestamp_ms;
} log_entry_t;
```

---

# 3. Những điểm tốt cần giữ

## 3.1. Static circular buffer

Giữ static storage vì:

- không fragment heap;
- memory usage xác định;
- phù hợp ESP32-S3;
- phù hợp mục tiêu recent diagnostic logs.

Payload text:

```text
64 × 192 = 12,288 bytes
```

Tổng RAM sau timestamp/padding vẫn hợp lý.

## 3.2. Ring arithmetic hiện tại đúng

Khi chưa đầy:

```text
oldest = 0
```

Khi đầy:

```text
oldest = s_head
```

`get_recent()` hiện trả:

```text
oldest selected -> newest selected
```

Behavior này phải được giữ.

## 3.3. String write đã bounded

`vsnprintf()` và explicit NUL termination phải được giữ.

## 3.4. Console output vẫn phải hoạt động

Hook phải tiếp tục forward log đến original ESP-IDF `vprintf` handler.

---

# 4. Các vấn đề bắt buộc phải sửa

## 4.1. HIGH — loại bỏ unbounded `log_buffer_get_all()`

API:

```c
int log_buffer_get_all(log_entry_t *out_entries);
```

không biết caller cấp bao nhiêu entry.

Đây là contract không memory-safe.

### Quyết định

Xóa hoàn toàn:

```c
log_buffer_get_all()
```

Public read API duy nhất:

```c
int log_buffer_get_recent(
    log_entry_t *out_entries,
    size_t capacity);
```

Muốn lấy toàn bộ:

```c
log_entry_t entries[LOG_BUFFER_CAPACITY];

int count = log_buffer_get_recent(
    entries,
    LOG_BUFFER_CAPACITY);
```

### Invariant

> Không public API nào được copy vào caller memory mà không nhận explicit capacity.

---

## 4.2. HIGH — giữ error semantics cho read API

Không dùng `size_t` return value nếu tất cả failure cùng collapse thành `0`.

API đích:

```c
int log_buffer_get_recent(
    log_entry_t *out_entries,
    size_t capacity);
```

Contract:

```text
> 0  số entry copy thành công
  0  buffer hợp lệ nhưng đang rỗng
 -1  invalid argument
 -1  component chưa initialized
 -1  reader không lấy được mutex trong timeout
```

Điều này giữ behavior tương thích với Web API hiện tại:

```c
int count = log_buffer_get_recent(...);

if (count < 0) {
    return web_send_api_error(...);
}
```

### Invariant

> `0` chỉ có nghĩa là một read hợp lệ của buffer rỗng.

---

## 4.3. HIGH — capture path không được chờ ring mutex

Hiện tại writer có thể:

```c
xSemaphoreTake(..., pdMS_TO_TICKS(1000))
```

Đây là latency risk nghiêm trọng vì call path là:

```text
ESP_LOGx
  |
  v
log_buffer_vprintf
  |
  +--> original vprintf
  |
  +--> capture
         |
         +--> ring mutex
```

### Quyết định

Writer dùng try-lock:

```c
if (xSemaphoreTake(s_mutex, 0) != pdTRUE) {
    increment_dropped_count();
    return false;
}
```

Không retry.

Không sleep.

Không block.

### Invariant chính xác

Không tuyên bố toàn bộ `log_buffer_vprintf()` là non-blocking vì original ESP-IDF output handler có thể có latency riêng.

Invariant đúng là:

> RAM capture path MUST NOT wait for the ring mutex.

---

## 4.4. HIGH — global hook là install-once trong firmware lifetime

v1 đề xuất public `deinit()` để restore callback cũ. Thiết kế đó tạo nguy cơ race với concurrent logging.

Ví dụ nguy hiểm:

```text
Task A                           Task B

log_buffer_vprintf()
                                deinit()
                                  restore hook
                                  delete mutex

log_buffer_push()
  access deleted mutex
```

ESP-IDF log hook là global và callback có thể được gọi từ nhiều task.

### Quyết định v2

**Không có public production `log_buffer_deinit()`.**

Lifecycle:

```text
boot
 |
 +--> log_buffer_init()
          |
          +--> install hook
          |
          +--> hook tồn tại đến reset/reboot
```

Đây phù hợp firmware lifecycle hiện tại.

### Test isolation

Tests dùng:

```c
log_buffer_clear();
```

Không uninstall/install hook giữa từng test.

Nếu sau này thật sự cần test helper để restore hook, helper đó phải:

- chỉ compile trong test build;
- có precondition không task nào còn phát log;
- không thuộc production public API.

### Invariant

> `log_buffer` sở hữu ESP-IDF `vprintf` hook từ successful first init đến firmware reset.

---

## 4.5. HIGH — init phải idempotent và non-fatal cho gateway boot

API đích:

```c
esp_err_t log_buffer_init(void);
```

Semantics:

```text
first call:
  initialize state
  create mutex
  install hook

next calls:
  return ESP_OK
  do not clear logs
  do not install another hook
```

### Failure policy trong `app_main()`

`log_buffer` là diagnostic subsystem.

Failure của nó không được ngăn:

- NVS;
- Wi-Fi;
- BLE;
- Web server;
- MCP;
- command dispatcher

khởi động.

Pattern bắt buộc:

```c
esp_err_t log_error = log_buffer_init();
if (log_error != ESP_OK) {
    ESP_LOGW(
        TAG,
        "RAM log buffer unavailable: %s",
        esp_err_to_name(log_error));
}
```

Sau đó boot tiếp tục.

### Invariant

> `log_buffer_init()` failure degrades diagnostics only; gateway core services still boot.

---

## 4.6. MEDIUM — tách `clear()` khỏi `init()`

Repeated init không được xóa log.

Thêm:

```c
void log_buffer_clear(void);
```

Semantics:

```text
clear:
  reset ring contents
  reset s_head
  reset s_count
  keep hook installed
  keep component initialized
```

### Dropped counter

`clear()` **không reset** dropped counter.

Lý do: dropped counter là lifecycle diagnostic metric, không phải ring content.

Nếu tests cần reset metric, có thể dùng test-only reset helper hoặc test ordering phù hợp.

---

## 4.7. MEDIUM — timestamp nội bộ phải explicit

Đổi:

```c
long timestamp_ms;
```

thành:

```c
uint64_t uptime_ms;
```

Value:

```c
(uint64_t)(esp_timer_get_time() / 1000ULL)
```

Semantic:

```text
milliseconds since boot
not wall-clock time
```

---

## 4.8. HIGH — không phá contract `/api/logs`

HTTP API hiện trả array:

```json
[
  {
    "text": "...",
    "timestamp_ms": 12345
  }
]
```

Refactor `log_buffer` không được tự ý đổi thành:

```json
{
  "count": 1,
  "dropped": 0,
  "entries": [...]
}
```

và không rename HTTP field thành `uptime_ms`.

### Quyết định

Internal:

```c
entry.uptime_ms
```

Web compatibility layer vẫn trả:

```c
cJSON_AddNumberToObject(
    item,
    "timestamp_ms",
    s_log_snapshot[i].uptime_ms);
```

### Invariant

> Refactor `log_buffer` v2 không thay đổi JSON shape của `/api/logs`.

---

## 4.9. MEDIUM — dropped counter phải có semantics cố định

Thêm:

```c
uint32_t log_buffer_get_dropped_count(void);
```

`dropped_count` tăng **chỉ khi**:

1. input entry hợp lệ;
2. component initialized;
3. capture muốn commit vào ring;
4. writer không lấy được ring mutex ngay lập tức.

Không tăng khi:

```text
text == NULL
text == ""
component chưa initialized
line sau trim trở thành empty
```

### Counter update

Dùng atomic increment hoặc primitive tương đương không blocking.

Không dùng mutex ring chỉ để tăng dropped counter.

---

# 5. Public API đích

```c
#ifndef LOG_BUFFER_H
#define LOG_BUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LOG_BUFFER_CAPACITY 64U
#define LOG_ENTRY_MAX_LEN   192U

typedef struct {
    char text[LOG_ENTRY_MAX_LEN];
    uint64_t uptime_ms;
} log_entry_t;

/**
 * Initialize the RAM log buffer and install the ESP-IDF vprintf hook.
 *
 * Idempotent.
 *
 * A second call returns ESP_OK without clearing entries or installing
 * another hook.
 */
esp_err_t log_buffer_init(void);

/**
 * Clear stored entries while keeping the component initialized and
 * keeping the ESP-IDF log hook installed.
 */
void log_buffer_clear(void);

/**
 * Push one already-formatted text entry into the RAM ring.
 *
 * The capture operation never waits for the ring mutex.
 *
 * Returns true only if the entry is committed to the ring.
 */
bool log_buffer_push(const char *text);

/**
 * Copy up to `capacity` most recent entries into `out_entries`.
 *
 * Ordering:
 *   oldest selected entry -> newest selected entry
 *
 * Returns:
 *   >= 0 : number of copied entries
 *   -1   : invalid argument, unavailable component, or read lock timeout
 */
int log_buffer_get_recent(
    log_entry_t *out_entries,
    size_t capacity);

/**
 * Number of valid entries dropped due to writer contention.
 */
uint32_t log_buffer_get_dropped_count(void);

#ifdef __cplusplus
}
#endif

#endif
```

---

# 6. API contract chi tiết

## 6.1. `log_buffer_init()`

Return:

```text
ESP_OK
ESP_ERR_NO_MEM
ESP_FAIL / ESP_ERR_INVALID_STATE nếu implementation cần
```

Contract:

- successful first call initializes component;
- subsequent calls return `ESP_OK`;
- repeated call không clear;
- repeated call không install hook lần nữa;
- không leak semaphore;
- component chỉ publish ready state khi setup hoàn tất.

---

## 6.2. `log_buffer_clear()`

Contract:

- safe nếu component chưa initialized: no-op;
- reset ring entries;
- reset `s_head`;
- reset `s_count`;
- không uninstall hook;
- không reset dropped counter.

Implementation không nhất thiết phải `memset()` toàn bộ buffer nếu `s_count = 0` đã đủ correctness.

Nếu muốn tránh stale test/debug data, có thể memset trong clear.

---

## 6.3. `log_buffer_push()`

Input rules:

```text
NULL        -> false
empty       -> false
not ready   -> false
```

Valid entry:

- truncate tại `LOG_ENTRY_MAX_LEN - 1`;
- luôn NUL terminate;
- lấy `uptime_ms`;
- try-lock ring;
- fail lock -> increment dropped + false;
- success -> write/advance/unlock + true.

Không gọi:

- `ESP_LOGx()`;
- NVS;
- HTTP;
- malloc/free;
- blocking queue;
- long-running callbacks.

---

## 6.4. `log_buffer_get_recent()`

Validation:

```text
out_entries == NULL -> -1
capacity == 0       -> -1
not initialized     -> -1
```

Empty:

```text
initialized + no entries -> 0
```

Lock:

Reader có thể wait một bounded short timeout.

Đề xuất:

```c
#define LOG_BUFFER_READ_LOCK_TIMEOUT_MS 20U
```

Nếu timeout:

```text
return -1
```

Ordering:

Buffer:

```text
A B C D E
```

Capacity 3:

```text
C D E
```

---

# 7. Internal state đích

```c
static log_entry_t s_buffer[LOG_BUFFER_CAPACITY];

static size_t s_head;
static size_t s_count;

static SemaphoreHandle_t s_mutex;

static vprintf_like_t s_original_vprintf = &vprintf;

static bool s_initialized;
static bool s_hook_installed;

static uint32_t s_dropped_count;
```

Nếu muốn khóa init publication rõ hơn:

```c
typedef enum {
    LOG_BUFFER_STATE_UNINITIALIZED = 0,
    LOG_BUFFER_STATE_INITIALIZING,
    LOG_BUFFER_STATE_READY,
} log_buffer_state_t;
```

Một state enum tốt hơn hai bool nếu implementation cần concurrent-safe initialization.

---

# 8. Initialization concurrency

Hiện `app_main()` gọi `log_buffer_init()` rất sớm, nên normal production boot chỉ có một caller.

Tuy nhiên API idempotent phải không leak nếu future code gọi lại.

## 8.1. Quy tắc

Không giữ critical section trong khi:

- `xSemaphoreCreateMutex()`;
- `esp_log_set_vprintf()`;
- bất kỳ API có thể allocate/block.

## 8.2. Preferred pattern

Dùng state transition:

```text
UNINITIALIZED
    |
    v
INITIALIZING
    |
    +--> create mutex
    +--> reset state
    +--> install hook
    |
    v
READY
```

Nếu caller khác thấy:

```text
READY -> return ESP_OK
```

Nếu thấy:

```text
INITIALIZING
```

có thể:

- return `ESP_ERR_INVALID_STATE`, hoặc
- implement short deterministic wait.

V2 ưu tiên đơn giản:

> Production invariant vẫn là `app_main()` là first/primary initializer.

Do đó không cần over-engineer multi-caller init synchronization nếu không có real caller.

---

# 9. Re-entrancy requirement

`log_buffer_vprintf()` phải re-entrant.

Mọi temporary formatting state phải nằm trên stack:

```c
char line[LOG_ENTRY_MAX_LEN];

va_list serial_args;
va_list buffer_args;
```

Không dùng static scratch buffer.

Mutable shared state chỉ truy cập qua synchronization phù hợp:

```text
ring state        -> ring mutex
dropped counter   -> atomic
init/hook state   -> initialization synchronization
```

---

# 10. Producer concurrency model

```text
Task A ESP_LOGx ─┐
Task B ESP_LOGx ─┼──> log_buffer_vprintf()
Task C ESP_LOGx ─┘
                         |
                         +--> original vprintf
                         |
                         +--> local formatting
                         |
                         +--> try ring lock
                                  |
                     ┌────────────┴─────────────┐
                     |                          |
                    fail                      success
                     |                          |
             atomic dropped++          write ring entry
                     |                  advance head/count
                    return                     |
                                               v
                                             unlock
```

No caller waits for another log producer to finish ring capture.

---

# 11. Consumer concurrency model

Web API:

```text
HTTP task
   |
   v
log_buffer_get_recent()
   |
   +--> short bounded lock
   +--> copy snapshot
   +--> unlock
   |
   v
build cJSON
   |
   v
send HTTP response
```

Không giữ ring mutex trong:

- `cJSON_Create*`;
- serialization;
- socket send;
- HTTP response processing.

---

# 12. Ring invariants

Luôn phải giữ:

```text
0 <= s_head < LOG_BUFFER_CAPACITY
0 <= s_count <= LOG_BUFFER_CAPACITY
```

Successful push:

```c
s_head = (s_head + 1U) % LOG_BUFFER_CAPACITY;

if (s_count < LOG_BUFFER_CAPACITY) {
    s_count++;
}
```

Oldest:

```c
size_t oldest =
    s_count < LOG_BUFFER_CAPACITY
        ? 0U
        : s_head;
```

Recent subset:

```c
size_t count =
    s_count < capacity
        ? s_count
        : capacity;

size_t skip = s_count - count;
```

Index:

```c
size_t idx =
    (oldest + skip + i) % LOG_BUFFER_CAPACITY;
```

---

# 13. Hook implementation

Target shape:

```c
static int log_buffer_vprintf(
    const char *format,
    va_list args)
{
    va_list serial_args;
    va_copy(serial_args, args);

    int result =
        s_original_vprintf(format, serial_args);

    va_end(serial_args);

    char line[LOG_ENTRY_MAX_LEN];

    va_list buffer_args;
    va_copy(buffer_args, args);

    int length =
        vsnprintf(
            line,
            sizeof(line),
            format,
            buffer_args);

    va_end(buffer_args);

    if (length <= 0) {
        return result;
    }

    size_t stored_length =
        strnlen(line, sizeof(line));

    while (
        stored_length > 0U &&
        (line[stored_length - 1U] == '\n' ||
         line[stored_length - 1U] == '\r')) {

        line[--stored_length] = '\0';
    }

    if (stored_length > 0U) {
        (void)log_buffer_push(line);
    }

    return result;
}
```

---

# 14. Hook recursion rules

Trong:

```text
log_buffer_vprintf()
log_buffer_push()
dropped counter helper
ring helper
```

không được gọi:

```c
ESP_LOGI
ESP_LOGW
ESP_LOGE
ESP_LOGD
ESP_LOGV
```

Nếu internal failure xảy ra trên capture path:

- return;
- increment counter nếu đúng semantics;
- không log lại.

---

# 15. Timestamp compatibility

Internal struct:

```c
typedef struct {
    char text[LOG_ENTRY_MAX_LEN];
    uint64_t uptime_ms;
} log_entry_t;
```

Web API hiện tại vẫn publish:

```json
{
  "text": "...",
  "timestamp_ms": 12345
}
```

Mapping:

```c
cJSON_AddNumberToObject(
    item,
    "timestamp_ms",
    s_log_snapshot[i].uptime_ms);
```

Không đổi external key trong refactor này.

---

# 16. `/api/logs` migration bắt buộc

Code hiện tại:

```c
static log_entry_t s_log_snapshot[LOG_API_MAX_ENTRIES];

static esp_err_t logs_get_handler(httpd_req_t *request)
{
    int count =
        log_buffer_get_recent(
            s_log_snapshot,
            LOG_API_MAX_ENTRIES);

    if (count < 0) {
        return web_send_api_error(
            request,
            "500 Internal Server Error",
            "Could not read logs");
    }

    ...
}
```

Sau refactor vẫn giữ control flow này.

Chỉ đổi field internal:

```c
s_log_snapshot[i].timestamp_ms
```

thành:

```c
s_log_snapshot[i].uptime_ms
```

nhưng JSON key vẫn:

```text
timestamp_ms
```

---

# 17. Dropped metric integration

V2 không bắt buộc thay `/api/logs`.

Có hai lựa chọn future integration:

### Option A — `/api/status`

Thêm:

```json
{
  "log_dropped_count": 3
}
```

### Option B — chưa expose qua Web

Giữ metric chỉ cho firmware/tests.

V2 acceptance không yêu cầu đổi Web API ngoài field mapping nội bộ.

---

# 18. Memory ordering cho dropped counter

Preferred implementation dùng C11 atomic nếu toolchain/config project hỗ trợ ổn định:

```c
#include <stdatomic.h>

static atomic_uint_fast32_t s_dropped_count;
```

Increment:

```c
atomic_fetch_add_explicit(
    &s_dropped_count,
    1U,
    memory_order_relaxed);
```

Read:

```c
return (uint32_t)atomic_load_explicit(
    &s_dropped_count,
    memory_order_relaxed);
```

`memory_order_relaxed` đủ vì counter không dùng để synchronize ring state.

Nếu project policy không muốn C11 atomic, dùng ESP-IDF/port atomic primitive tương đương.

Không dùng ring mutex để tăng counter.

---

# 19. Truncation policy

```text
max stored chars = LOG_ENTRY_MAX_LEN - 1
always NUL terminated
```

Không malloc full line.

Không dynamic resize.

Không thêm `truncated` flag trong v2 nếu chưa có consumer cần.

---

# 20. CMake

Giữ component:

```cmake
idf_component_register(
    SRCS "log_buffer.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer freertos log
)
```

Không tạo dependency từ `log_buffer` sang `web_server`.

Dependency direction:

```text
web_server
   |
   v
log_buffer
```

Không ngược lại.

---

# 21. `main.c` migration

Hiện:

```c
void app_main(void)
{
    log_buffer_init();

    ...
}
```

Đổi thành:

```c
void app_main(void)
{
    esp_err_t log_error = log_buffer_init();

    if (log_error != ESP_OK) {
        ESP_LOGW(
            TAG,
            "RAM log buffer unavailable: %s",
            esp_err_to_name(log_error));
    }

    ...
}
```

Nếu init fail:

```text
gateway continues boot
```

Nếu init thành công:

```text
all following ESP_LOGx entries are captured
```

---

# 22. Error handling policy

## Init

Return explicit `esp_err_t`.

Caller decides whether to degrade or abort.

Project policy:

```text
degrade only
do not abort gateway
```

## Push

```text
true  = committed
false = not committed
```

No internal ESP logging.

## Read

```text
>=0 = valid read
-1  = failure
```

---

# 23. Test plan bắt buộc

## 23.1. Basic push/read

Push:

```text
A B C
```

Expected:

```text
A B C
```

---

## 23.2. Recent subset

Buffer:

```text
A B C D E
```

Capacity 2:

```text
D E
```

---

## 23.3. Empty buffer semantics

After init + clear:

```c
int count = log_buffer_get_recent(...);
```

Expected:

```text
count == 0
```

---

## 23.4. Invalid read semantics

Cases:

```text
NULL output
capacity = 0
```

Expected:

```text
-1
```

---

## 23.5. Exact capacity

Push exactly:

```text
LOG_BUFFER_CAPACITY
```

Expected:

```text
count == LOG_BUFFER_CAPACITY
first == first pushed
last == last pushed
```

---

## 23.6. Capacity + 1

Push:

```text
LOG_BUFFER_CAPACITY + 1
```

Expected:

```text
oldest first entry overwritten
count stays capacity
```

---

## 23.7. Multiple wraps

Push:

```text
2 * LOG_BUFFER_CAPACITY + 7
```

Expected:

```text
last LOG_BUFFER_CAPACITY entries
correct chronological order
```

---

## 23.8. Bounded-output canary

Setup:

```text
stored = 64
requested capacity = 4
```

Memory layout:

```text
[4 log_entry_t][canary]
```

Expected:

```text
exactly 4 entries modified
canary unchanged
```

---

## 23.9. Long text

Input > max length.

Expected:

```text
strlen == LOG_ENTRY_MAX_LEN - 1
last byte == '\0'
```

---

## 23.10. CR/LF trimming

Inputs:

```text
"abc\n"
"abc\r"
"abc\r\n"
```

Stored:

```text
"abc"
```

---

## 23.11. NULL/empty push

Expected:

```text
false
ring count unchanged
dropped_count unchanged
```

---

## 23.12. Repeated init

Flow:

```text
init
push A
init
read
```

Expected:

```text
A remains
no second hook installation
ESP_OK
```

---

## 23.13. Clear

Flow:

```text
init
push A/B
clear
read
```

Expected:

```text
count == 0
hook still active
ESP_LOGI after clear is captured
```

---

## 23.14. Dropped counter contention test

Force writer contention:

```text
hold ring mutex from controlled test helper
call valid push
```

Expected:

```text
push == false
dropped_count += 1
```

Không dùng timing-only test nếu có thể tạo deterministic contention helper.

---

## 23.15. Direct multi-producer smoke test

4 FreeRTOS tasks:

```text
each -> log_buffer_push()
```

Acceptance:

- no crash;
- no deadlock;
- count <= capacity;
- valid NUL-terminated strings;
- ring readable.

---

## 23.16. Real ESP logger re-entrancy test

4 FreeRTOS tasks cùng gọi:

```c
ESP_LOGI(
    "log_buffer_test",
    "task=%u entry=%u",
    task_id,
    i);
```

Acceptance:

- no crash;
- no deadlock;
- console logging still works;
- recent ring remains valid;
- strings not corrupted;
- dropped count may be >0 and is acceptable under contention.

Đây là test bắt buộc vì production producer đi qua global ESP-IDF hook.

---

## 23.17. Web API compatibility test

Nếu web_server test harness thuận tiện, kiểm tra `/api/logs` response vẫn:

```json
[
  {
    "text": "...",
    "timestamp_ms": 123
  }
]
```

Không đổi thành object wrapper.

Nếu chưa có HTTP unit harness, ít nhất code review phải verify JSON key và root type không thay đổi.

---

# 24. Test isolation

Do hook là install-once:

Không dùng:

```text
init -> deinit -> init
```

giữa tests.

Recommended suite setup:

```text
test application startup
   |
   +--> log_buffer_init once
```

Mỗi test:

```c
log_buffer_clear();
```

Nếu dropped counter cần deterministic zero, cung cấp test-only reset helper:

```c
#ifdef CONFIG_LOG_BUFFER_TEST_HELPERS
void log_buffer_test_reset_metrics(void);
#endif
```

Không đưa helper này vào production API.

---

# 25. Migration plan theo commit

## Commit 1 — capacity-safe read API

Changes:

- remove `log_buffer_get_all()`;
- change recent capacity type to `size_t`;
- keep return type `int`;
- update callers;
- add bounded-copy tests.

Suggested message:

```text
refactor(log_buffer): make read API capacity-safe
```

Acceptance:

- no `log_buffer_get_all` references;
- Web `/api/logs` still builds;
- error semantics preserved.

---

## Commit 2 — explicit uptime field

Changes:

```text
long timestamp_ms
->
uint64_t uptime_ms
```

Update `/api/logs` internal mapping while keeping JSON key:

```text
timestamp_ms
```

Suggested message:

```text
refactor(log_buffer): use explicit uptime timestamp
```

---

## Commit 3 — idempotent install-once initialization

Changes:

- `log_buffer_init()` returns `esp_err_t`;
- first call initializes + installs hook;
- later calls return `ESP_OK`;
- re-init does not clear;
- add `log_buffer_clear()`;
- no production `deinit()`.

Update `main.c` with non-fatal failure policy.

Suggested message:

```text
refactor(log_buffer): define install-once lifecycle
```

---

## Commit 4 — non-blocking capture

Changes:

- writer try-lock;
- dropped atomic counter;
- bounded reader timeout;
- no 1000 ms wait on capture path.

Suggested message:

```text
refactor(log_buffer): make RAM capture contention-safe
```

---

## Commit 5 — concurrency and compatibility tests

Changes:

- multi-wrap;
- canary;
- truncation;
- clear/re-init;
- direct multi-producer;
- concurrent `ESP_LOGI`;
- dropped counter;
- `/api/logs` compatibility.

Suggested message:

```text
test(log_buffer): cover ring contention and web compatibility
```

---

# 26. Không làm trong v2

Không thêm:

- persistent flash logging;
- PSRAM log storage;
- lock-free MPMC ring;
- dynamic buffer resize;
- per-module buffers;
- file logging;
- syslog;
- MQTT log transport;
- structured logging framework;
- WebSocket streaming;
- wall-clock conversion;
- `/api/logs` schema redesign;
- public hook deinit.

---

# 27. Performance expectations

## Capture

Expected:

```text
local stack format
+ original vprintf call
+ one try-lock
+ one fixed-size copy
+ O(1) ring update
```

No heap allocation by `log_buffer`.

No ring-lock wait.

## Reader

```text
O(N)
N <= min(capacity, LOG_BUFFER_CAPACITY)
```

Lock only during snapshot copy.

---

# 28. Security rules

Do not intentionally log:

- Wi-Fi password;
- bearer token;
- API secret;
- private key;
- raw authentication credential.

`log_buffer` không redact nội dung.

Producer phải chịu trách nhiệm không phát secret vào ESP logs.

---

# 29. Review checklist

- [ ] `log_buffer_get_all()` đã bị xóa.
- [ ] `get_recent()` nhận explicit `size_t capacity`.
- [ ] `get_recent()` vẫn trả `int`.
- [ ] Empty read trả `0`.
- [ ] Invalid/unavailable read trả `-1`.
- [ ] Writer dùng zero-timeout ring lock.
- [ ] Capture path không wait mutex.
- [ ] Dropped counter chỉ tăng do valid writer contention.
- [ ] Dropped counter update không block.
- [ ] Hook callback dùng stack-local formatting buffer.
- [ ] Hook implementation re-entrant.
- [ ] Không `ESP_LOGx()` trong capture internals.
- [ ] `log_buffer_init()` trả `esp_err_t`.
- [ ] Init idempotent.
- [ ] Re-init không clear.
- [ ] Có `log_buffer_clear()`.
- [ ] Không có production `log_buffer_deinit()`.
- [ ] Hook tồn tại đến firmware reset.
- [ ] `timestamp_ms` internal đổi thành `uint64_t uptime_ms`.
- [ ] `/api/logs` vẫn trả root JSON array.
- [ ] `/api/logs` vẫn dùng key `timestamp_ms`.
- [ ] Init failure trong `app_main()` là non-fatal.
- [ ] No dynamic allocation on capture path.
- [ ] Multi-wrap tests pass.
- [ ] Bounded-output canary pass.
- [ ] Direct concurrent producer test pass.
- [ ] Concurrent `ESP_LOGI` re-entrancy test pass.
- [ ] Firmware build pass.
- [ ] Test project build pass.
- [ ] Unity tests pass trên ESP32-S3.

---

# 30. Acceptance criteria

## Correctness

1. Ring giữ đúng `LOG_BUFFER_CAPACITY` entry mới nhất sau overflow.
2. Recent entries theo chronological order.
3. Không output buffer overflow.
4. Empty read phân biệt được với error.
5. String luôn NUL-safe.
6. `uptime_ms` là monotonic boot uptime.
7. Re-init không mất logs.
8. Clear không uninstall hook.

## Concurrency

9. Capture path không chờ ring mutex.
10. Multi-producer không corrupt ring.
11. Concurrent ESP-IDF logging không crash/deadlock.
12. Count không bao giờ vượt capacity.
13. Dropped metric phản ánh contention.

## Lifecycle

14. Hook chỉ install một lần.
15. Hook sống đến firmware reset.
16. Không production deinit race.
17. Init failure không abort gateway startup.

## Integration

18. Console logs vẫn hoạt động.
19. `/api/logs` vẫn đọc recent entries.
20. `/api/logs` JSON contract không đổi.
21. Không giữ ring mutex khi serialize/send HTTP.

## Build/test

22. Root firmware build thành công.
23. Test firmware build thành công.
24. Unity tests pass trên ESP32-S3.

---

# 31. Definition of Done

`components/log_buffer` v2 được xem là hoàn tất khi đạt:

```text
bounded read contract
+ preserved error semantics
+ fixed static memory
+ zero-wait capture mutex
+ install-once hook lifecycle
+ idempotent non-fatal init
+ explicit uint64 uptime
+ dropped contention metric
+ HTTP compatibility
+ real concurrent ESP_LOG test coverage
```

Không cần architecture phức tạp hơn trước khi benchmark thực tế cho thấy vấn đề.

---

# 32. Kết luận

Refactor v2 tập trung đúng vào failure modes có ảnh hưởng hệ thống:

1. memory corruption từ unbounded read;
2. latency spike từ blocking capture;
3. global-hook lifecycle race;
4. loss of error semantics;
5. accidental breaking change của `/api/logs`;
6. insufficient re-entrancy verification.

Sau implementation theo tài liệu này, `log_buffer` vẫn là một component nhỏ nhưng có contract rõ ràng, memory usage xác định và behavior phù hợp với ESP32 BLE Gateway chạy đồng thời BLE, Wi-Fi, HTTP và MCP.
