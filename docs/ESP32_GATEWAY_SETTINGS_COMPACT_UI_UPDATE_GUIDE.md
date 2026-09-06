# Hướng dẫn cập nhật UI Gateway Settings cho ESP32-S3

**Repository:** `hailp-vn38/esp-ble-gateway`  
**Branch tham chiếu:** `dev-ws`  
**Mục tiêu:** làm phần thông tin hệ thống trên trang **Gateway Settings** gọn hơn, rõ nghĩa hơn và phù hợp với ESP32-S3.

---

## 1. Mục tiêu thay đổi

UI hiện tại đang tách các thông tin hệ thống thành nhiều card lớn như:

- Firmware
- Uptime
- Free Memory
- Free RAM / PSRAM

Cách hiển thị này chiếm nhiều chiều cao và giá trị `Free Memory` dễ gây nhầm với Internal RAM hoặc tổng heap có khả năng cấp phát từ nhiều vùng nhớ.

### UI mục tiêu

Gom thông tin thành **một card System** nhỏ gọn:

```text
┌────────────────────────────────────────────────────────────────────┐
│ SYSTEM                                                             │
│ Firmware               ESP-IDF              Uptime                 │
│ v1.0.0-95-g00c26be     v6.1-rc1             0h 00m                 │
│                                                                    │
│ Internal RAM           PSRAM                                        │
│ 107.3 KB free          7.4 MB free                                  │
│ Min 82.5 KB            Min 7.2 MB                                   │
└────────────────────────────────────────────────────────────────────┘
```

Trên desktop dùng grid. Trên mobile tự xuống dòng.

Không hiển thị card `Free Memory` riêng.

---

## 2. Trạng thái code hiện tại

Trang Settings đang được tách module và source chính nằm tại:

```text
components/web_server/www_src/dashboard/
├── views/settings.html
├── js/features/settings.js
└── js/core/i18n.js
```

API Settings:

```text
components/web_server/web_settings_api.c
```

Nguồn telemetry chung:

```text
components/gateway_status/
├── gateway_status.c
└── include/gateway_status.h
```

### Hiện tại `/api/settings` trả về

```json
{
  "system": {
    "firmware": "...",
    "idf": "...",
    "uptime_ms": 0,
    "free_heap": 0
  }
}
```

Trong khi `gateway_status_t` đã có sẵn telemetry chi tiết:

```c
uint32_t internal_free;
uint32_t internal_min_free;
uint32_t internal_largest_free_block;

bool psram_ready;
uint32_t psram_free;
uint32_t psram_min_free;
uint32_t psram_largest_free_block;
```

Vì vậy **không cần tạo thêm module thu thập RAM mới**.

---

## 3. Quyết định thiết kế

### 3.1 Không dùng `free_heap` làm thông tin chính trên UI

Code hiện tại lấy:

```c
status->free_heap = esp_get_free_heap_size();
```

Giá trị này phù hợp cho telemetry tổng quát, nhưng label `Free Memory` trên UI không nói rõ loại memory nào.

Đề xuất:

- Giữ `free_heap` trong API để tránh breaking change.
- Không hiển thị `free_heap` ở phần summary.
- Hiển thị:
  - `internal_free`
  - `internal_min_free`
  - `psram_free`
  - `psram_min_free`

Điều này hữu ích hơn khi debug ESP32 chạy đồng thời BLE, Wi-Fi, WebSocket, Web UI và MCP.

---

## 4. Backend: mở rộng `/api/settings`

### File

```text
components/web_server/web_settings_api.c
```

Tại `settings_get_handler()`, phần hiện tại:

```c
cJSON *system = cJSON_AddObjectToObject(response, "system");
cJSON_AddStringToObject(system, "firmware", gw_status.firmware_version);
cJSON_AddStringToObject(system, "idf", gw_status.idf_version);
cJSON_AddNumberToObject(system, "uptime_ms", (double)gw_status.uptime_ms);
cJSON_AddNumberToObject(system, "free_heap", gw_status.free_heap);
```

Thêm telemetry memory:

```c
cJSON_AddNumberToObject(system, "internal_free",
                        gw_status.internal_free);
cJSON_AddNumberToObject(system, "internal_min_free",
                        gw_status.internal_min_free);

cJSON_AddBoolToObject(system, "psram_ready",
                      gw_status.psram_ready);

if (gw_status.psram_ready) {
    cJSON_AddNumberToObject(system, "psram_free",
                            gw_status.psram_free);
    cJSON_AddNumberToObject(system, "psram_min_free",
                            gw_status.psram_min_free);
}
```

### JSON mong muốn

```json
{
  "system": {
    "firmware": "v1.0.0-95-g00c26be-dirty",
    "idf": "v6.1-rc1",
    "uptime_ms": 120000,
    "free_heap": 7864320,

    "internal_free": 109875,
    "internal_min_free": 84480,

    "psram_ready": true,
    "psram_free": 7759462,
    "psram_min_free": 7581204
  }
}
```

> `free_heap` vẫn giữ lại để tương thích với client hiện tại.

---

## 5. Frontend HTML: gom các card thành một card

### File

```text
components/web_server/www_src/dashboard/views/settings.html
```

Hiện tại phần System Summary đang dùng:

```html
<div class="grid grid-cols-1 md:grid-cols-3 gap-4">
```

với ba card riêng:

- Firmware
- Uptime
- Free Memory

### Thay bằng một card

Đề xuất:

```html
<section aria-labelledby="settings-summary-title">
    <h3 id="settings-summary-title"
        data-i18n="settings.system_summary"
        class="sr-only">
        System summary
    </h3>

    <article class="bg-white rounded-xl border border-gray-200 p-4 md:p-5 shadow-sm">

        <div class="flex items-center gap-2 mb-4">
            <i class="ph ph-cpu text-lg text-brand-600"
               aria-hidden="true"></i>

            <h3 data-i18n="settings.system"
                class="text-sm font-semibold text-gray-900">
                System
            </h3>
        </div>

        <div class="grid grid-cols-2 md:grid-cols-3 gap-x-6 gap-y-4">

            <div class="min-w-0">
                <p data-i18n="settings.firmware_version"
                   class="text-xs font-medium text-gray-500">
                    Firmware
                </p>
                <p id="set-fw-version"
                   class="mt-1 text-sm font-semibold text-gray-900 font-mono break-all">
                    —
                </p>
            </div>

            <div>
                <p data-i18n="settings.idf_version"
                   class="text-xs font-medium text-gray-500">
                    ESP-IDF
                </p>
                <p id="set-idf-version"
                   class="mt-1 text-sm font-semibold text-gray-900 font-mono">
                    —
                </p>
            </div>

            <div>
                <p data-i18n="settings.uptime"
                   class="text-xs font-medium text-gray-500">
                    Uptime
                </p>
                <p id="set-uptime"
                   class="mt-1 text-lg font-semibold text-gray-900">
                    —
                </p>
            </div>

            <div>
                <p data-i18n="settings.internal_ram"
                   class="text-xs font-medium text-gray-500">
                    Internal RAM
                </p>
                <p id="set-internal-free"
                   class="mt-1 text-lg font-semibold text-gray-900 font-mono">
                    —
                </p>
                <p id="set-internal-min"
                   class="mt-0.5 text-xs text-gray-500 font-mono">
                    —
                </p>
            </div>

            <div>
                <p data-i18n="settings.psram"
                   class="text-xs font-medium text-gray-500">
                    PSRAM
                </p>
                <p id="set-psram-free"
                   class="mt-1 text-lg font-semibold text-gray-900 font-mono">
                    —
                </p>
                <p id="set-psram-min"
                   class="mt-0.5 text-xs text-gray-500 font-mono">
                    —
                </p>
            </div>

        </div>
    </article>
</section>
```

### Điểm chính

Không còn:

```text
Firmware card
Uptime card
Free Memory card
Free RAM card
```

Mà chỉ còn:

```text
System card
```

---

## 6. Frontend JS: bind dữ liệu mới

### File

```text
components/web_server/www_src/dashboard/js/features/settings.js
```

Hiện tại trong `settings.load()`:

```js
document.getElementById('set-fw-version').textContent =
    system.firmware || '—';

document.getElementById('set-idf-version').textContent =
    `IDF ${system.idf || '—'}`;

document.getElementById('set-uptime').textContent =
    this.formatUptime(system.uptime_ms || 0);

document.getElementById('set-heap').textContent =
    this.formatMemory(system.free_heap || 0);
```

### Thay phần memory bằng

```js
document.getElementById('set-fw-version').textContent =
    system.firmware || '—';

document.getElementById('set-idf-version').textContent =
    system.idf || '—';

document.getElementById('set-uptime').textContent =
    this.formatUptime(system.uptime_ms || 0);

document.getElementById('set-internal-free').textContent =
    `${this.formatMemory(system.internal_free || 0)} free`;

document.getElementById('set-internal-min').textContent =
    `Min ${this.formatMemory(system.internal_min_free || 0)}`;

const psramReady = Boolean(system.psram_ready);

document.getElementById('set-psram-free').textContent =
    psramReady
        ? `${this.formatMemory(system.psram_free || 0)} free`
        : 'N/A';

document.getElementById('set-psram-min').textContent =
    psramReady
        ? `Min ${this.formatMemory(system.psram_min_free || 0)}`
        : '';
```

### Xóa binding cũ

```js
document.getElementById('set-heap')
```

Nếu không còn nơi nào khác dùng `id="set-heap"` thì xóa element này khỏi HTML.

---

## 7. i18n

### File

```text
components/web_server/www_src/dashboard/js/core/i18n.js
```

Bổ sung các key tương ứng cho `en` và `vi`.

### English

```js
"settings.system": "System",
"settings.idf_version": "ESP-IDF",
"settings.internal_ram": "Internal RAM",
"settings.psram": "PSRAM",
"settings.minimum_free": "Minimum free",
```

### Tiếng Việt

```js
"settings.system": "Hệ thống",
"settings.idf_version": "ESP-IDF",
"settings.internal_ram": "RAM nội",
"settings.psram": "PSRAM",
"settings.minimum_free": "Thấp nhất",
```

Nếu muốn hỗ trợ dịch hoàn toàn chuỗi `Min ...`, nên tạo helper thay vì hard-code chữ `Min` trong JS.

Ví dụ:

```js
`${i18n.t('settings.minimum_free')} ${this.formatMemory(...)}`
```

---

## 8. Có nên hiển thị ESP32-S3 / CPU 240 MHz không?

Có thể, nhưng không cần cho phiên bản đầu tiên.

Nếu muốn hiển thị:

```text
ESP32-S3
240 MHz · 2 cores
```

không nên hard-code trong HTML nếu firmware có khả năng chạy trên target khác.

Có thể mở rộng `gateway_status_t` sau bằng:

```c
esp_chip_info_t chip_info;
esp_chip_info(&chip_info);
```

và:

```c
esp_clk_cpu_freq() / 1000000
```

Sau đó trả API:

```json
{
  "chip": "ESP32-S3",
  "cores": 2,
  "cpu_mhz": 240
}
```

Đây là **optional enhancement**, không phải requirement của lần refactor UI này.

---

## 9. Không nên thêm Flash vào card ở giai đoạn này

Flash không cùng semantics với heap.

Nếu muốn hiển thị Flash phải xác định rõ:

- Flash chip size
- App partition size
- OTA partition
- Data partition
- Free filesystem space

Không nên chỉ thêm một giá trị `Flash xx MB free` nếu chưa định nghĩa chính xác `free` là gì.

---

## 10. Responsive layout

### Desktop

```text
Firmware       ESP-IDF       Uptime
Internal RAM   PSRAM
```

### Mobile

```text
Firmware       ESP-IDF
Uptime         Internal RAM
PSRAM
```

Tailwind:

```html
grid grid-cols-2 md:grid-cols-3 gap-x-6 gap-y-4
```

Không cần card con cho từng metric.

---

## 11. Kích thước UI đề xuất

Card chính:

```text
padding desktop: 20px
padding mobile: 16px
border radius: 12px
```

Label:

```text
12px
font-medium
text-gray-500
```

Firmware / IDF:

```text
14px
font-semibold
font-mono
```

Metric:

```text
18px
font-semibold
```

Không nên dùng `text-2xl` hoặc font lớn hơn cho mỗi metric vì Settings là màn hình cấu hình, không phải dashboard KPI.

---

## 12. File cần thay đổi

Bắt buộc:

```text
components/web_server/web_settings_api.c

components/web_server/www_src/dashboard/views/settings.html

components/web_server/www_src/dashboard/js/features/settings.js

components/web_server/www_src/dashboard/js/core/i18n.js
```

Không cần thay đổi:

```text
components/gateway_status/gateway_status.c
components/gateway_status/include/gateway_status.h
```

vì Internal RAM và PSRAM telemetry đã tồn tại.

---

## 13. Build Web UI

Source cần sửa trong:

```text
components/web_server/www_src/
```

Không sửa trực tiếp generated file nếu build system đang generate:

```text
components/web_server/www/dashboard.html
```

Sau khi chỉnh source, chạy build Web UI theo tool của project:

```bash
python3 components/web_server/tools/build_webui.py
```

Sau đó build firmware:

```bash
idf.py build
```

Nếu project sử dụng script build/flash riêng:

```bash
./build_flash.sh
```

---

## 14. Kiểm thử API

Sau khi flash firmware:

```bash
curl http://<gateway-ip>/api/settings
```

Kiểm tra:

```json
{
  "system": {
    "firmware": "...",
    "idf": "...",
    "uptime_ms": 123456,
    "internal_free": 100000,
    "internal_min_free": 80000,
    "psram_ready": true,
    "psram_free": 7000000,
    "psram_min_free": 6800000
  }
}
```

### Điều kiện pass

- `internal_free > 0`
- `internal_min_free > 0`
- `internal_min_free <= internal_free` trong trường hợp hiện tại chưa xuống thấp hơn sau thời điểm snapshot; nhìn chung min-free phải không lớn hơn mức free lớn nhất đã quan sát.
- Nếu PSRAM có sẵn:
  - `psram_ready == true`
  - `psram_free > 0`
  - `psram_min_free > 0`
- API cũ vẫn có `free_heap`.

---

## 15. Kiểm thử UI

### Desktop

Kiểm tra:

- Chỉ còn một card System.
- Firmware không overflow.
- Uptime nằm cùng card.
- Internal RAM và PSRAM rõ ràng.
- Không còn `Free Memory` card.
- Chiều cao phần System giảm đáng kể.

### Mobile

Kiểm tra viewport:

```text
320 px
375 px
390 px
430 px
```

Yêu cầu:

- Không horizontal scroll.
- Firmware dài được wrap.
- Metric không đè nhau.
- Grid tự chuyển thành 2 cột.

---

## 16. Kiểm thử runtime memory

Gateway này chạy đồng thời:

```text
BLE
Wi-Fi
HTTP Server
WebSocket
MCP
Xiaozhi MCP Bridge
```

Nên kiểm tra `internal_min_free`, không chỉ `internal_free`.

Test flow:

```text
Boot
 ↓
Connect Wi-Fi
 ↓
Open Web UI
 ↓
Scan BLE
 ↓
Connect nhiều device
 ↓
Open WebSocket
 ↓
Run MCP requests
 ↓
Xiaozhi connect/reconnect
 ↓
Reload Settings
```

Quan sát:

```text
Internal RAM free
Internal RAM min
PSRAM free
PSRAM min
```

Nếu `internal_min_free` tụt thấp liên tục qua mỗi chu kỳ thao tác thì cần kiểm tra memory leak hoặc allocation pressure.

---

## 17. Acceptance criteria

Thay đổi được xem là hoàn thành khi:

- [ ] Settings chỉ dùng một card System cho firmware/runtime/memory.
- [ ] Không còn card `Free Memory`.
- [ ] Internal RAM hiển thị riêng.
- [ ] PSRAM hiển thị riêng.
- [ ] Minimum Internal RAM được hiển thị.
- [ ] Minimum PSRAM được hiển thị khi PSRAM available.
- [ ] `/api/settings` vẫn giữ field `free_heap` để backward compatibility.
- [ ] Không thêm API polling mới.
- [ ] Không tạo thêm task ESP-IDF.
- [ ] Không tạo thêm memory telemetry module.
- [ ] Desktop layout gọn hơn UI cũ.
- [ ] Mobile không overflow.
- [ ] English / Vietnamese hoạt động.
- [ ] Build Web UI thành công.
- [ ] `idf.py build` thành công.

---

## 18. Thứ tự triển khai đề xuất

```text
Phase 1
  web_settings_api.c
       ↓
  expose internal/PSRAM telemetry

Phase 2
  settings.html
       ↓
  gom System cards

Phase 3
  settings.js
       ↓
  bind API mới

Phase 4
  i18n.js
       ↓
  thêm label

Phase 5
  build_webui.py
       ↓
  idf.py build

Phase 6
  flash + API test + responsive test
```

---

## 19. Kết quả mong muốn

### Trước

```text
[Firmware                          ]

[Uptime                            ]

[Free Memory                       ]

[Free RAM / PSRAM                  ]
```

### Sau

```text
┌────────────────────────────────────────────────────┐
│ SYSTEM                                             │
│ Firmware        ESP-IDF        Uptime              │
│ v1.0.0...       v6.1-rc1       0h 00m              │
│                                                    │
│ Internal RAM    PSRAM                               │
│ 107.3 KB free   7.4 MB free                        │
│ Min 82.5 KB     Min 7.2 MB                         │
└────────────────────────────────────────────────────┘
```

Ưu điểm:

- giảm đáng kể chiều cao trang;
- phân biệt đúng Internal SRAM và PSRAM;
- thông tin hữu ích hơn cho debug ESP32-S3;
- không tạo thêm telemetry hoặc task;
- giữ backward compatibility API;
- tận dụng `gateway_status` hiện tại làm single source of truth.
