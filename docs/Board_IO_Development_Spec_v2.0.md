# Tài liệu phát triển Component `board_io` — ESP32 BLE Gateway

**Phiên bản:** 2.1  
**Ngày cập nhật:** 26/08/2026  
**Target:** ESP32-S3  
**Framework:** ESP-IDF 5.4.4 native  
**Repository:** `hailp-vn38/esp-ble-gateway`  
**Trạng thái:** Development Specification — Ready for Implementation  
**Thay thế:** `Board_IO_Development_Spec.md` v1.0

---

# 0. Changelog v2.0

Phiên bản 2.0 xử lý các mâu thuẫn và khoảng trống kiến trúc được phát hiện khi review v1.0.

## 0.0 Changelog v2.1 — prod-hardening

Các thay đổi sau vòng review sẵn sàng triển khai production:

1. Blocklist GPIO ESP32-S3 đầy đủ theo loại module: 22–25 chip-invalid, 26–32 flash, 33–37 octal PSRAM (mục 38, test BO-PIN-011+).
2. Overlay priority sửa thành `FACTORY_ARMED > RESTART_ARMED > ERROR > ...`: giữ nút phải luôn có feedback kể cả khi base là ERROR (mục 26).
3. Khóa riêng return value cho display runtime-disabled, khác compile-time disabled (mục 31, BO-API-012).
4. LED pattern output dùng relative delta + sentinel `NO_TRANSITION`, hết mơ hồ uint32/uint64 (mục 28).
5. Worker wait dùng `portMAX_DELAY` sentinel thay vì `pdMS_TO_TICKS(UINT32_MAX)` (mục 14).
6. Validation polarity ↔ pull mode chống phantom press (mục 38, BO-PIN-014).
7. `board_io_register_event_handler()` trước init bị reject (mục 8.4, BO-API-005).
8. Double-buffer display là yêu cầu tường minh (mục 32).
9. CMake thêm `log` vào `PRIV_REQUIRES` (mục 47).
10. Ghi chú ràng buộc dispatcher registry freeze cho remote command (mục 46).

## 0.1 Breaking changes (v2.0)

1. `board_io` không còn được định nghĩa là single-shot cho toàn bộ boot.
   - `init()` lần hai khi đang chạy vẫn bị từ chối.
   - Sau `deinit()` được phép `init()` lại.
   - Lifecycle chính thức:
     `UNINITIALIZED -> INITIALIZING -> RUNNING -> STOPPING -> UNINITIALIZED`.

2. `board_io_deinit()` đổi thành:
   ```c
   esp_err_t board_io_deinit(void);
   ```
   để có thể báo lỗi lifecycle và self-deinit.

3. Public button event bỏ `BUTTON_PRESSED` và `BUTTON_RELEASED`.
   Public API chỉ emit semantic action:
   - `BUTTON_SHORT_PRESS`
   - `RESTART_REQUEST`
   - `FACTORY_RESET_REQUEST`

4. Button FSM không còn mâu thuẫn "một output nhưng cần hai event trên release".

5. Thread-safety contract được định nghĩa đầy đủ.

6. Deinit có stop handshake; không free resource trước khi worker task xác nhận đã dừng.

7. Runtime status synchronization được bổ sung ở application layer.
   `app_main()` không còn là nguồn duy nhất cập nhật LED state.

8. Display base abstraction không phụ thuộc I2C.
   Bus-specific Kconfig được đặt bên trong backend cụ thể.

9. Giới hạn I2C backend cho ESP32-S3 ở mức tối đa 400 kHz trong specification này.

10. GPIO ISR contract được thống nhất:
    - V1 dùng ISR service không IRAM.
    - ISR callback không gắn `IRAM_ATTR`.
    - Chỉ chuyển sang IRAM ISR nếu có yêu cầu latency đặc biệt và toàn bộ callback path được review IRAM-safe.

11. Pull configuration chuyển từ hai boolean sang Kconfig `choice`.

12. LED overlay retrigger và priority được khóa rõ.

13. Display disabled semantics được khóa:
    - `board_io_display_update()` trả `ESP_ERR_NOT_SUPPORTED`.
    - `board_io_display_set_enabled()` trả `ESP_ERR_NOT_SUPPORTED`.

14. Test heap init/deinit được sửa để không giả định global GPIO ISR service phải được giải phóng.

---

# 1. Bối cảnh

ESP32 BLE Gateway hiện được tổ chức thành các component riêng cho BLE, command dispatch/execution, device persistence, Wi-Fi provisioning, gateway status, log buffer, Web UI và MCP.

Gateway cần thêm phần cứng cục bộ:

- reset/factory-reset button,
- status LED,
- local display,
- có thể mở rộng buzzer hoặc board-specific indicators sau này.

Nếu application hoặc subsystem trực tiếp thao tác GPIO:

```c
gpio_set_level(GPIO_NUM_x, ...);
```

thì pin mapping và hardware policy sẽ bị rải rác khắp codebase.

Component `board_io` được tạo để giải quyết việc này.

---

# 2. Architectural decision

Tên component:

```text
components/board_io
```

`board_io` là abstraction cho **I/O vật lý của board gateway**, không phải generic GPIO service.

Application nói bằng semantic intent:

```text
Gateway đang provisioning
Gateway ready
Pulse activity
Identify gateway
Hiển thị frame này
```

`board_io` quyết định:

```text
GPIO level
button debounce
button hold classification
LED pattern timing
display backend
```

Theo chiều ngược lại, `board_io` chỉ phát semantic event:

```text
short press
restart requested
factory reset requested
```

Application quyết định tác động hệ thống.

---

# 3. Mục tiêu

## 3.1 Button

Component phải:

- cấu hình input GPIO;
- hỗ trợ active-low/active-high;
- hỗ trợ pull mode;
- sử dụng GPIO interrupt;
- debounce;
- đo hold duration;
- phân loại:
  - short press,
  - restart request,
  - factory reset request;
- không thực hiện restart trong ISR;
- không xóa NVS;
- không phụ thuộc Wi-Fi, Device Store hoặc BLE.

## 3.2 Status LED

Component phải:

- thể hiện lifecycle state;
- non-blocking;
- hỗ trợ base pattern;
- hỗ trợ transient signal;
- hỗ trợ visual feedback khi giữ reset button;
- không sử dụng delay để chạy toàn bộ pattern;
- không cho subsystem khác điều khiển board LED GPIO trực tiếp.

## 3.3 Display

Component phải:

- optional;
- public API độc lập controller/bus;
- copy input frame;
- render async trong worker task;
- coalesce rapid update;
- giới hạn refresh rate;
- không làm gateway chết nếu display optional bị lỗi.

## 3.4 Pin ownership

Component phải:

- có một nơi duy nhất quyết định board-level GPIO;
- validate conflict;
- không hard-code pin chưa xác nhận schematic;
- feature nào disable thì pin của feature đó không tham gia validation.

## 3.5 Testability

Core logic phải test được mà không cần:

- button thật,
- LED thật,
- display thật,
- Wi-Fi,
- BLE,
- Web server.

Hardware test được tách riêng thành HIL/manual tests.

---

# 4. Non-goals

`board_io` V2 không có trách nhiệm:

- BLE management;
- Wi-Fi management;
- NVS management;
- factory-reset persistence workflow;
- BLE bond purge;
- command dispatch;
- HTTP/MCP;
- generic GPIO read/write API;
- remote raw GPIO control;
- shared I2C/SPI bus manager cho toàn hệ thống;
- graphical UI framework đầy đủ.

Nếu local peripheral tăng nhiều, bus ownership nên được tách thành component khác thay vì biến `board_io` thành monolith.

---

# 5. Dependency rules

Được phép:

```text
main/application -> board_io
```

Có thể:

```text
command integration -> board_io
```

nhưng command registration thuộc application/dispatcher layer, không nằm trong `board_io`.

Không được phép:

```text
board_io -> wifi_provisioning
board_io -> ble_central
board_io -> command_dispatcher
board_io -> command_executor
board_io -> device_store
board_io -> web_server
board_io -> mcp_endpoint
board_io -> gateway_status
```

`board_io` chỉ phụ thuộc ESP-IDF driver/FreeRTOS primitives cần thiết.

---

# 6. Cấu trúc thư mục

```text
components/
└── board_io/
    ├── CMakeLists.txt
    ├── Kconfig
    ├── README.md
    │
    ├── include/
    │   └── board_io.h
    │
    ├── board_io.c
    ├── board_io_internal.h
    │
    ├── board_pin_map.c
    ├── board_pin_map.h
    │
    ├── board_button.c
    ├── board_button.h
    ├── board_button_fsm.c
    ├── board_button_fsm.h
    │
    ├── board_led.c
    ├── board_led.h
    ├── board_led_pattern.c
    ├── board_led_pattern.h
    │
    ├── board_display.c
    ├── board_display.h
    │
    ├── display_backends/
    │   ├── board_display_backend.h
    │   ├── board_display_none.c
    │   └── ...
    │
    └── test/
        ├── CMakeLists.txt
        ├── test_board_io.c
        ├── test_board_pin_map.c
        ├── test_board_button_fsm.c
        ├── test_board_button_debounce.c
        ├── test_board_led_pattern.c
        └── test_board_display.c
```

Public header duy nhất:

```text
include/board_io.h
```

Không đưa internal FSM/backend/test hooks vào public include.

---

# 7. Public API v2

Đề xuất:

```c
#ifndef BOARD_IO_H
#define BOARD_IO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOARD_IO_DISPLAY_LINES       4
#define BOARD_IO_DISPLAY_LINE_LEN   32

typedef enum {
    BOARD_STATUS_BOOTING = 0,
    BOARD_STATUS_PROVISIONING,
    BOARD_STATUS_WIFI_CONNECTING,
    BOARD_STATUS_READY,
    BOARD_STATUS_DEGRADED,
    BOARD_STATUS_ERROR,
    BOARD_STATUS_COUNT,
} board_status_t;

typedef enum {
    BOARD_IO_EVENT_BUTTON_SHORT_PRESS = 0,
    BOARD_IO_EVENT_RESTART_REQUEST,
    BOARD_IO_EVENT_FACTORY_RESET_REQUEST,
    BOARD_IO_EVENT_COUNT,
} board_io_event_t;

typedef enum {
    BOARD_SIGNAL_ACTIVITY = 0,
    BOARD_SIGNAL_IDENTIFY,
    BOARD_SIGNAL_COUNT,
} board_signal_t;

typedef struct {
    char line[BOARD_IO_DISPLAY_LINES][BOARD_IO_DISPLAY_LINE_LEN];
} board_display_frame_t;

typedef void (*board_io_event_handler_t)(
    board_io_event_t event,
    void *context
);

esp_err_t board_io_init(void);
esp_err_t board_io_deinit(void);

bool board_io_is_initialized(void);

esp_err_t board_io_register_event_handler(
    board_io_event_handler_t handler,
    void *context
);

esp_err_t board_io_set_status(board_status_t status);

esp_err_t board_io_signal(board_signal_t signal);

esp_err_t board_io_display_update(
    const board_display_frame_t *frame
);

esp_err_t board_io_display_set_enabled(bool enabled);

#ifdef __cplusplus
}
#endif

#endif
```

---

# 8. API contract

## 8.1 `board_io_init()`

State requirement:

```text
UNINITIALIZED
```

Behavior:

- validate config;
- validate pin map;
- initialize enabled backends;
- create synchronization primitives;
- create worker task;
- configure LED safe state;
- configure button;
- add GPIO ISR handler last;
- transition to `RUNNING`.

Return:

```text
ESP_OK
ESP_ERR_INVALID_STATE
ESP_ERR_INVALID_ARG
ESP_ERR_NO_MEM
driver-specific esp_err_t khi critical resource fail
```

Nếu display optional fail:

```text
CONFIG_BOARD_IO_DISPLAY_REQUIRED=n
```

thì log warning, disable display runtime và init vẫn có thể trả `ESP_OK`.

## 8.2 `board_io_deinit()`

Cho phép:

```text
RUNNING -> STOPPING -> UNINITIALIZED
```

Nếu chưa init:

```text
ESP_OK
```

Nếu đang `INITIALIZING` hoặc `STOPPING`:

```text
ESP_ERR_INVALID_STATE
```

Nếu gọi từ chính `board_io_task`:

```text
ESP_ERR_INVALID_STATE
```

để tránh self-wait/deadlock.

Sau deinit thành công, có thể gọi `board_io_init()` lại.

## 8.3 `board_io_is_initialized()`

True chỉ ở:

```text
RUNNING
```

False ở các state còn lại.

## 8.4 `board_io_register_event_handler()`

V2 dùng một handler duy nhất.

Rules:

- chưa `RUNNING` (trước init hoặc đang stop) -> `ESP_ERR_INVALID_STATE` (v2.1).
- `handler != NULL` khi chưa có handler -> `ESP_OK`.
- register handler thứ hai -> `ESP_ERR_INVALID_STATE`.
- `handler == NULL` -> unregister handler hiện tại và clear context.
- callback chạy trong `board_io_task`.
- callback không chạy trong ISR.
- callback được gọi **ngoài mọi internal lock**.
- callback không được gọi `board_io_deinit()` trực tiếp.

Nếu callback cần workflow nặng, callback chỉ post sang application queue.

## 8.5 `board_io_set_status()`

- thread-safe;
- không ISR-safe;
- `status >= BOARD_STATUS_COUNT` -> `ESP_ERR_INVALID_ARG`;
- trước init/đang stop -> `ESP_ERR_INVALID_STATE`;
- chỉ update desired base state và notify task;
- không block theo pattern duration.

## 8.6 `board_io_signal()`

- thread-safe;
- không ISR-safe;
- invalid enum -> `ESP_ERR_INVALID_ARG`;
- activity/identify semantics được định nghĩa ở mục LED.

## 8.7 Display API

Nếu display feature compile-time disabled hoặc không có runtime backend:

```text
ESP_ERR_NOT_SUPPORTED
```

Nếu component chưa RUNNING:

```text
ESP_ERR_INVALID_STATE
```

Nếu frame NULL:

```text
ESP_ERR_INVALID_ARG
```

---

# 9. Thread-safety contract

Tất cả public API, trừ query đơn giản được ghi rõ, phải an toàn khi gọi từ nhiều FreeRTOS task.

Public API:

```text
NOT ISR SAFE
```

Không gọi từ:

```text
GPIO ISR
timer ISR
NimBLE ISR-like low-level callback nếu callback có ISR restriction
```

Internal rules:

1. Shared mutable state phải được bảo vệ.
2. Không giữ mutex trong khi:
   - gọi application callback;
   - render display;
   - gọi GPIO driver chậm;
   - chờ worker stop.
3. ISR không dùng mutex.
4. ISR chỉ notify worker.
5. `deinit()` chuyển state sang STOPPING trước khi teardown để ngăn API mới.

---

# 10. Lifecycle state machine

Internal:

```c
typedef enum {
    BOARD_IO_STATE_UNINITIALIZED = 0,
    BOARD_IO_STATE_INITIALIZING,
    BOARD_IO_STATE_RUNNING,
    BOARD_IO_STATE_STOPPING,
} board_io_lifecycle_t;
```

State transition:

```text
UNINITIALIZED
      |
      | init
      v
 INITIALIZING
      |
      +---- fail -----------------> UNINITIALIZED
      |
      v
   RUNNING
      |
      | deinit
      v
   STOPPING
      |
      +---- cleanup complete -----> UNINITIALIZED
```

Invalid:

```text
RUNNING -> init
INITIALIZING -> init
STOPPING -> init
INITIALIZING -> deinit
STOPPING -> deinit
```

---

# 11. Deinit stop handshake

Đây là bắt buộc trong v2.

Sequence:

```text
caller
  |
  | lock
  | RUNNING -> STOPPING
  | unlock
  |
  +--> disable button GPIO interrupt
  +--> gpio_isr_handler_remove()
  |
  +--> notify BOARD_NOTIFY_STOP
  |
  +--> wait stopped semaphore/event
                |
                v
          board_io_task exits
                |
                v
          signal stopped
  |
  +--> deinit display
  +--> set LED safe/off
  +--> delete mutex/semaphore
  +--> clear handler/context
  +--> clear task handle
  +--> state UNINITIALIZED
```

Resource không được free trước khi worker task đã dừng.

Nếu worker không stop trong bounded timeout:

- log error;
- return timeout/error;
- không free resource mà worker còn có thể truy cập.

Không được "free anyway".

---

# 12. Concurrency model

V2 vẫn dùng một worker task.

```text
                        public API
                           |
          +----------------+----------------+
          |                |                |
       status            signal          display
          |                |                |
          +----------------+----------------+
                           |
                           v
                   task notification
                           |
                           v
                    board_io_task
                 /       |        \
                /        |         \
           button       LED       display
```

Không tạo:

```text
button_task
led_task
display_task
```

trong V2.

---

# 13. Task notification model

Bitmask:

```c
#define BOARD_NOTIFY_BUTTON_EDGE     (1UL << 0)
#define BOARD_NOTIFY_STATUS_CHANGED  (1UL << 1)
#define BOARD_NOTIFY_ACTIVITY        (1UL << 2)
#define BOARD_NOTIFY_IDENTIFY        (1UL << 3)
#define BOARD_NOTIFY_DISPLAY         (1UL << 4)
#define BOARD_NOTIFY_STOP            (1UL << 5)
```

Lý do không dùng generic queue:

- button bounce có thể coalesce;
- status chỉ cần latest value;
- display chỉ cần latest frame;
- repeated activity signal không cần một object riêng cho từng call;
- tránh queue overflow khi button bounce/event storm.

---

# 14. Deadline-driven worker loop

Worker không poll liên tục.

Deadline có thể gồm:

```text
button debounce deadline
restart threshold deadline
factory threshold deadline
LED next transition
activity pulse deadline
identify deadline
display next allowed render time
```

Pseudo loop:

```c
#define BOARD_IO_WAIT_NONE UINT32_MAX

for (;;) {
    uint32_t timeout_ms = compute_next_deadline_ms();

    uint32_t bits = 0;
    xTaskNotifyWait(
        0,
        UINT32_MAX,
        &bits,
        (timeout_ms == BOARD_IO_WAIT_NONE)
            ? portMAX_DELAY
            : pdMS_TO_TICKS(timeout_ms)
    );

    if (bits & BOARD_NOTIFY_STOP) break;

    uint64_t now_ms = board_time_now_ms();

    handle_button(bits, now_ms);
    handle_led(bits, now_ms);
    handle_display(bits, now_ms);
}
```

Không dùng busy loop.

`BOARD_IO_WAIT_NONE` là sentinel "không có deadline". Không dùng
`pdMS_TO_TICKS(UINT32_MAX)` làm timeout vô hạn vì tick math 32-bit có thể
wrap. Deadline thực tế đều nhỏ (< vài giây) nên an toàn với tick 32-bit.

---

# 15. Time source

Production time source:

```c
esp_timer_get_time()
```

convert:

```c
uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
```

Pure FSM/pattern engine không được gọi trực tiếp `esp_timer_get_time()`.

Thay vào đó caller truyền `now_ms`.

Lợi ích:

- deterministic unit tests;
- dễ test exact boundary;
- không sleep trong test.

---

# 16. Button public semantics

Public event chỉ còn:

```text
BUTTON_SHORT_PRESS
RESTART_REQUEST
FACTORY_RESET_REQUEST
```

Không public:

```text
BUTTON_PRESSED
BUTTON_RELEASED
```

Pressed/released là physical state nội bộ.

Lý do:

- application quan tâm action;
- tránh mâu thuẫn multi-event;
- API ổn định hơn;
- physical bounce/details không leak ra ngoài abstraction.

---

# 17. Button internal FSM

Internal states:

```c
typedef enum {
    BOARD_BUTTON_RELEASED = 0,
    BOARD_BUTTON_PRESSED,
    BOARD_BUTTON_RESTART_ARMED,
    BOARD_BUTTON_FACTORY_ARMED,
} board_button_state_t;
```

Transition:

```text
RELEASED
   |
   | stable press
   v
PRESSED
   |
   | hold >= restart_ms
   v
RESTART_ARMED
   |
   | hold >= factory_ms
   v
FACTORY_ARMED
```

Release:

```text
PRESSED         -> SHORT_PRESS
RESTART_ARMED   -> RESTART_REQUEST
FACTORY_ARMED   -> FACTORY_RESET_REQUEST
```

Mỗi physical press cycle phát tối đa **một semantic event**.

---

# 18. Button thresholds

Default đề xuất:

```text
debounce_ms = 40
restart_ms = 2000
factory_reset_ms = 8000
```

Boundary:

```text
duration < restart_ms
    => SHORT_PRESS

restart_ms <= duration < factory_reset_ms
    => RESTART_REQUEST

duration >= factory_reset_ms
    => FACTORY_RESET_REQUEST
```

Action chỉ emit khi release.

Không emit restart ngay khi chạm 2 giây vì user có thể đang tiếp tục giữ tới factory reset.

---

# 19. Button armed visual state

Khi giữ nút:

```text
< restart threshold
    normal base LED

>= restart threshold
    internal RESTART_ARMED overlay

>= factory threshold
    internal FACTORY_ARMED overlay
```

Không emit public semantic event cho tới release.

Armed overlay hiển thị kể cả khi base state là ERROR (xem priority mục 26).

Điều này vừa cho user feedback vừa tránh thực thi action sớm.

---

# 20. Debounce

Default:

```text
40 ms quiet window
```

Algorithm:

1. ISR notify button edge.
2. Worker nhận edge.
3. Set:
   ```text
   debounce_deadline = now + debounce_ms
   ```
4. Edge mới trước deadline:
   - worker wake;
   - reset deadline.
5. Khi deadline đến:
   - sample `gpio_get_level()`.
6. Nếu stable logical state khác state trước:
   - commit physical state;
   - feed FSM.

Không block task bằng:

```c
vTaskDelay(debounce_ms);
```

vì worker còn phụ trách LED/display.

---

# 21. GPIO ISR contract

V2 chọn implementation đơn giản:

```text
gpio_install_isr_service(0)
```

Không dùng:

```text
ESP_INTR_FLAG_IRAM
```

trong V2 mặc định.

ISR callback:

```c
static void button_gpio_isr(void *arg)
{
    BaseType_t task_woken = pdFALSE;

    TaskHandle_t task = s_board_task;
    if (task != NULL) {
        xTaskNotifyFromISR(
            task,
            BOARD_NOTIFY_BUTTON_EDGE,
            eSetBits,
            &task_woken
        );
    }

    portYIELD_FROM_ISR(task_woken);
}
```

Không gắn `IRAM_ATTR` trong contract mặc định.

Nếu tương lai chuyển sang `ESP_INTR_FLAG_IRAM`, phải review lại:

- callback code;
- accessed data;
- called FreeRTOS ISR API;
- memory placement.

---

# 22. GPIO ISR service ownership

`gpio_install_isr_service(0)`:

- `ESP_OK`: service mới được cài.
- `ESP_ERR_INVALID_STATE`: service đã tồn tại; tiếp tục.

`board_io` chỉ ownership per-pin handler.

Deinit:

```c
gpio_intr_disable(button_gpio);
gpio_isr_handler_remove(button_gpio);
```

Không gọi:

```c
gpio_uninstall_isr_service();
```

Vì service global có thể được component khác dùng.

Hệ quả cho test memory:

- global ISR service có thể giữ allocation process-lifetime;
- heap leak test phải warm-up trước khi lấy baseline.

---

# 23. Factory reset ownership

`board_io` chỉ emit:

```text
BOARD_IO_EVENT_FACTORY_RESET_REQUEST
```

Không thực hiện:

```text
wifi_prov_clear_credentials()
device_store_delete()
BLE bond erase
NVS erase
esp_restart()
```

Factory reset là application-level workflow.

Current repository đã có API clear Wi-Fi credentials, nhưng chưa có một API duy nhất thể hiện "factory reset toàn gateway".

Vì vậy spec `board_io` không được tự suy diễn factory reset nghĩa là xóa những namespace/bond nào.

---

# 24. Application event handler

Ví dụ:

```c
static void on_board_io_event(board_io_event_t event, void *context)
{
    (void)context;

    switch (event) {
    case BOARD_IO_EVENT_BUTTON_SHORT_PRESS:
        application_post(APP_EVENT_BUTTON_SHORT_PRESS);
        break;

    case BOARD_IO_EVENT_RESTART_REQUEST:
        application_post(APP_EVENT_RESTART_REQUEST);
        break;

    case BOARD_IO_EVENT_FACTORY_RESET_REQUEST:
        application_post(APP_EVENT_FACTORY_RESET_REQUEST);
        break;

    default:
        break;
    }
}
```

Không thực hiện workflow dài trực tiếp trong callback.

Recommended:

```text
board callback
    |
    v
application queue
    |
    v
application control task
```

---

# 25. LED base states

Enum `board_status_t` có định nghĩa canonical duy nhất ở mục 7 — không
duplicate tại đây để tránh drift khi sửa enum.

Pattern mặc định cho single-color LED:

| State | Pattern |
|---|---|
| BOOTING | 100 ms ON / 100 ms OFF |
| PROVISIONING | 500 ms ON / 500 ms OFF |
| WIFI_CONNECTING | 125 ms ON / 125 ms OFF |
| READY | steady ON |
| DEGRADED | 150 ms ON / 850 ms OFF |
| ERROR | 3 x 150 ms blink + 1000 ms pause |

Pattern timing constants ở internal code hoặc Kconfig tùy implementation; V2 không cần expose mọi timing ra menuconfig.

---

# 26. LED overlays

Internal overlay:

```text
NONE
ACTIVITY
IDENTIFY
RESTART_ARMED
FACTORY_ARMED
```

Priority v2.1:

```text
FACTORY_ARMED
    >
RESTART_ARMED
    >
ERROR base state
    >
IDENTIFY
    >
ACTIVITY
    >
base state
```

Lý do:

- factory reset destructive feedback luôn phải rõ;
- armed overlay là phản hồi cho hành động vật lý cục bộ của user nên phải
  thắng mọi trạng thái nền, kể cả ERROR — user giữ nút trên thiết bị đang
  ERROR vẫn thấy feedback ở mốc restart thay vì phải giữ mù tới factory
  threshold;
- ERROR vẫn cao hơn IDENTIFY/ACTIVITY nên tín hiệu remote không che được lỗi;
- activity có priority thấp.

---

# 27. LED signal retrigger semantics

## 27.1 Activity

Default pulse:

```text
80 ms
```

Nếu signal mới đến trong khi pulse đang active:

```text
activity_deadline = now + pulse_ms
```

Tức là **extend/restart pulse từ latest signal**, không queue từng pulse.

## 27.2 Identify

Default duration:

```text
5000 ms
```

Nếu identify được trigger lại:

```text
identify_deadline = now + identify_duration_ms
pattern_epoch = now
```

Tức là restart identify duration từ đầu.

## 27.3 Base status change trong overlay

Base status vẫn được cập nhật.

Khi overlay kết thúc, LED trở về **latest base state**, không phải state tại lúc overlay bắt đầu.

---

# 28. LED pure pattern engine

Input:

```c
typedef struct {
    board_status_t base_status;
    board_led_overlay_t overlay;
    uint64_t pattern_epoch_ms;
    uint64_t now_ms;
} board_led_pattern_input_t;
```

Output:

```c
#define BOARD_LED_PATTERN_NO_TRANSITION UINT32_MAX

typedef struct {
    bool logical_on;

    /* Relative delta (ms) từ now_ms tới transition kế tiếp.
       BOARD_LED_PATTERN_NO_TRANSITION nếu steady, không có transition. */
    uint32_t rel_next_transition_ms;
} board_led_pattern_output_t;
```

Caller convert sang absolute deadline: `deadline = now_ms +
rel_next_transition_ms` (dùng saturating add để tránh wrap).

Pure engine không gọi GPIO.

Hardware adapter convert:

```text
logical_on + active_low -> physical GPIO level
```

---

# 29. Display abstraction

Base `board_io` không biết display dùng:

```text
I2C
SPI
8080
RGB
USB
```

Public model:

```c
typedef struct {
    char line[4][32];
} board_display_frame_t;
```

Đây là V2 text-level contract.

Không expose controller-specific drawing API.

---

# 30. Display backend interface

Internal:

```c
typedef struct {
    esp_err_t (*init)(void);
    void (*deinit)(void);
    esp_err_t (*set_enabled)(bool enabled);
    esp_err_t (*render)(const board_display_frame_t *frame);
} board_display_backend_t;
```

Backend ownership:

```text
board_display.c
      |
      +--> none backend
      +--> future SSD1306 I2C backend
      +--> future ST7789 SPI backend
```

Base component không có SDA/SCL config nếu backend không dùng I2C.

---

# 31. Display disabled contract

Compile-time display disabled:

```text
CONFIG_BOARD_IO_DISPLAY_ENABLE=n
```

Result:

```c
board_io_display_update(...)
    -> ESP_ERR_NOT_SUPPORTED

board_io_display_set_enabled(...)
    -> ESP_ERR_NOT_SUPPORTED
```

Runtime disabled (`CONFIG_BOARD_IO_DISPLAY_ENABLE=y` nhưng đã gọi
`set_enabled(false)`):

```c
board_io_display_update(frame)
    -> ESP_OK; frame trở thành latest pending, render ngay khi re-enabled

board_io_display_set_enabled(false)
    -> ESP_OK (idempotent)

board_io_display_set_enabled(true) khi backend không available
    -> ESP_ERR_NOT_SUPPORTED
```

Không silent `ESP_OK` ở mức capability.

Điều này giúp caller phân biệt "capability không tồn tại" với "đang bị tắt
tạm thời".

---

# 32. Display update semantics

`board_io_display_update(frame)`:

1. validate pointer;
2. validate lifecycle;
3. copy frame vào internal pending buffer;
4. force NUL termination;
5. mark dirty;
6. notify worker;
7. return.

Buffer model bắt buộc (v2.1):

```text
hai frame buffer tĩnh: pending + render_snapshot
update():  copy caller frame -> pending      (dưới mutex, ngắn)
worker:    pending -> render_snapshot        (dưới mutex)
           render(render_snapshot)           (ngoài lock)
```

Caller có thể reuse/modify input ngay sau khi API return.

Không giữ pointer của caller.

Render không được chạy khi đang giữ internal lock (BO-DSP-011).

---

# 33. Display coalescing

Nếu:

```text
frame A
frame B
frame C
```

đến trước refresh window tiếp theo:

```text
render C
```

Không đảm bảo A/B được render.

Đây là deliberate latest-state-wins semantics.

---

# 34. Display refresh

Base setting:

```text
CONFIG_BOARD_IO_DISPLAY_MAX_REFRESH_HZ
```

Default:

```text
5 Hz
```

Worker chỉ render khi:

```text
dirty == true
AND
now >= next_render_allowed
AND
display_enabled == true
AND
backend_available == true
```

---

# 35. Display backend Kconfig

Base:

```kconfig
config BOARD_IO_DISPLAY_ENABLE
    bool "Enable local display"
    default n

if BOARD_IO_DISPLAY_ENABLE

choice BOARD_IO_DISPLAY_BACKEND
    prompt "Display backend"
    default BOARD_IO_DISPLAY_BACKEND_NONE

config BOARD_IO_DISPLAY_BACKEND_NONE
    bool "No hardware backend"

# Add only when implementation exists.
# config BOARD_IO_DISPLAY_BACKEND_SSD1306_I2C
#     bool "SSD1306 over I2C"

endchoice

config BOARD_IO_DISPLAY_REQUIRED
    bool "Display failure is fatal to board_io init"
    default n

config BOARD_IO_DISPLAY_MAX_REFRESH_HZ
    int "Maximum display refresh rate"
    range 1 30
    default 5

endif
```

Backend-specific Kconfig chỉ được thêm cùng concrete backend.

Ví dụ SSD1306 I2C:

```kconfig
if BOARD_IO_DISPLAY_BACKEND_SSD1306_I2C

config BOARD_IO_DISPLAY_SSD1306_SDA_GPIO
    int "SSD1306 SDA GPIO"

config BOARD_IO_DISPLAY_SSD1306_SCL_GPIO
    int "SSD1306 SCL GPIO"

config BOARD_IO_DISPLAY_SSD1306_I2C_FREQ_HZ
    int "SSD1306 I2C frequency"
    range 10000 400000
    default 400000

config BOARD_IO_DISPLAY_SSD1306_ADDRESS
    hex "SSD1306 I2C address"
    range 0x03 0x77
    default 0x3C

endif
```

Không dùng base `DISPLAY_SDA_GPIO`/`DISPLAY_SCL_GPIO`.

---

# 36. Kconfig — button

```kconfig
menu "Board I/O"

config BOARD_IO_BUTTON_ENABLE
    bool "Enable board button"
    default n

if BOARD_IO_BUTTON_ENABLE

config BOARD_IO_BUTTON_GPIO
    int "Button GPIO"
    range 0 48

config BOARD_IO_BUTTON_ACTIVE_LOW
    bool "Button is active-low"
    default y

choice BOARD_IO_BUTTON_PULL_MODE
    prompt "Button internal pull mode"
    default BOARD_IO_BUTTON_PULL_UP

config BOARD_IO_BUTTON_PULL_NONE
    bool "No internal pull"

config BOARD_IO_BUTTON_PULL_UP
    bool "Internal pull-up"

config BOARD_IO_BUTTON_PULL_DOWN
    bool "Internal pull-down"

endchoice

config BOARD_IO_BUTTON_DEBOUNCE_MS
    int "Button debounce time (ms)"
    range 5 500
    default 40

config BOARD_IO_BUTTON_RESTART_MS
    int "Restart hold threshold (ms)"
    range 500 30000
    default 2000

config BOARD_IO_BUTTON_FACTORY_RESET_MS
    int "Factory reset hold threshold (ms)"
    range 1000 60000
    default 8000

endif
```

Runtime validation vẫn phải kiểm tra:

```text
factory_reset_ms > restart_ms
debounce_ms < restart_ms
```

---

# 37. Kconfig — LED/task

```kconfig
config BOARD_IO_LED_ENABLE
    bool "Enable status LED"
    default n

if BOARD_IO_LED_ENABLE

config BOARD_IO_LED_GPIO
    int "Status LED GPIO"
    range 0 48

config BOARD_IO_LED_ACTIVE_LOW
    bool "Status LED is active-low"
    default n

endif

config BOARD_IO_TASK_STACK_SIZE
    int "Board I/O task stack size"
    range 2048 8192
    default 3072

config BOARD_IO_TASK_PRIORITY
    int "Board I/O task priority"
    range 1 10
    default 3
```

---

# 38. Pin validation

Validation chỉ xét enabled feature.

Phải kiểm tra:

- GPIO hợp lệ;
- output role dùng output-capable GPIO;
- button != LED;
- backend-specific pin uniqueness;
- SDA != SCL nếu I2C backend;
- `factory_reset_ms > restart_ms`;
- `debounce_ms < restart_ms`;
- polarity ↔ pull coherence (bảng bên dưới).

**Blocklist GPIO ESP32-S3 bắt buộc v2.1.** `GPIO_IS_VALID_GPIO` chỉ biết
chip-level validity, không biết sơ đồ module. Production validation phải
reject:

```text
22–25      chip-invalid              -> invalid
26–32      embedded flash            -> invalid
33–37      octal PSRAM               -> invalid khi CONFIG_SPIRAM_MODE_OCT=y,
                                        warning khi quad
0,3,45,46  strapping                 -> runtime OK, bắt buộc review mục 39
43,44      UART0 console mặc định    -> warning
```

Kconfig range `0..48` không mã hóa blocklist này — nó phải là code + test,
không phải comment.

**Polarity ↔ pull coherence.** Contradiction trực tiếp gây floating input /
phantom press:

```text
BUTTON_ACTIVE_LOW=y + PULL_DOWN -> ESP_ERR_INVALID_ARG
BUTTON_ACTIVE_LOW=n + PULL_UP   -> ESP_ERR_INVALID_ARG
BUTTON_ACTIVE_LOW=* + PULL_NONE -> valid, log warning (cần external resistor)
```

Pin 22–25 phải có dedicated test invalid trên ESP32-S3.

---

# 39. Hardware pin review ngoài software

Runtime validation không chứng minh GPIO an toàn trên PCB.

Production pin assignment phải review:

- strapping pins;
- native USB pins;
- flash/PSRAM pins;
- JTAG/debug;
- boot-time pull;
- external resistor;
- power-on default level;
- peripheral-specific electrical requirement.

Không merge production enable config khi chưa xác nhận schematic.

---

# 40. Application startup integration

`board_io_init()` phải chạy sớm.

Đề xuất:

```c
void app_main(void)
{
    log_buffer_init();

    esp_err_t io_rc = board_io_init();
    if (io_rc == ESP_OK) {
        board_io_register_event_handler(on_board_io_event, NULL);
        board_io_set_status(BOARD_STATUS_BOOTING);
    } else {
        ESP_LOGE(TAG, "Board I/O init failed: %s",
                 esp_err_to_name(io_rc));
    }

    esp_err_t ret = nvs_flash_init();
    ...
}
```

Không đặt `board_io_init()` sau BLE/Web.

---

# 41. Wi-Fi startup status mapping

Quan trọng: `wifi_prov_init()` hiện thực hiện boot connect/provisioning nội bộ trước khi return.

Do đó application không thể làm:

```text
wifi_prov_init()
then set WIFI_CONNECTING
```

và mong đó là boot-connecting indication chính xác.

Correct mapping:

- trước `wifi_prov_init()`:
  `BOOTING`;
- trong runtime status sync:
  `BOOT_CONNECTING -> WIFI_CONNECTING`;
- `PROVISIONING/TESTING/RESTART_PENDING -> PROVISIONING`;
- `CONNECTED -> tiếp tục đánh giá gateway health`;
- `RECONNECTING -> WIFI_CONNECTING`;
- `FAILED -> ERROR/DEGRADED` theo policy.

---

# 42. Runtime status synchronization

V2 bổ sung application-level status synchronizer.

Không đặt logic này trong `board_io`.

Đề xuất ban đầu:

```text
main/
├── main.c
├── board_status_sync.c
└── board_status_sync.h
```

hoặc giữ static helper trong `main.c` nếu muốn tối giản.

Flow:

```text
wifi_prov_get_state() ----+
                          |
BLE/service health -------+--> application status resolver
                          |
startup flags ------------+
                                  |
                                  v
                         board_io_set_status()
```

Interval đề xuất:

```text
250–500 ms
```

Chỉ gọi `board_io_set_status()` khi resolved state thay đổi.

---

# 43. Status resolver policy

Ví dụ:

```text
if wifi == UNINITIALIZED
    BOOTING

else if wifi == BOOT_CONNECTING ||
        wifi == RECONNECTING
    WIFI_CONNECTING

else if wifi == PROVISIONING ||
        wifi == TESTING ||
        wifi == RESTART_PENDING
    PROVISIONING

else if wifi == FAILED
    ERROR

else if wifi == CONNECTED && critical_service_failed
    ERROR

else if wifi == CONNECTED && optional_service_failed
    DEGRADED

else if wifi == CONNECTED && gateway_ready
    READY

else
    BOOTING
```

`board_io` không biết các rule này.

---

# 44. Provisioning mode

Firmware hiện có flow return khỏi `app_main()` khi provisioning mode active.

Điều này không làm worker task chết.

Sau:

```c
board_io_init();
```

FreeRTOS task tiếp tục chạy dù `app_main()` return.

Vì vậy:

```text
LED
button
display
status synchronizer
```

có thể tiếp tục trong provisioning mode nếu được tạo trước return.

---

# 45. Display presenter và `gateway_status`

`gateway_status_get()` hữu ích khi gateway services đã initialize.

Không gọi nó một cách mù quáng trong early provisioning nếu các dependency của nó chưa sẵn sàng.

Presenter phải có startup phase awareness.

Ví dụ normal mode:

```c
gateway_status_t status;
if (gateway_status_get(&status) == ESP_OK) {
    board_display_frame_t frame = {0};

    snprintf(frame.line[0], sizeof(frame.line[0]),
             "ESP BLE Gateway");

    snprintf(frame.line[1], sizeof(frame.line[1]),
             "IP: %s", status.ip);

    snprintf(frame.line[2], sizeof(frame.line[2]),
             "BLE: %d/%d",
             status.connected_count,
             status.device_count);

    if (status.has_wifi_rssi) {
        snprintf(frame.line[3], sizeof(frame.line[3]),
                 "RSSI: %d dBm", status.wifi_rssi);
    } else {
        snprintf(frame.line[3], sizeof(frame.line[3]),
                 "RSSI: n/a");
    }

    board_io_display_update(&frame);
}
```

---

# 46. Command Dispatcher integration

Không expose:

```text
set_pin
get_pin
write_gpio
gpio_level
```

Nếu thêm remote board commands:

```text
identify_gateway
set_gateway_display
```

thì implementation gọi semantic `board_io` API.

Destructive command:

```text
restart_gateway
factory_reset_gateway
```

phải qua application control policy.

`board_io` không tự register command.

Ràng buộc repository: dispatcher registry bị freeze sau init (Appendix C).
Remote command mới (`identify_gateway`, `set_gateway_display`) phải được
đăng ký trong window init của application; không được thiết kế
lazy-registration.

---

# 47. CMake

Base `components/board_io/CMakeLists.txt`:

```cmake
set(board_io_priv_reqs
    esp_driver_gpio
    esp_timer
    log
)

# Add bus driver only when concrete backend is enabled.
if(CONFIG_BOARD_IO_DISPLAY_BACKEND_SSD1306_I2C)
    list(APPEND board_io_priv_reqs esp_driver_i2c)
endif()

idf_component_register(
    SRCS
        "board_io.c"
        "board_pin_map.c"
        "board_button.c"
        "board_button_fsm.c"
        "board_led.c"
        "board_led_pattern.c"
        "board_display.c"
        "display_backends/board_display_none.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "."
        "display_backends"
    PRIV_REQUIRES
        ${board_io_priv_reqs}
)
```

Không thêm `esp_driver_i2c` khi không có I2C backend.

---

# 48. Root `main/CMakeLists.txt`

Project dùng `MINIMAL_BUILD ON`.

Phải thêm:

```text
board_io
```

vào `REQUIRES` của `main`.

Nếu có `board_status_sync.c`, file đó thuộc `main` component và không tạo dependency reverse.

---

# 49. Test project integration

Repository có ESP-IDF test project riêng tại:

```text
test/
```

Append:

```text
board_io
```

vào `TEST_COMPONENTS` hiện tại trong:

```text
test/CMakeLists.txt
```

Không hard-code lại cả list trong tài liệu nếu list repository thay đổi; chỉ yêu cầu append component mới.

---

# 50. Test component CMake

```cmake
idf_component_register(
    SRCS
        "test_board_io.c"
        "test_board_pin_map.c"
        "test_board_button_fsm.c"
        "test_board_button_debounce.c"
        "test_board_led_pattern.c"
        "test_board_display.c"
    PRIV_INCLUDE_DIRS
        ".."
        "../display_backends"
    REQUIRES
        unity
        board_io
)
```

`PRIV_INCLUDE_DIRS ".."` là white-box access tới internal headers
(FSM, pattern engine) — chủ đích của thiết kế test, không phá rule
"public header duy nhất" vì chỉ test project nhìn thấy. Mechanism gắn
component khớp convention hiện có của repo (`<component>/test` qua
`TEST_COMPONENTS`).

Không thêm test-only symbol vào public API.

---

# 51. Test levels

```text
L0 Build/static
L1 Pure unit
L2 Component integration
L3 Hardware-in-the-loop
L4 Stress/soak
```

Board IO có phần logic đủ lớn để L1 không phụ thuộc hardware.

---

# 52. Lifecycle/API tests

## BO-API-001 — init success

Expected:

```text
UNINITIALIZED
init -> ESP_OK
is_initialized -> true
```

## BO-API-002 — double init rejected

```text
init -> ESP_OK
init -> ESP_ERR_INVALID_STATE
```

## BO-API-003 — deinit when uninitialized

```text
deinit -> ESP_OK
```

## BO-API-004 — reinit after deinit

```text
init   -> ESP_OK
deinit -> ESP_OK
init   -> ESP_OK
```

Đây là contract bắt buộc v2.

## BO-API-005 — API before init

Expected:

```text
ESP_ERR_INVALID_STATE
```

cho status/signal/display capability có support, bao gồm cả
`board_io_register_event_handler()` (v2.1).

## BO-API-006 — invalid status

Expected:

```text
ESP_ERR_INVALID_ARG
```

## BO-API-007 — invalid signal

Expected:

```text
ESP_ERR_INVALID_ARG
```

## BO-API-008 — null display frame

Expected:

```text
ESP_ERR_INVALID_ARG
```

khi display capability tồn tại.

## BO-API-009 — display disabled

Expected:

```text
ESP_ERR_NOT_SUPPORTED
```

## BO-API-010 — handler registration

- register first -> OK;
- register second -> invalid state;
- unregister NULL -> OK;
- register new after unregister -> OK.

## BO-API-011 — self-deinit protection

Từ callback context:

```text
board_io_deinit() -> ESP_ERR_INVALID_STATE
```

Không deadlock.

## BO-API-012 — display runtime-disabled semantics (v2.1)

Với `DISPLAY_ENABLE=y`:

```text
set_enabled(false)          -> ESP_OK
update(frame) khi disabled  -> ESP_OK, latest pending được giữ
set_enabled(true)           -> ESP_OK, render latest pending (BO-DSP-007)
```

Phân biệt rõ với compile-time disabled -> `ESP_ERR_NOT_SUPPORTED`.

---

# 53. Lifecycle stress test

## BO-API-STRESS-001

Warm-up:

```text
init
deinit
```

Sau đó lấy heap baseline.

Loop:

```text
100 cycles
init
deinit
```

Acceptance:

- không crash;
- không watchdog;
- không task leak;
- heap không giảm tuyến tính;
- cho phép process-lifetime allocation từ shared GPIO ISR service.

---

# 54. Button FSM tests

Pure, không GPIO.

## BO-BTN-001

Initial state RELEASED, không event.

## BO-BTN-002

Stable press:

```text
RELEASED -> PRESSED
```

không public event.

## BO-BTN-003

Short press:

```text
duration = restart_ms - 1
release
```

Expected exactly:

```text
BUTTON_SHORT_PRESS
```

## BO-BTN-004

Exact restart boundary:

```text
duration = restart_ms
```

Expected exactly:

```text
RESTART_REQUEST
```

## BO-BTN-005

Restart range:

```text
restart_ms < duration < factory_ms
```

Expected restart.

## BO-BTN-006

Factory boundary minus one:

```text
duration = factory_ms - 1
```

Expected restart.

## BO-BTN-007

Exact factory boundary:

```text
duration = factory_ms
```

Expected factory reset.

## BO-BTN-008

Long factory hold:

Expected exactly one factory event on release.

## BO-BTN-009

No restart event while still pressed.

## BO-BTN-010

Factory armed supersedes restart armed.

## BO-BTN-011

Duplicate released sample không emit thêm event.

## BO-BTN-012

Timestamp regression:

Pure FSM phải xử lý deterministic và không unsigned-underflow.

Khuyến nghị reset/reject sample và không emit destructive event.

---

# 55. Button debounce tests

## BO-DB-001

Single stable press.

## BO-DB-002

Bounce sequence:

```text
0  press
5  release
8  press
12 release
18 press
```

Expected một logical press sau quiet window.

## BO-DB-003

Không commit trước debounce deadline.

## BO-DB-004

Release bounce chỉ tạo một physical release.

## BO-DB-005

Short electrical glitch không tạo semantic action.

## BO-DB-006

Edge storm coalescing không overflow queue vì không dùng event queue.

---

# 56. Button armed overlay tests

## BO-BTN-OVL-001

Trước restart threshold:

```text
overlay NONE
```

## BO-BTN-OVL-002

Exact restart threshold:

```text
RESTART_ARMED
```

## BO-BTN-OVL-003

Exact factory threshold:

```text
FACTORY_ARMED
```

## BO-BTN-OVL-004

Release clear armed overlay trước khi application callback chạy xong.

---

# 57. LED pattern tests

## BO-LED-001

BOOTING boundary:

```text
0
99
100
199
200 ms
```

## BO-LED-002

PROVISIONING.

## BO-LED-003

WIFI_CONNECTING.

## BO-LED-004

READY steady.

## BO-LED-005

DEGRADED full cycle.

## BO-LED-006

ERROR triple blink + pause.

## BO-LED-007

Base state change resets pattern epoch.

## BO-LED-008

Active-low conversion.

---

# 58. LED overlay tests

## BO-LED-OVL-001 — activity

Activity starts pulse.

## BO-LED-OVL-002 — activity retrigger

New activity before deadline:

```text
deadline = latest_now + pulse_ms
```

## BO-LED-OVL-003 — identify

Identify lasts configured duration.

## BO-LED-OVL-004 — identify retrigger

Restart duration/pattern epoch.

## BO-LED-OVL-005 — latest base wins

Base changes while identify active.

After overlay:

```text
return latest base
```

## BO-LED-OVL-006 — ERROR > identify/activity

## BO-LED-OVL-007 — FACTORY_ARMED > ERROR

## BO-LED-OVL-008 — RESTART_ARMED > ERROR (v2.1)

Base ERROR, giữ nút quá restart threshold:

```text
LED thể hiện RESTART_ARMED, không còn error pattern
```

## BO-LED-OVL-009 — armed không bị che bởi identify/activity

IDENTIFY active + FACTORY_ARMED -> vẫn FACTORY_ARMED.

---

# 59. Display tests

Dùng fake backend.

## BO-DSP-001

Disabled capability -> `ESP_ERR_NOT_SUPPORTED`.

## BO-DSP-002

Frame copied.

Caller mutate original sau API call; render vẫn snapshot cũ.

## BO-DSP-003

Force NUL termination.

## BO-DSP-004

Rapid A/B/C update -> latest C render.

## BO-DSP-005

Refresh cap không bị vượt.

## BO-DSP-006

Disable runtime stops render.

## BO-DSP-007

Re-enable render latest pending frame.

## BO-DSP-008

Backend render failure không kill worker.

## BO-DSP-009

Optional backend init failure -> board_io init vẫn OK.

## BO-DSP-010

Required backend init failure -> board_io init fail.

## BO-DSP-011

No internal lock held khi fake render callback chạy.

---

# 60. Pin validation tests

## BO-PIN-001

button == LED -> invalid.

## BO-PIN-002

invalid GPIO -> invalid.

## BO-PIN-003

ESP32-S3 GPIO22 -> invalid.

## BO-PIN-004

GPIO23 -> invalid.

## BO-PIN-005

GPIO24 -> invalid.

## BO-PIN-006

GPIO25 -> invalid.

## BO-PIN-007

Disabled feature pin không gây conflict.

## BO-PIN-008

factory threshold <= restart threshold -> invalid.

## BO-PIN-009

debounce >= restart threshold -> invalid.

## BO-PIN-010

Backend-specific duplicate pin -> invalid.

## BO-PIN-011

GPIO26 (flash blocklist) -> invalid.

## BO-PIN-012

GPIO31 (flash blocklist) -> invalid.

## BO-PIN-013

GPIO35 với `CONFIG_SPIRAM_MODE_OCT=y` -> invalid;
với quad PSRAM -> valid + warning.

## BO-PIN-014

Polarity/pull contradiction -> invalid:

```text
ACTIVE_LOW=y + PULL_DOWN -> invalid
ACTIVE_LOW=n + PULL_UP   -> invalid
```

## BO-PIN-015

`PULL_NONE` -> init vẫn OK nhưng có warning log.

---

# 61. Thread-safety tests

## BO-THREAD-001

Hai task đồng thời gọi:

```text
set_status
signal
```

Không crash/data race biểu hiện.

## BO-THREAD-002

Display update concurrent.

Latest frame semantics vẫn nhất quán.

## BO-THREAD-003

Deinit đồng thời với API call.

Expected:

- API trước STOPPING có thể succeed;
- API sau STOPPING trả invalid state;
- không use-after-free.

## BO-THREAD-004

Callback gọi `set_status()`.

Phải không deadlock vì callback được gọi ngoài lock.

## BO-THREAD-005

Callback attempt `deinit()`.

Expected invalid state, không deadlock.

---

# 62. HIL — button

Board thật.

Procedure:

1. boot;
2. short press;
3. hold restart range;
4. hold factory range.

Verify semantic callback event.

Không thực hiện actual factory reset trong generic HIL test.

---

# 63. HIL — LED

Verify:

- active polarity;
- BOOTING;
- PROVISIONING;
- WIFI_CONNECTING;
- READY;
- ERROR;
- activity;
- identify;
- armed feedback.

Logic analyzer được ưu tiên nếu cần đo timing.

---

# 64. HIL — provisioning

Procedure:

1. xóa Wi-Fi credentials bằng test/setup có chủ đích;
2. reboot;
3. Wi-Fi provisioning active;
4. status sync resolve `PROVISIONING`;
5. LED thể hiện provisioning;
6. button vẫn hoạt động;
7. BLE/dispatcher chưa cần init.

PASS khi board IO không phụ thuộc normal gateway services.

---

# 65. HIL — runtime reconnect

Procedure:

1. boot normal;
2. READY;
3. làm AP mất kết nối;
4. `wifi_prov` chuyển runtime reconnect;
5. status synchronizer phải chuyển:
   ```text
   READY -> WIFI_CONNECTING
   ```
6. khi IP quay lại:
   ```text
   WIFI_CONNECTING -> READY
   ```

Đây là test bắt buộc vì v1 thiếu runtime synchronization.

---

# 66. HIL — display

Chỉ bật khi có concrete backend.

Test:

- initial render;
- multiple updates;
- disable/enable;
- I2C/SPI error;
- disconnect display;
- optional backend failure isolation.

Gateway BLE/Web không được crash vì display optional.

---

# 67. Restart test policy

Generic Unity auto-run không gọi `esp_restart()`.

Unit test chỉ chứng minh:

```text
button -> RESTART_REQUEST
```

Reboot test thật là test riêng.

Nếu automation qua reboot:

1. set RTC/noinit marker;
2. restart;
3. boot;
4. verify marker;
5. clear marker.

---

# 68. Factory reset test policy

Generic Unity auto-run không thực hiện destructive factory reset.

Unit test chỉ chứng minh:

```text
button -> FACTORY_RESET_REQUEST
```

Full factory reset test phải:

```text
manual
HIL
destructive
```

và có setup/cleanup riêng.

---

# 69. Stress tests

## BO-STRESS-001 — status churn

Hàng nghìn state changes.

## BO-STRESS-002 — activity storm

Repeated activity signals.

Không queue overflow.

## BO-STRESS-003 — identify retrigger storm

Deadline/state stable.

## BO-STRESS-004 — display update storm

Latest frame, bounded render rate.

## BO-STRESS-005 — synthetic button edge storm

Debounce collapse.

## BO-STRESS-006 — concurrent API storm

Multiple task gọi status/signal/display.

Không deadlock/watchdog.

---

# 70. Soak test

Duration:

```text
8–24 giờ
```

Workload:

- normal READY;
- Wi-Fi reconnect events;
- BLE traffic;
- periodic activity;
- display update;
- occasional physical/synthetic button press.

Theo dõi:

```text
free heap
minimum free heap
board task stack high-water
watchdog
reset reason
display error count
unexpected semantic button event
```

Acceptance:

- không reset bất ngờ;
- không deadlock;
- không linear heap leak;
- không task leak;
- không false factory reset request.

---

# 71. Observability

Optional diagnostics:

```c
typedef struct {
    uint32_t gpio_edges;
    uint32_t stable_presses;
    uint32_t short_presses;
    uint32_t restart_requests;
    uint32_t factory_reset_requests;
    uint32_t status_changes;
    uint32_t activity_signals;
    uint32_t identify_signals;
    uint32_t display_updates;
    uint32_t display_renders;
    uint32_t display_errors;
} board_io_stats_t;
```

Không bắt buộc public trong initial implementation.

Có thể compile dưới:

```text
CONFIG_BOARD_IO_DIAGNOSTICS
```

---

# 72. Logging

Tag:

```c
static const char *TAG = "board_io";
```

Init log:

```text
I board_io: button enabled gpio=X active_low=1
I board_io: status LED enabled gpio=Y active_low=0
I board_io: display backend=none
I board_io: worker started
```

Không log mỗi LED transition.

Semantic event có thể debug log.

Display repeated error phải rate-limit.

---

# 73. Memory policy

Không allocation per event.

Không malloc trong ISR.

Display fixed-size frame.

Worker task duy nhất.

No cJSON/JSON trong board_io.

No command message format trong board_io.

---

# 74. Failure isolation

Critical:

```text
invalid config
worker creation fail
required button/LED init fail
required display init fail
```

-> `board_io_init()` fail.

Optional display fail:

```text
log warning
display unavailable
button/LED continue
board_io_init() may succeed
```

Runtime display error:

```text
record/log
keep worker alive
keep button/LED alive
```

---

# 75. Security/safety

Remote raw GPIO control bị cấm.

Không expose:

```text
GPIO number
GPIO level
pin mode
pull mode
```

qua Web/MCP.

Chỉ expose semantic allow-listed actions.

Factory reset remote command, nếu có trong tương lai, cần security/confirmation policy riêng.

---

# 76. Production pin configuration

Không commit placeholder GPIO dưới dạng enabled production config.

`sdkconfig.defaults.esp32s3` chỉ bật feature sau khi pin được xác nhận.

Ví dụ template, không phải giá trị thật:

```text
CONFIG_BOARD_IO_BUTTON_ENABLE=y
CONFIG_BOARD_IO_BUTTON_GPIO=<CONFIRMED_GPIO>

CONFIG_BOARD_IO_LED_ENABLE=y
CONFIG_BOARD_IO_LED_GPIO=<CONFIRMED_GPIO>

CONFIG_BOARD_IO_DISPLAY_ENABLE=n
```

`<CONFIRMED_GPIO>` không được commit nguyên văn.

---

# 77. Phased implementation plan

## Phase 1 — Core lifecycle

Implement:

- public API;
- lifecycle;
- synchronization;
- task;
- stop handshake;
- pin validation skeleton.

Exit:

- API tests pass;
- reinit pass;
- no self-deinit deadlock.

## Phase 2 — Button

Implement:

- GPIO;
- ISR;
- debounce;
- pure FSM;
- armed state;
- semantic callback.

Exit:

- boundary tests;
- bounce tests;
- HIL button.

## Phase 3 — LED

Implement:

- pure pattern engine;
- overlays;
- polarity;
- activity/identify retrigger.

Exit:

- pattern boundary tests;
- HIL LED.

## Phase 4 — Application integration

Implement:

- early board_io init;
- application event forwarding;
- runtime status synchronizer;
- provisioning integration;
- reconnect integration.

Exit:

- provisioning HIL;
- reconnect HIL;
- normal regression.

## Phase 5 — Display abstraction

Implement:

- public frame;
- none backend;
- fake test backend;
- coalescing;
- refresh cap;
- enable/disable;
- failure isolation.

Exit:

- all display unit tests pass with fake/none backend.

## Phase 6 — Concrete display backend

Chỉ bắt đầu khi hardware xác nhận:

- controller;
- bus;
- resolution;
- address/chip-select;
- power/reset;
- GPIO.

Exit:

- physical render;
- bus failure recovery;
- no regression.

---

# 78. Files expected to change

New:

```text
components/board_io/**
components/board_io/test/**
```

Modify:

```text
main/main.c
main/CMakeLists.txt
test/CMakeLists.txt
sdkconfig.defaults.esp32s3   # only after hardware confirmation
```

Recommended:

```text
main/board_status_sync.c
main/board_status_sync.h
```

Optional docs:

```text
README.md
docs/Tai_lieu_Test_ESP32_BLE_Gateway.md
```

---

# 79. Definition of Done

- [ ] Public API đúng v2 contract.
- [ ] Lifecycle re-init sau deinit hoạt động.
- [ ] Double-init bị reject.
- [ ] Deinit có worker stop handshake.
- [ ] Self-deinit không deadlock.
- [ ] Public API thread-safe từ task context.
- [ ] Callback chạy ngoài internal locks.
- [ ] Button public event chỉ semantic action.
- [ ] Mỗi physical press phát tối đa một semantic event.
- [ ] Exact restart/factory boundaries được test.
- [ ] Bounce không tạo duplicate action.
- [ ] Button ISR không business logic.
- [ ] GPIO ISR service không bị board_io uninstall.
- [ ] LED pattern non-blocking.
- [ ] Activity retrigger semantics đúng.
- [ ] Identify retrigger semantics đúng.
- [ ] FACTORY_ARMED priority cao nhất.
- [ ] RESTART_ARMED feedback hiển thị cả khi base ERROR (v2.1).
- [ ] ERROR không bị activity/identify che.
- [ ] Display abstraction không phụ thuộc I2C ở base layer.
- [ ] Display compile-time disabled trả `ESP_ERR_NOT_SUPPORTED`.
- [ ] Display runtime-disabled giữ latest pending, re-enable render lại (v2.1).
- [ ] Rapid display update được coalesce.
- [ ] Optional display failure không kill gateway.
- [ ] Pin conflict validation pass.
- [ ] Blocklist ESP32-S3 có test: 22–25, 26–32, 33–37 octal PSRAM (v2.1).
- [ ] Polarity/pull contradiction bị reject (v2.1).
- [ ] Production pin map đã review schematic.
- [ ] `main/CMakeLists.txt` link board_io.
- [ ] `test/CMakeLists.txt` include board_io.
- [ ] Runtime status synchronizer tồn tại.
- [ ] Provisioning status đúng.
- [ ] Runtime Wi-Fi reconnect status đúng.
- [ ] Existing BLE/Web/MCP behavior regression pass.
- [ ] No linear heap leak after warm-up init/deinit cycles.
- [ ] No watchdog/deadlock under stress.
- [ ] Soak test pass.

---

# 80. Acceptance criteria

| Area | Acceptance |
|---|---|
| Lifecycle | `init -> deinit -> init` pass |
| Double init | second init while RUNNING rejected |
| Deinit | worker confirms stop before resources free |
| Button | exactly one semantic action per physical press |
| Debounce | no duplicate event under bounce pattern |
| Restart | exact threshold classified correctly |
| Factory reset | exact threshold classified correctly |
| LED | API call does not block for pattern duration |
| Activity | latest trigger extends pulse |
| Identify | retrigger restarts duration |
| Error state | not obscured by activity/identify |
| Restart armed | visible feedback even when base is ERROR (v2.1) |
| Pin blocklist | flash/PSRAM pins rejected per module config (v2.1) |
| Display runtime off | update accepted, latest pending restored on enable (v2.1) |
| Display | burst update latest-state-wins |
| Display refresh | never exceeds configured cap |
| Display fault | optional fault does not kill worker |
| Runtime status | reconnect reflected without reboot |
| Thread safety | no deadlock in concurrent API test |
| Memory | no linear leak after ISR-service warm-up |
| HIL | button/LED/provisioning/reconnect pass |
| Soak | 8–24 h without unexpected reset/deadlock |

---

# 81. Những quyết định còn phụ thuộc hardware

Spec v2 cố tình chưa quyết định:

1. Button GPIO thật.
2. LED GPIO thật.
3. LED là GPIO đơn, RGB hay addressable.
4. Display controller.
5. Display bus.
6. Display resolution.
7. I2C address/SPI CS.
8. External pull resistor.
9. Active polarity thực tế.
10. PCB revision pin mapping.

Các quyết định này phải đến từ schematic/hardware BOM.

Không được lấp khoảng trống bằng giả định software.

---

# 82. Những khoảng trống ngoài phạm vi `board_io`

Sau khi component hoàn thành, project vẫn cần quyết định application-level:

- factory reset thực sự xóa những gì;
- BLE bonds được purge bằng workflow nào;
- remote restart/factory-reset security;
- short press có chức năng gì;
- display screen layout cuối cùng;
- DEGRADED vs ERROR policy cho từng subsystem.

`board_io` không được tự quyết định các vấn đề này.

---

# 83. Kết luận kiến trúc

Boundary cuối cùng:

```text
          application/system policy
                   |
      +------------+------------+
      |                         |
      v                         v
 status resolver          action controller
      |                         ^
      v                         |
 board_io_set_status       semantic button event
      |                         ^
      +-----------+-------------+
                  |
               board_io
             /    |     \
         button   LED   display
            |      |      |
           GPIO   GPIO   backend
```

Invariant quan trọng:

```text
board_io không biết Wi-Fi
board_io không biết BLE
board_io không biết Device Store
board_io không biết MCP
board_io không biết factory reset policy
```

Nó chỉ biết hardware behavior và semantic board-level I/O.

Đây là contract được khuyến nghị để triển khai production component.

---

# Appendix A — Test command

Production:

```bash
idf.py set-target esp32s3
idf.py build
```

Test project:

```bash
cd test
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

Unity test project chạy độc lập với root firmware project.

---

# Appendix B — Review checklist cho implementation PR

## Architecture

- [ ] Không reverse dependency.
- [ ] Không raw GPIO API.
- [ ] Không Wi-Fi/BLE include trong board_io.

## Lifecycle

- [ ] Reinit works.
- [ ] Stop handshake.
- [ ] Failure cleanup đúng partial-init stage.

## ISR

- [ ] ISR minimal.
- [ ] No malloc/log/business action.
- [ ] ISR service global ownership đúng.

## Threading

- [ ] Callback ngoài lock.
- [ ] Deinit race safe.
- [ ] API after STOPPING rejected.

## Button

- [ ] Debounce quiet-window.
- [ ] One semantic event per press.
- [ ] Exact thresholds.
- [ ] Armed overlay beats ERROR (v2.1).

## LED

- [ ] Deadline-driven.
- [ ] Overlay priority.
- [ ] Retrigger semantics.

## Display

- [ ] Base abstraction bus-neutral.
- [ ] Copy frame.
- [ ] Coalescing.
- [ ] Refresh cap.
- [ ] Runtime-disable keeps latest pending (v2.1).
- [ ] Optional failure isolation.

## Test

- [ ] Component wired into test app.
- [ ] S3 GPIO blocklist tests present (v2.1).
- [ ] No destructive auto-run.
- [ ] HIL provisioning.
- [ ] HIL reconnect.
- [ ] Stress/soak evidence.

---

# Appendix C — Repository context used by this specification

Specification này được hoàn thiện để phù hợp với trạng thái repository hiện tại:

- firmware root và `test/` là hai ESP-IDF project riêng;
- test dùng Unity trên ESP32-S3 thật;
- root build dùng `MINIMAL_BUILD ON`;
- `main` hiện trực tiếp điều phối Wi-Fi/BLE/dispatcher/web lifecycle;
- Wi-Fi provisioning có runtime state machine riêng;
- `gateway_status` tổng hợp nhiều runtime metric;
- dispatcher registry được freeze sau init;
- `wifi_prov_clear_credentials()` tồn tại nhưng full factory-reset workflow chưa phải API duy nhất của project.

Phiên bản 2.0 cố tình tách phần được source hiện tại hỗ trợ khỏi các quyết định hardware/application policy chưa được xác nhận.
