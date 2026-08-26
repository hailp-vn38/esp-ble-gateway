# board_io

Component quản lý toàn bộ I/O vật lý của board gateway trên ESP32-S3:
button reset/factory-reset, status LED và display text tùy chọn (4 dòng x 32 ký tự).
Target ESP-IDF 5.4.4, build cùng firmware root hoặc chạy unit test qua project `test/`.

## Nguyên tắc thiết kế

- Application nói bằng **semantic intent**, component quyết định phần cứng:
  - Application gọi: `board_io_set_status()`, `board_io_signal()`,
    `board_io_display_update()`.
  - Component tự xử lý: GPIO level, debounce, phân loại hold duration,
    LED pattern timing, display backend.
- Chiều ngược lại, component chỉ emit **semantic event** qua một handler duy nhất:
  `BUTTON_SHORT_PRESS`, `RESTART_REQUEST`, `FACTORY_RESET_REQUEST`.
  Component không bao giờ tự restart hay xóa NVS.
- Không phụ thuộc Wi-Fi/BLE/device_store/web_server/mcp_endpoint.
  Component hoạt động độc lập kể cả khi gateway services chưa init
  (ví dụ trong provisioning mode khi `app_main()` return sớm).
- Public API thread-safe từ FreeRTOS task context, **không ISR-safe**.
  Callback application được gọi trong worker task của component, ngoài mọi lock.

## Vòng đời

```text
UNINITIALIZED --init--> INITIALIZING --ok--> RUNNING --deinit--> STOPPING --> UNINITIALIZED
       ^                    |                                          |
       +-------- fail ------+              timeout stop handshake ----+
```

- `init()` hai lần liên tiếp khi đang RUNNING: `ESP_ERR_INVALID_STATE`.
- Sau `deinit()` thành công được phép `init()` lại.
- `deinit()` có stop handshake: disable interrupt → remove ISR handler →
  notify worker → chờ worker xác nhận dừng → mới giải phóng tài nguyên.
  Timeout 3 giây: trả `ESP_ERR_TIMEOUT`, giữ nguyên tài nguyên.
- Gọi `deinit()` từ chính callback của component: `ESP_ERR_INVALID_STATE`
  (không deadlock).

## Public API

| Hàm | Mô tả | Lỗi chính |
|---|---|---|
| `board_io_init()` | Validate pin, init module, tạo worker | `INVALID_ARG` khi pin/threshold sai, driver error |
| `board_io_deinit()` | Stop handshake rồi teardown | `INVALID_STATE` nếu đang init/stopping, `TIMEOUT` |
| `board_io_is_initialized()` | True chỉ ở RUNNING | — |
| `board_io_register_event_handler(h, ctx)` | Một handler duy nhất; `NULL` = unregister | `INVALID_STATE` trước init hoặc handler thứ hai |
| `board_io_set_status(status)` | Đặt base state cho LED, non-blocking | `INVALID_ARG`, `INVALID_STATE` |
| `board_io_signal(signal)` | `ACTIVITY` hoặc `IDENTIFY` overlay | `INVALID_ARG`, `INVALID_STATE` |
| `board_io_display_update(frame)` | Copy frame, render async | `NOT_SUPPORTED` khi capability tắt, `INVALID_ARG` frame NULL |
| `board_io_display_set_enabled(bool)` | Bật/tắt display lúc runtime | `NOT_SUPPORTED` khi không có backend |

Trạng thái hệ thống (`board_status_t`): `BOOTING`, `PROVISIONING`,
`WIFI_CONNECTING`, `READY`, `DEGRADED`, `ERROR`.

## Hoạt động chi tiết

### Button

- Debounce kiểu quiet-window mặc định 40 ms; edge chỉ notify, mọi xử lý
  nằm ở worker task; bounce/storm coalesce tự nhiên nên không cần queue.
- Ngưỡng giữ nút (tính từ lúc press ổn định):

```text
duration < 2000 ms                -> BUTTON_SHORT_PRESS     (khi release)
2000 ms <= duration < 8000 ms     -> RESTART_REQUEST        (khi release)
duration >= 8000 ms               -> FACTORY_RESET_REQUEST  (khi release)
```

- Event chỉ emit **một lần khi release** — giữ tiếp từ mốc 2 s lên 8 s
  vẫn chỉ ra một event factory-reset, không bắn restart sớm.
- Trong lúc giữ, LED phản hồi armed overlay (xem bên dưới) nhưng không
  emit event công khai.

### Status LED

Base pattern (single-color LED, anchor reset mỗi khi đổi base state):

| State | Pattern |
|---|---|
| BOOTING | 100 ms ON / 100 ms OFF |
| PROVISIONING | 500 ms ON / 500 ms OFF |
| WIFI_CONNECTING | 125 ms ON / 125 ms OFF |
| READY | sáng liên tục |
| DEGRADED | 150 ms ON / 850 ms OFF |
| ERROR | 3 nhịp 150 ms + pause 1000 ms (chu kỳ 1750 ms) |

Transient/armed overlay:

| Overlay | Biểu diễn | Thời lượng |
|---|---|---|
| ACTIVITY | sáng liên tục | pulse 80 ms, retrigger = extend |
| IDENTIFY | nhấp nháy 250/250 | 5000 ms, retrigger = restart từ đầu |
| RESTART_ARMED | sáng liên tục | trong suốt thời gian giữ qua mốc 2 s |
| FACTORY_ARMED | nhấp nháy 100/100 | trong suốt thời gian giữ qua mốc 8 s |

Độ ưu tiên hiển thị (cao → thấp):
`FACTORY_ARMED > RESTART_ARMED > ERROR > IDENTIFY > ACTIVITY > base`.

Lý do: armed là phản hồi hành động vật lý cục bộ nên phải thắng cả ERROR;
ERROR vẫn đứng trên IDENTIFY/ACTIVITY để tín hiệu remote không che lỗi.
Khi overlay kết thúc, LED quay về base state mới nhất (latest wins).

### Display

- Frame copy ngay trong API call (`ESP_OK` trả về là caller được mutate bản gốc);
  render async trong worker, nhiều update liên tiếp coalesce về frame mới nhất.
- Refresh cap mặc định 5 Hz (`BOARD_IO_DISPLAY_MAX_REFRESH_HZ`).
- Semantics tắt/bật:

```text
Compile-time disabled (BOARD_IO_DISPLAY_ENABLE=n):
    update / set_enabled        -> ESP_ERR_NOT_SUPPORTED

Runtime disabled (đã gọi set_enabled(false)):
    update(frame)               -> ESP_OK, giữ làm latest pending
    set_enabled(true)           -> ESP_OK nếu có backend;
                                   ESP_ERR_NOT_SUPPORTED nếu backend none
```

- Backend interface bus-neutral (`display_backends/board_display_backend.h`).
  Hiện chỉ có backend `none`; SSD1306/SPI thêm sau cùng với Kconfig riêng
  của backend đó. Render failure không làm chết worker hay gateway.

## Cấu hình Kconfig

| Option | Default | Ghi chú |
|---|---|---|
| `BOARD_IO_BUTTON_ENABLE` | n | |
| `BOARD_IO_BUTTON_GPIO` | -1 | -1 = chưa đặt, init sẽ fail |
| `BOARD_IO_BUTTON_ACTIVE_LOW` | y | |
| `BOARD_IO_BUTTON_PULL_MODE` | PULL_UP | choice: NONE / UP / DOWN |
| `BOARD_IO_BUTTON_DEBOUNCE_MS` | 40 | range 5–500 |
| `BOARD_IO_BUTTON_RESTART_MS` | 2000 | range 500–30000 |
| `BOARD_IO_BUTTON_FACTORY_RESET_MS` | 8000 | range 1000–60000 |
| `BOARD_IO_LED_ENABLE` | n | |
| `BOARD_IO_LED_GPIO` | -1 | -1 = chưa đặt |
| `BOARD_IO_LED_ACTIVE_LOW` | n | |
| `BOARD_IO_DISPLAY_ENABLE` | n | |
| `BOARD_IO_DISPLAY_BACKEND` | NONE | chỉ NONE khả dụng hiện tại |
| `BOARD_IO_DISPLAY_REQUIRED` | n | y = display fail làm fail toàn bộ init |
| `BOARD_IO_DISPLAY_MAX_REFRESH_HZ` | 5 | range 1–30 |
| `BOARD_IO_TASK_STACK_SIZE` | 3072 | range 2048–8192 |
| `BOARD_IO_TASK_PRIORITY` | 3 | range 1–10 |

Validation runtime bắt buộc thêm: `factory_reset_ms > restart_ms`,
`debounce_ms < restart_ms`. Vi phạm → `board_io_init()` trả
`ESP_ERR_INVALID_ARG` kèm log lỗi rõ ràng.

---

## Hướng dẫn triển khai PIN

Đây là phần quan trọng nhất khi đưa vào production. Nguyên tắc số một:
**Kconfig range 0–48 không mã hóa độ an toàn của pin** — một giá trị nằm
trong range vẫn có thể phá flash hoặc treo boot. Quy trình dưới đây bắt buộc
cho từng feature (button / LED / display) trước khi bật trong production.

### Bước 1 — Nắm ràng buộc cứng của ESP32-S3

Bảng blocklist mà runtime validation (`board_pin_map.c`) thực thi:

| GPIO | Phân loại | Hành vi của validation |
|---|---|---|
| 22–25 | Chip-invalid (không tồn tại vật lý) | **Reject** — `ESP_ERR_INVALID_ARG` |
| 26–32 | Nối sẵn SPI flash nội bộ | **Reject** — dùng sẽ crash/hỏng flash |
| 33–37 | Octal PSRAM (module N8R8/N16R8…) | **Reject** khi `CONFIG_SPIRAM_MODE_OCT=y`; warning khi quad PSRAM |
| 0, 3, 45, 46 | Strapping pins | Runtime cho phép nhưng **warning**; bắt buộc review schematic |
| 43, 44 | UART0 console mặc định (TX/RX) | Warning |
| Ngoài 0–48 | Invalid | Reject qua `GPIO_IS_VALID_GPIO` |

Chi tiết strapping (chỉ review, không bị chặn bởi software vì có thể là
lựa chọn hợp lệ khi có external resistor phù hợp):

- **GPIO0**: boot mode. Nếu dùng làm button, phải đảm bảo mức khi nhấn
  không kéo board vào download mode ngoài ý muốn (button boot thông thường
  chủ động kéo LOW khi nhấn — chấp nhận được vì người dùng chủ động nhấn).
- **GPIO3**: JTAG source select lúc reset.
- **GPIO45**: VDD_SPI voltage select lúc reset.
- **GPIO46**: boot mode phụ lúc reset; mức lơ lửng lúc boot phải được
  định nghĩa bằng trở ngoài nếu có gắn thiết bị khác.

### Bước 2 — Review schematic từng feature

Với mỗi pin ứng viên, đối chiếu schematic/BOM trước khi ghi vào config:

**Button:**
1. Pin không thuộc blocklist bảng trên.
2. Có trở kéo bên ngoài (thường 10k) hay dựa internal pull? Internal pull
   đủ dùng cho button trên dây ngắn; nếu dùng `PULL_NONE` thì external
   resistor là bắt buộc, validation chỉ warning chứ không đoán giúp.
3. Mức idle khi chưa nhấn xác định rõ (pull-up → active-low là tổ hợp chuẩn).
4. Dây dài/công nghiệp ồn → cân nhắc RC thêm và tăng `DEBOUNCE_MS`.
5. Nếu là GPIO0: xác nhận hành vi boot khi nhấn đúng thiết kế.

**Status LED:**
1. Pin phải output-capable (validation đã check `GPIO_IS_VALID_OUTPUT_GPIO`).
2. Trở hạn lưu đúng dòng (LED 20 mA điển hình); active level theo cách đấu
   cathode/anode — cấu hình `BOARD_IO_LED_ACTIVE_LOW` tương ứng.
3. Tránh pin có chức năng cao khác (USB D-/D+ là **19/20** trên S3 — nếu
   board dùng native USB CDC/JTAG thì tuyệt đối không dùng).

**Display (khi có backend thật):**
1. Controller/bus xác nhận (I2C/SPI), SDA≠SCL hoặc CS riêng.
2. Tần số bus nằm trong giới hạn (I2C tối đa 400 kHz trong spec hiện hành).
3. Power/reset pin nếu controller yêu cầu.
4. Các pin bus cũng phải qua bảng blocklist ở Bước 1.

### Bước 3 — Chọn polarity và pull mode (quy tắc chống phantom press)

Tổ hợp sai gây floating input → button "tự nhấn" ngẫu nhiên:

```text
ACTIVE_LOW=y  + PULL_DOWN   -> ESP_ERR_INVALID_ARG  (idle bị kéo thấp = nhấn)
ACTIVE_LOW=n  + PULL_UP     -> ESP_ERR_INVALID_ARG  (idle bị kéo cao = nhấn)
ACTIVE_LOW=*  + PULL_NONE   -> ESP_OK + WARNING (phải có trở ngoài)
ACTIVE_LOW=y  + PULL_UP     -> tổ hợp chuẩn
ACTIVE_LOW=n  + PULL_DOWN   -> tổ hợp chuẩn
```

Quy tắc nhớ nhanh: pull phải kéo input về **mức nghịch** với active level.

### Bước 4 — Bật config theo giai đoạn

**Giai đoạn dev/chưa có schematic:** giữ mọi feature `=n` (default).
Component vẫn build, init thành công với toàn bộ feature tắt, các API
trả lỗi đúng contract. Đây là trạng thái commit an toàn cho repo.

```text
# sdkconfig.defaults.esp32s3 — TRẠNG THÁI PRODUCTION AN TOÀN KHI CHƯA CHỐT PIN
# Không ghi CONFIG_BOARD_IO_*_ENABLE=y với GPIO placeholder
```

Cấm commit `<CONFIRMED_GPIO>` hoặc số pin "tạm" dạng enabled production
config. Placeholder phải nằm ở nơi khác (nhánh local, PR draft).

**Giai đoạn verify trên board thật (dev branch/local):** bật feature kèm
pin đã chọn, flash và xem log init. Ví dụ:

```text
CONFIG_BOARD_IO_BUTTON_ENABLE=y
CONFIG_BOARD_IO_BUTTON_GPIO=4
CONFIG_BOARD_IO_BUTTON_ACTIVE_LOW=y
CONFIG_BOARD_IO_LED_ENABLE=y
CONFIG_BOARD_IO_LED_GPIO=5
CONFIG_BOARD_IO_LED_ACTIVE_LOW=n
```

Log mong đợi khi init OK:

```text
I board_io: Button enabled gpio=4 active_low=1
I board_io: Status LED enabled gpio=5 active_low=0
I board_io: Worker started
```

Ví dụ log khi config sai (GPIO 27 trùng flash):

```text
E board_io: Button GPIO 27 is reserved for flash
```

→ `board_io_init()` trả `ESP_ERR_INVALID_ARG`; `main.c` log lỗi nhưng
gateway vẫn chạy tiếp (board IO degrade không kéo sập services).

**Giai đoạn production:** chỉ sau khi Bước 1–3 hoàn tất và verify pass,
ghi config cuối vào `sdkconfig.defaults.esp32s3`, xóa `sdkconfig` sinh ra
và rebuild để defaults áp dụng sạch.

### Bước 5 — Verify tự động trên target

Unit test project (`test/`) đã chặn các trường hợp blocklist bằng test thật:

| Nhóm test | Kiểm chứng gì |
|---|---|
| `pin map rejects chip-invalid gpio 22–25` | Macro validity chip-level |
| `pin map rejects flash blocklist gpio 26/31` | Vùng flash |
| `pin map applies psram policy on gpio 35` | Theo `CONFIG_SPIRAM_MODE_OCT` thực tế của build |
| `pin map rejects shared button and led gpio` | Trùng pin giữa 2 feature |
| `pin map ignores pins of disabled features` | Feature tắt không tham gia validation |
| `pin map rejects polarity pull contradiction` | Bảng Bước 3 |
| `pin map rejects factory/debounce threshold...` | Ngưỡng thời gian hợp lý |

Sau khi chọn pin production, chạy lại test suite; nếu thêm pin lạ vào
cấu hình mà vi phạm blocklist, `board_io_init()` fail ngay tại boot thay vì
hỏng hardware.

---

## Tích hợp application

Thứ tự trong `app_main()` (đã áp dụng ở `main/main.c`):

1. `log_buffer_init()`;
2. `board_io_init()` **sớm** — trước NVS/Wi-Fi; worker task sống qua
   provisioning-mode return nên LED/button/display vẫn hoạt động;
3. register event handler + `set_status(BOOTING)`;
4. `board_status_sync_start()` — task poll `wifi_prov_get_state()` mỗi
   300 ms, map sang `board_status_t` và chỉ push khi thay đổi:
   `BOOT_CONNECTING/RECONNECTING → WIFI_CONNECTING`,
   `PROVISIONING/TESTING/RESTART_PENDING → PROVISIONING`,
   `CONNECTED → READY`, `FAILED → ERROR`, còn lại `BOOTING`.

Policy action cho button event thuộc application (đang áp dụng trong
`main.c`): short-press chỉ log; restart → `esp_restart()`;
factory-reset → `wifi_prov_clear_credentials()` + `esp_restart()`.
Workflow factory-reset đầy đủ (BLE bond purge, device store wipe) là
quyết định application-level sau này; component không tự suy diễn.

Không đăng ký remote command điều khiển raw GPIO qua Web/MCP. Nếu thêm
`identify_gateway`/`set_gateway_display` thì gọi semantic API của component,
và phải đăng ký trong window init do dispatcher registry freeze sau init.

## Unit tests

64 test case thuộc tag `[board_io]` chạy trong Unity trên ESP32-S3 thật,
bao gồm: FSM boundary chính xác từng ngưỡng, debounce bounce/glitch/storm,
LED pattern boundary + priority overlay, pin validation blocklist,
lifecycle init/deinit/re-init/stress 100 chu kỳ không leak, concurrent
API smoke. Component đã nằm trong `TEST_COMPONENTS` của `test/CMakeLists.txt`.

HIL thủ công còn lại: nhấn nút vật lý qua đủ 3 vùng ngưỡng, đo LED bằng
mắt/logic analyzer, scenario provisioning/reconnect xem LED chuyển đúng,
rút/cắm display nếu có backend thật.

## Giới hạn hiện tại

- Backend display chỉ có `none` (capability testable, chưa có hardware);
  backend I2C/SPI thêm kèm Kconfig riêng của nó.
- ISR dùng `gpio_install_isr_service(0)` không IRAM; chỉ chuyển sang
  IRAM ISR khi có yêu cầu latency và review toàn bộ callback path.
- `DEGRADED` vs `ERROR` theo health từng subsystem (BLE/web) chưa map —
  hiện sync task chỉ dựa trạng thái Wi-Fi provisioning.
