# ESP32-S3 BLE Gateway — Production Memory & Resource Implementation Guide

**Phiên bản:** 2.1 — ESP-IDF v6.1-rc1 migration baseline  
**Ngày:** 2026-08-27  
**Dự án:** `hailp-vn38/esp-ble-gateway`  
**Repository baseline:** commit `5baf93517f448144958e612531b5752652f49965`  
**Target:** ESP32-S3 / ESP-IDF v6.1-rc1 (pin đúng Git tag `v6.1-rc1`)  
**Phần cứng:** 16 MiB Flash + 8 MiB PSRAM + 512 KiB internal SRAM + 16 KiB RTC SRAM  
**Vai trò firmware:** BLE Central + Wi-Fi + HTTP/Web UI + REST + MCP/JSON-RPC subset + CBOR/QCBOR + NVS  
**Giới hạn BLE production:** tối đa 9 kết nối đồng thời trên ESP32-S3/NimBLE Kconfig của v6.1-rc1  
**Logging history/ring buffer:** không thuộc phạm vi; chức năng `log_buffer` được giả định loại khỏi dự án

---

## 1. Mục tiêu tài liệu

Tài liệu này là **implementation contract** để developer hoặc AI agent có thể tối ưu tài nguyên gateway theo một thứ tự xác định, có tiêu chí kiểm chứng và không dựa vào ước lượng mơ hồ.

Mục tiêu chính:

1. Giữ internal SRAM cho BLE, Wi-Fi, task stack và control path.
2. Khai thác PSRAM 8 MiB cho các buffer/caches lớn nhưng không phá vỡ tính ổn định.
3. Không để allocation lớn âm thầm ăn hết internal SRAM.
4. Giữ NimBLE trong internal memory ở baseline production.
5. Đo memory theo checkpoint runtime thay vì dùng budget tĩnh làm source of truth.
6. Tối ưu HTTP/cJSON để tránh peak heap lớn.
7. Sửa stack telemetry trước khi tối ưu kích thước task stack.
8. Chuyển Flash 16 MiB sang layout OTA A/B có rollback thực sự.
9. Giữ hệ thống ổn định ở 9 BLE links + Wi-Fi + HTTP workload đồng thời.

Tài liệu này không đặt mục tiêu "dùng hết RAM/PSRAM/Flash". Tối ưu đúng nghĩa là **dùng đúng loại bộ nhớ cho đúng workload và giữ headroom dự phòng**.

---

## 2. Phạm vi và non-goals

### 2.1 Trong phạm vi

- `sdkconfig.defaults`
- `sdkconfig.defaults.esp32s3`
- NimBLE memory policy
- PSRAM allocator policy
- `command_executor`
- `gateway_status`
- HTTP/cJSON memory behavior
- FreeRTOS stack sizing
- custom partition table
- OTA A/B + rollback validation
- runtime memory telemetry
- stress test 1/5/8/9 BLE links
- build/CI memory checks

### 2.2 Ngoài phạm vi

- `log_buffer` / circular logging history
- giữ log history trong PSRAM
- filesystem logging
- BLE Peripheral/dual-role giai đoạn sau
- tăng giới hạn BLE trên 9 bằng patch ESP-IDF
- ép task stack sang PSRAM
- ép toàn bộ `.bss` sang PSRAM
- tối ưu 120 MHz PSRAM cho production

### 2.3 Giả định về logging

Tài liệu này giả định chức năng log history đã được loại bỏ. `ESP_LOGx()` có thể vẫn được dùng cho console/debug trong quá trình phát triển, nhưng không có RAM ring/history riêng.

---

## 3. Source of truth và toolchain target

Tài liệu phân biệt hai baseline để tránh nhầm lẫn:

- **Repository code baseline:** commit `5baf93517f448144958e612531b5752652f49965`; code hiện tại trước migration được mô tả/test với ESP-IDF 5.4.4.
- **Toolchain target mới:** ESP-IDF **`v6.1-rc1`**, target `esp32s3`. Mọi build/benchmark sau migration phải dùng đúng tag này; không dùng `master`, `release/6.1`, hoặc một bản 6.1 khác rồi coi là tương đương.

Tại repository baseline đã kiểm tra:

- Target: `esp32s3`.
- Flash: `16 MiB`.
- `CONFIG_BT_NIMBLE_MAX_CONNECTIONS=9`.
- `CONFIG_BT_CTRL_BLE_MAX_ACT=10`.
- `CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=256`.
- NimBLE Central + Observer + GATT Client.
- Device Store hỗ trợ tới 16 thiết bị persistent.
- HTTP/Web UI, REST và MCP chạy qua Wi-Fi.
- `command_executor` mặc định:
  - 2 workers;
  - queue length 2;
  - worker stack 4096 bytes.
- `gateway_status` hiện chỉ trả `esp_get_free_heap_size()` cho tổng heap, chưa tách internal/PSRAM, chưa có minimum/largest block.
- partition hiện tại vẫn dùng `CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y`.

### 3.1 Hard constraint BLE trên v6.1-rc1

Kconfig NimBLE của ESP-IDF `v6.1-rc1` vẫn giới hạn ESP32-S3 ở tối đa 9 concurrent connections, và yêu cầu phối hợp với `BT_CTRL_BLE_MAX_ACT`. Vì vậy production phải dùng:

```ini
CONFIG_BT_NIMBLE_MAX_CONNECTIONS=9
CONFIG_BT_CTRL_BLE_MAX_ACT=10
```

Không dùng 10 simultaneous connections làm acceptance target.

Lý do kiến trúc:

```text
9 BLE connections
+ 1 scanning/controller activity
-------------------------------
= activity budget 10
```

Device Store vẫn có thể chứa 16 thiết bị. "16 registered devices" không đồng nghĩa "16 connected links".

Trên controller Kconfig dùng bởi ESP32-S3 trong v6.1-rc1, `BT_CTRL_BLE_MAX_ACT` có range 1–10 và mỗi controller activity được mô tả tiêu thụ khoảng **828 bytes**. Vì target 9 links cần thêm scan/reconnect activity, `MAX_ACT=10` là intentional cost, không phải giá trị nên giảm chỉ để lấy lại ~828 B. Nếu sản phẩm đổi target active links thấp hơn, có thể benchmark giảm activity count tương ứng.

### 3.2 Những thay đổi IDF 6.x ảnh hưởng trực tiếp tới plan

Migration từ 5.4.4 lên v6.1-rc1 không được coi là chỉ đổi version string. Các thay đổi cần phản ánh trong implementation:

1. **PicolibC là libc mặc định từ IDF 6.0.** Binary/stdio/stack footprint có thể khác baseline 5.4.4; vì vậy mọi stack và heap checkpoint phải đo lại từ đầu trên v6.1-rc1. Không tái sử dụng số đo runtime cũ làm acceptance evidence.
2. **FreeRTOS function placement thay đổi trong IDF 6.0.** Phần lớn FreeRTOS code mặc định nằm trong Flash thay vì IRAM. Không bật `CONFIG_FREERTOS_IN_IRAM` ở baseline memory plan; chỉ bật sau benchmark latency/IRAM riêng.
3. **`esp_spiram.h` đã bị loại bỏ.** Chỉ dùng `esp_psram.h`. Các component trực tiếp include/use PSRAM API phải khai báo dependency `esp_psram`.
4. **Built-in `json` component đã bị loại bỏ trong IDF 6.0.** Project hiện đã dùng `espressif/cjson` qua Component Manager; giữ kiến trúc này và regenerate lock file bằng v6.1-rc1.
5. **PSRAM Kconfig v6.1-rc1 có `SPIRAM_BOOT_HW_INIT`, memory protection và các mode sử dụng rõ ràng.** Baseline sẽ dùng PSRAM qua `malloc()`/heap capabilities, nhưng application allocation quan trọng vẫn phải đi qua `memory_policy`.
6. **PSRAM 120 MHz trên ESP32-S3:** v6.1-rc1 đánh dấu **Quad PSRAM 120 MHz stable**, còn **Octal PSRAM 120 MHz experimental**. Do board hiện mới xác nhận dung lượng 8 MiB nhưng chưa xác nhận bus mode, production baseline vẫn là 80 MHz.

### 3.3 Release-candidate policy

`v6.1-rc1` là release candidate. Nếu dự án chủ động khóa ở RC này thì CI và dev environment phải pin đúng tag, và mỗi workaround liên quan IDF phải ghi rõ version. Không tự động chuyển sang `v6.1`, `release/6.1` hoặc `master` nếu chưa chạy lại toàn bộ acceptance test.

---

## 4. Kiến trúc bộ nhớ production

```text
ESP32-S3 Gateway
│
├── Internal SRAM 512 KiB
│   ├── BLE controller
│   ├── NimBLE Host
│   ├── Wi-Fi / lwIP critical working set
│   ├── FreeRTOS task stacks
│   ├── queue / mutex / semaphore / event group
│   ├── command executor / dispatcher control state
│   ├── BLE connection / ACK / session state
│   ├── request body nhỏ, ngắn hạn
│   └── safety headroom
│
├── PSRAM 8 MiB
│   ├── HTTP response buffer lớn nếu cần
│   ├── MCP serialization buffer lớn nếu cần
│   ├── application cache lớn
│   ├── future message queue/history không critical
│   ├── diagnostics snapshot lớn
│   └── feature data có thể degrade khi thiếu RAM
│
├── Flash 16 MiB
│   ├── NVS
│   ├── OTA metadata
│   ├── OTA slot A
│   ├── OTA slot B
│   ├── optional storage partition
│   └── coredump
│
└── RTC SRAM 16 KiB
    └── chỉ dùng cho breadcrumb/reset reason nhỏ nếu thật sự cần
```

### 4.1 Quy tắc bắt buộc

**Internal SRAM:**

- task stacks;
- NimBLE;
- Wi-Fi/lwIP critical allocations;
- BLE session/control objects;
- ACK state;
- queues nhỏ;
- DMA/internal-only allocations;
- buffer nhỏ dưới vài KiB trên đường nóng.

**PSRAM:**

- large cache;
- large response workspace;
- large diagnostics snapshot;
- dữ liệu application có thể tái tạo hoặc degrade;
- future queues không real-time.

**Flash/NVS:**

- chỉ dữ liệu persistent;
- không dùng flash làm queue tần suất cao;
- config/device metadata/bonding/OTA state.

---

# PHẦN A — IMPLEMENTATION PLAN

## 4A. Phase P-1 — Migration gate sang ESP-IDF v6.1-rc1

Phase này bắt buộc hoàn thành trước mọi memory tuning. Mục tiêu là tách lỗi migration API/build-system khỏi lỗi tối ưu memory.

### 4A.1 Pin toolchain

Cài/checkout chính xác:

```sh
git clone -b v6.1-rc1 --recursive https://github.com/espressif/esp-idf.git esp-idf-v6.1-rc1
cd esp-idf-v6.1-rc1
./install.sh esp32s3
. ./export.sh
idf.py --version
```

CI phải assert output/version tương ứng `v6.1-rc1`.

### 4A.2 Không tái sử dụng build artefact 5.4.4

Trong project gateway:

```sh
idf.py fullclean
rm -f sdkconfig
idf.py set-target esp32s3
idf.py reconfigure
idf.py build
```

`sdkconfig.defaults*` là input; generated `sdkconfig` phải được tạo lại bằng v6.1-rc1 để phát hiện symbol bị đổi/xóa.

### 4A.3 Component Manager

`main/idf_component.yml` hiện có `idf: ">=5.0"`. Target mới nên nâng compatibility floor lên prerelease 6.1 và vẫn pin exact toolchain ở CI/dev environment. Ví dụ:

```yaml
dependencies:
  idf: ">=6.1.0-rc1,<6.2.0"
  espressif/cjson: "1.7.19~2"
```

Sau đó chạy:

```sh
idf.py update-dependencies
idf.py reconfigure
```

Commit `dependencies.lock` mới chỉ sau khi build/test bằng đúng v6.1-rc1.

### 4A.4 Compile-migration checklist

Trước khi tối ưu memory, build phải sạch các vấn đề sau:

- header/API deprecated đã bị remove trong IDF 6.x;
- implicit FreeRTOS includes;
- dependency `esp_psram` cho component dùng `esp_psram.h`;
- không còn dependency vào built-in `json`;
- cJSON vẫn resolve qua Component Manager;
- unit-test app cũng build bằng v6.1-rc1, không chỉ firmware production.

### 4A.5 Baseline mới

Sau khi build thành công trên v6.1-rc1, tạo lại:

```sh
idf.py size
idf.py size-components
```

và runtime checkpoints M0–M4. Các số này trở thành **v6.1-rc1 baseline**. Không so trực tiếp absolute heap/stack với 5.4.4 để pass/fail; chỉ dùng để hiểu migration delta.

### 4A.6 Pass criteria

- `idf.py --version` xác nhận v6.1-rc1.
- production app build sạch.
- test app build sạch.
- `dependencies.lock` được resolve lại.
- không còn compile dependency ngầm từ 5.4.4.
- có size report mới trước khi bắt đầu P0.

## 5. Phase P0 — Sửa stack telemetry trước khi tuning

### 5.1 Vấn đề hiện tại

Trong `components/command_executor/command_executor.c`, code đang coi giá trị từ `uxTaskGetStackHighWaterMark()` như "words" rồi nhân với `sizeof(StackType_t)`.

Trên ESP32-S3 với ESP-IDF v6.1-rc1, `uxTaskGetStackHighWaterMark()` tiếp tục được dùng như **bytes** trong IDF port. Nếu nhân thêm, telemetry có thể báo stack margin lớn hơn thực tế.

### 5.2 Target change

File:

```text
components/command_executor/command_executor.c
```

Thay logic:

```c
UBaseType_t words = uxTaskGetStackHighWaterMark(task);
uint32_t bytes = (uint32_t)(words * sizeof(StackType_t));
```

bằng:

```c
UBaseType_t free_bytes = uxTaskGetStackHighWaterMark(task);
uint32_t bytes = (uint32_t)free_bytes;
```

### 5.3 Naming cleanup

Biến output nên đổi từ tên gây mơ hồ thành:

```c
uint32_t *worker_stack_min_free_bytes
```

Nếu public API đang dùng tên cũ, có thể giữ ABI nhưng comment phải ghi rõ đơn vị là bytes.

### 5.4 Test

1. Build unit test.
2. Chạy executor với 2 workers.
3. Gửi nhiều lệnh liên tục.
4. Đọc stack high-watermark.
5. Xác nhận metric giảm hợp lý khi workload tăng.

### 5.5 Pass criteria

- Không còn phép nhân `sizeof(StackType_t)` cho stack HWM.
- Metric được ghi/document là bytes.
- Worker stack high-watermark sau stress test phải còn ít nhất:

```text
>= 768 bytes: pass tối thiểu
>= 1024 bytes: khuyến nghị production
```

Nếu margin > 2048 bytes ổn định qua stress test, có thể xem xét giảm stack từng bước 512 bytes.

---

## 6. Phase P1 — Bật và xác minh PSRAM đúng phần cứng

### 6.1 Không giả định QUAD/OCT

Thông tin "8 MiB PSRAM" chưa đủ để xác định bus mode. Trước khi commit config production phải xác nhận module/board dùng:

```text
QUAD PSRAM
hoặc
OCTAL PSRAM
```

### 6.2 Baseline `sdkconfig.defaults`

Thêm:

```ini
# ==== External PSRAM / ESP-IDF v6.1-rc1 ====
CONFIG_SPIRAM=y
CONFIG_SPIRAM_BOOT_HW_INIT=y
CONFIG_SPIRAM_BOOT_INIT=y
CONFIG_SPIRAM_MEMTEST=y
CONFIG_SPIRAM_IGNORE_NOTFOUND=n
CONFIG_SPIRAM_PRE_CONFIGURE_MEMORY_PROTECTION=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_SPEED_80M=y

# Chọn đúng MỘT mode theo board/module thực tế:
# CONFIG_SPIRAM_MODE_QUAD=y
# CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_TYPE_AUTO=y

# Starting point để benchmark, không coi là immutable:
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=2048
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=49152
```

### 6.2A Dependency `esp_psram` trên IDF 6.1

Kconfig/API PSRAM chỉ nên được xem là available khi component `esp_psram` được đưa vào dependency graph. Mọi component gọi `esp_psram_is_initialized()` phải khai báo dependency trực tiếp thay vì dựa vào include gián tiếp.

Ví dụ cho `components/gateway_status/CMakeLists.txt`:

```cmake
idf_component_register(
    SRCS "gateway_status.c"
    INCLUDE_DIRS "include"
    REQUIRES device_store wifi_provisioning ble_central esp_wifi
             esp_app_format esp_timer esp_common
    PRIV_REQUIRES esp_system esp_psram
)
```

`components/memory_policy/CMakeLists.txt` cũng phải có `REQUIRES` hoặc `PRIV_REQUIRES esp_psram` nếu component trực tiếp include `esp_psram.h`.

### 6.2B 80 MHz baseline và track 120 MHz

- **Unknown PSRAM mode:** giữ 80 MHz.
- **QUAD đã xác minh:** v6.1-rc1 đánh dấu 120 MHz stable; có thể mở một optimization track riêng sau khi toàn bộ plan pass ở 80 MHz.
- **OCTAL:** 120 MHz vẫn là experimental trên v6.1-rc1; production baseline giữ 80 MHz. Không bật `IDF_EXPERIMENTAL_FEATURES` chỉ để lấy benchmark cao hơn.

Mọi chuyển 80 → 120 MHz phải chạy lại boot/reconnect/HTTP/OTA/thermal stress test và không được thay đổi cùng lúc với allocator threshold để tránh mất khả năng quy nguyên nhân.

### 6.3 Vì sao chọn 2 KiB / 48 KiB làm starting point

Đây chỉ là baseline để đo:

- allocation nhỏ `<= 2 KiB`: ưu tiên internal;
- allocation lớn hơn: allocator có xu hướng ưu tiên external;
- 48 KiB internal reserve tạo vùng đệm cho DMA/internal-only workload.

Phải benchmark thêm:

```text
ALWAYSINTERNAL: 1024 / 2048 / 4096
RESERVE_INTERNAL: 32 KiB / 48 KiB / 64 KiB
```

### 6.3A Optional optimization track — Wi-Fi/lwIP ưu tiên PSRAM

v6.1-rc1 có `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`, cho phép Wi-Fi/lwIP thử cấp phát từ PSRAM trước rồi fallback internal. **Không bật trong baseline đầu tiên.** Chỉ thử sau khi 9-link baseline đã pass để có dữ liệu A/B rõ ràng.

Experiment:

```ini
CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y
```

So sánh với baseline `n` ở cùng workload:

- M2/M8/M9 internal free + largest block;
- Wi-Fi throughput/latency;
- REST/MCP p95 latency;
- BLE reconnect stability khi Wi-Fi load cao;
- OTA/NVS operations;
- PSRAM fragmentation.

Chỉ enable production nếu internal SRAM headroom tăng có ý nghĩa và không tạo latency/stability regression. Không bật đồng thời với thay đổi PSRAM frequency hoặc allocator threshold trong cùng experiment.

`CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY` vẫn **disabled baseline**, vì nó có thể di chuyển BSS của networking/BT libraries sang external RAM rộng hơn phạm vi mong muốn và làm placement khó kiểm soát.

### 6.4 Runtime PSRAM verification

Thêm kiểm tra boot-time trong memory/status layer:

```c
#include "esp_heap_caps.h"
#include "esp_psram.h"

esp_err_t gw_memory_verify_psram(void)
{
    if (!esp_psram_is_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t free_psram = heap_caps_get_free_size(
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (free_psram == 0) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
```

### 6.5 Pass criteria

Tại boot production:

- `esp_psram_is_initialized() == true`.
- external heap > 0.
- allocation test 64 KiB bằng `MALLOC_CAP_SPIRAM` thành công.
- pointer test được free bình thường.
- firmware không boot vào gateway full mode nếu board được định nghĩa bắt buộc PSRAM mà PSRAM init fail.

### 6.6 Fail policy

Gateway này có phần cứng đã xác định 8 MiB PSRAM. Vì vậy production nên **fail closed** nếu PSRAM bị thiếu/hỏng thay vì âm thầm chạy full feature bằng internal SRAM.

Có thể chọn một trong hai policy:

```text
A. Safe mode: chỉ provisioning/status, không start BLE full workload.
B. Reboot/fault state với reason rõ ràng.
```

Không chạy đầy đủ Web + BLE + MCP rồi chờ OOM.

---

## 7. Phase P2 — Tạo `memory_policy` component

### 7.1 Mục tiêu

Không dùng `malloc()` ngẫu nhiên cho các allocation application lớn. Mỗi allocation quan trọng phải thể hiện semantic của nó.

### 7.2 Component mới

```text
components/memory_policy/
├── CMakeLists.txt
├── Kconfig.projbuild
├── include/
│   └── memory_policy.h
├── memory_policy.c
└── test/
    └── test_memory_policy.c
```

### 7.3 Public API đề xuất

```c
#ifndef MEMORY_POLICY_H
#define MEMORY_POLICY_H

#include <stddef.h>
#include "esp_err.h"

typedef enum {
    GW_MEM_INTERNAL_REQUIRED = 0,
    GW_MEM_EXTERNAL_REQUIRED,
    GW_MEM_EXTERNAL_PREFERRED,
    GW_MEM_DEFAULT,
} gw_mem_class_t;

void *gw_mem_alloc(size_t size, gw_mem_class_t mem_class);
void *gw_mem_calloc(size_t count, size_t size, gw_mem_class_t mem_class);
void gw_mem_free(void *ptr);

esp_err_t gw_memory_verify_psram(void);

#endif
```

### 7.4 Allocation semantics bắt buộc

#### INTERNAL_REQUIRED

```c
heap_caps_malloc(size,
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
```

Dùng cho:

- control state;
- BLE-related application state;
- queue storage nếu yêu cầu internal;
- DMA/internal-only buffer;
- object latency-sensitive.

#### EXTERNAL_REQUIRED

```c
heap_caps_malloc(size,
                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
```

Nếu fail: trả `NULL`.

**Không fallback internal.**

Dùng cho:

- cache lớn;
- diagnostics snapshot lớn;
- large optional response workspace;
- future message buffer không critical.

#### EXTERNAL_PREFERRED

Thử PSRAM trước. Chỉ fallback internal nếu toàn bộ điều kiện sau đúng:

```text
size <= CONFIG_GW_MEM_FALLBACK_MAX_BYTES
internal_free_after_estimate >= CONFIG_GW_MEM_INTERNAL_FLOOR_BYTES
largest_internal_block >= CONFIG_GW_MEM_INTERNAL_LARGEST_FLOOR_BYTES
```

Starting Kconfig:

```text
GW_MEM_FALLBACK_MAX_BYTES=2048
GW_MEM_INTERNAL_FLOOR_BYTES=65536
GW_MEM_INTERNAL_LARGEST_FLOOR_BYTES=32768
```

Pseudo-code:

```c
static bool can_fallback_internal(size_t size)
{
    if (size > CONFIG_GW_MEM_FALLBACK_MAX_BYTES) {
        return false;
    }

    size_t free_internal =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t largest_internal =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if (free_internal <= size) {
        return false;
    }

    if ((free_internal - size) < CONFIG_GW_MEM_INTERNAL_FLOOR_BYTES) {
        return false;
    }

    return largest_internal >=
           CONFIG_GW_MEM_INTERNAL_LARGEST_FLOOR_BYTES;
}
```

#### DEFAULT

Chỉ dùng khi allocation nhỏ, không critical và semantics không yêu cầu placement đặc biệt.

### 7.5 Quy tắc code review

Không được viết kiểu:

```c
void *p = malloc(128 * 1024);
```

cho application buffer lớn.

Phải là:

```c
void *p = gw_mem_alloc(128 * 1024,
                       GW_MEM_EXTERNAL_REQUIRED);
```

### 7.6 Tests

- INTERNAL_REQUIRED trả block có `MALLOC_CAP_INTERNAL`.
- EXTERNAL_REQUIRED trả block có `MALLOC_CAP_SPIRAM`.
- EXTERNAL_REQUIRED không fallback khi test gây external allocation failure.
- zero-size policy được định nghĩa rõ: trả `NULL` hoặc normalize; chọn một behavior và test.
- calloc overflow được kiểm tra trước `count * size` nếu tự implement wrapper.

---

## 8. Phase P3 — Ép NimBLE host dùng internal memory

### 8.1 Target config

Trong `sdkconfig.defaults`:

```ini
CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL=y
```

Giữ:

```ini
CONFIG_BT_NIMBLE_MAX_CONNECTIONS=9
CONFIG_BT_NIMBLE_ROLE_CENTRAL=y
CONFIG_BT_NIMBLE_ROLE_OBSERVER=y
CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=n
CONFIG_BT_NIMBLE_ROLE_BROADCASTER=n
CONFIG_BT_NIMBLE_GATT_CLIENT=y
CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=256
```

Trong `sdkconfig.defaults.esp32s3`:

```ini
CONFIG_BT_CTRL_BLE_MAX_ACT=10
```

### 8.2 Không dùng external NimBLE allocator ở baseline

Mặc dù ESP-IDF có mode allocate NimBLE từ external SPIRAM, tài liệu này chọn internal vì gateway cần:

- latency ổn định;
- giảm phụ thuộc cache/PSRAM cho BLE core path;
- predictable failure behavior;
- dễ benchmark 9 links.

### 8.3 Không tăng `MAX_CONNECTIONS`

Không sửa ESP-IDF hoặc Kconfig để vượt 9 trong phase này.

Nếu sản phẩm sau này cần >9 thiết bị active:

```text
Option 1: registered 16, active <=9, connect-on-demand
Option 2: connection scheduling
Option 3: multi-gateway
Option 4: đổi target/controller sau benchmark độc lập
```

### 8.4 Pass criteria

Stress 9 links phải đạt:

- tất cả link READY;
- scan/reconnect vẫn hoạt động;
- không OOM;
- không reboot/WDT;
- internal heap minimum không xuống dưới hard floor;
- command ACK vẫn trong timeout budget.

---

## 9. Phase P4 — Runtime memory telemetry

### 9.1 Vấn đề hiện tại

`gateway_status` chỉ có:

```c
status->free_heap = esp_get_free_heap_size();
```

Metric này gộp nhiều loại heap nên không đủ để quyết định SRAM/PSRAM health.

### 9.2 Target fields

Mở rộng `gateway_status_t`:

```c
uint32_t free_heap;

uint32_t internal_free;
uint32_t internal_min_free;
uint32_t internal_largest_free_block;

uint32_t psram_free;
uint32_t psram_min_free;
uint32_t psram_largest_free_block;

bool psram_ready;
```

### 9.3 Implementation

Trong `gateway_status.c`:

```c
#include "esp_heap_caps.h"
#include "esp_psram.h"

status->internal_free = (uint32_t)heap_caps_get_free_size(
    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

status->internal_min_free =
    (uint32_t)heap_caps_get_minimum_free_size(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

status->internal_largest_free_block =
    (uint32_t)heap_caps_get_largest_free_block(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

status->psram_ready = esp_psram_is_initialized();

if (status->psram_ready) {
    status->psram_free = (uint32_t)heap_caps_get_free_size(
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    status->psram_min_free =
        (uint32_t)heap_caps_get_minimum_free_size(
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    status->psram_largest_free_block =
        (uint32_t)heap_caps_get_largest_free_block(
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
```

### 9.4 Không tạo task monitor riêng ở baseline

Không cần task polling memory mỗi giây. Chỉ snapshot khi:

- `/api/status` được gọi;
- dispatcher `get_status` được gọi;
- MCP status được gọi;
- stress test thu thập checkpoint.

Mục tiêu là telemetry có chi phí thấp.

### 9.5 Runtime thresholds

Starting operational policy:

```text
internal_free >= 64 KiB                  hard floor
internal_free >= 80 KiB                  preferred
internal_largest_free_block >= 24 KiB    hard floor
internal_largest_free_block >= 32 KiB    preferred
PSRAM allocation failures = 0            required
```

Các số này là acceptance baseline ban đầu; sau test thực tế có thể hiệu chỉnh nhưng không được bỏ metric.

### 9.6 Fragmentation warning

Không chỉ nhìn `internal_free`.

Ví dụ:

```text
internal_free = 90 KiB
largest block = 12 KiB
```

vẫn phải coi là degraded vì fragmentation.

### 9.7 API compatibility

Giữ `free_heap` hiện tại để không phá UI/client cũ. Các field mới additive.

---

## 10. Phase P5 — HTTP/cJSON memory strategy

### 10.1 Giữ request body nhỏ trên stack

Các giới hạn hiện tại:

```c
#define WEB_DEVICE_BODY_MAX_LEN   512
#define WEB_COMMAND_BODY_MAX_LEN 1024
#define WEB_WIFI_BODY_MAX_LEN     256
```

được xem là hợp lý.

Không cần chuyển các buffer 256–1024 bytes này sang PSRAM. Đây là dữ liệu ngắn hạn, có giới hạn và thuận lợi cho deterministic behavior.

### 10.2 Vấn đề cần tránh

Current response path có dạng:

```c
cJSON *json = ...;
char *text = cJSON_PrintUnformatted(json);
cJSON_Delete(json);
httpd_resp_send(request, text, HTTPD_RESP_USE_STRLEN);
cJSON_free(text);
```

Peak memory có thể bao gồm đồng thời:

```text
cJSON tree
+
serialized JSON output
```

Với payload nhỏ: chấp nhận được.

Với payload lớn: không được mở rộng vô hạn theo pattern này.

### 10.3 Quy tắc endpoint

#### Small response

Nếu serialized output dự kiến < 4 KiB:

- giữ cJSON hiện tại;
- không tối ưu sớm.

#### Medium response

Khoảng 4–16 KiB:

- đo peak memory;
- ưu tiên PSRAM workspace nếu cần;
- không tăng response size vô hạn.

#### Large/unbounded response

>16 KiB hoặc phụ thuộc số record/history:

- dùng `httpd_resp_send_chunk()`;
- serialize từng item;
- không dựng full cJSON tree rồi print toàn bộ.

### 10.4 Không đổi global cJSON hooks ở phase đầu

Không dùng `cJSON_InitHooks()` để ép toàn bộ allocation sang PSRAM trong phase này, vì cJSON dùng nhiều allocation nhỏ và global hooks có thể làm behavior của toàn bộ firmware khó dự đoán.

Ưu tiên:

```text
bounded JSON tree
+
streaming cho response lớn
```

### 10.5 Helper đề xuất cho streaming JSON array

Ví dụ pattern:

```c
httpd_resp_set_type(request, "application/json");
httpd_resp_send_chunk(request, "[", 1);

for (size_t i = 0; i < count; i++) {
    if (i > 0) {
        httpd_resp_send_chunk(request, ",", 1);
    }

    char item[512];
    int n = serialize_item(item, sizeof(item), &items[i]);
    if (n < 0) {
        // Abort/close according to API error policy.
        break;
    }

    httpd_resp_send_chunk(request, item, n);
}

httpd_resp_send_chunk(request, "]", 1);
httpd_resp_send_chunk(request, NULL, 0);
```

Nếu item có thể > stack buffer budget thì dùng PSRAM `EXTERNAL_REQUIRED` buffer bounded.

### 10.6 Async command context

`command_async_context_t` hiện nhỏ và `malloc(sizeof(*context))` là acceptable. Không cần ép nó sang PSRAM.

Nguyên tắc:

```text
small control object → internal/default
large payload → explicit external
```

### 10.7 Pass criteria

- Không endpoint nào nhận body không bounded.
- Không có response path có kích thước unbounded mà dùng full-tree + full-print.
- 10–20 request đồng thời trong stress test không làm internal heap xuống dưới floor.
- HTTP timeout/503 behavior vẫn giữ nguyên khi executor full.

---

## 11. Phase P6 — Tối ưu `command_executor`

### 11.1 Baseline giữ nguyên

```text
CONFIG_CMD_EXEC_WORKER_COUNT=2
CONFIG_CMD_EXEC_QUEUE_LEN=2
CONFIG_CMD_EXEC_WORKER_STACK=4096
CONFIG_CMD_EXEC_JOB_TIMEOUT_MS=3000
```

Không tăng worker count chỉ vì PSRAM dư.

### 11.2 Vì sao

Mỗi worker tăng:

- task stack internal;
- worker persistent state;
- dispatch result storage;
- concurrency tới BLE ACK path;
- contention trong dispatcher/device state.

### 11.3 Tuning sequence

Do IDF 6.x mặc định dùng PicolibC và có thay đổi placement của FreeRTOS functions, số đo stack từ firmware 5.4.4 không được dùng để quyết định stack size mới. Phải đo lại trên v6.1-rc1.

Sau khi fix stack telemetry:

```text
4096 → stress
3584 → stress
3072 → stress
```

Chỉ giảm khi:

```text
min stack free >= 1024 bytes
```

ở toàn bộ workload:

- REST command;
- MCP command;
- device timeout;
- BLE disconnect/reconnect;
- error response;
- capability refresh.

Không giảm dưới 3072 trong phase đầu nếu chưa có bằng chứng thực tế.

### 11.4 Worker count

Chỉ tăng từ 2 → 3 nếu benchmark chứng minh:

- queue saturation thường xuyên;
- BLE/dispatcher hỗ trợ concurrency an toàn;
- memory floor vẫn pass.

Default production vẫn là 2.

---

## 12. Phase P7 — FreeRTOS task stack audit

### 12.1 Tất cả task phải có high-watermark measurement

Tối thiểu audit:

```text
NimBLE host task
HTTP server task
command executor workers
Wi-Fi provisioning worker
DNS/captive worker nếu có
BLE reconnect/supervisor task nếu riêng
main task trong boot path
```

### 12.2 Quy tắc tuning

Không giảm stack theo cảm tính.

Chỉ giảm nếu:

```text
observed minimum free >= 1.5 KiB
```

sau stress test đầy đủ.

Mỗi lần giảm tối đa 512 bytes rồi test lại.

### 12.3 Không đặt stack external ở baseline

Không bật external task stack chỉ để tiết kiệm SRAM. IDF 6.1 có hỗ trợ explicit/global PSRAM task stack, nhưng flash/NVS/OTA paths có cache-off restrictions; gateway baseline vẫn giữ stack internal.

Task stack phải internal trừ khi có một RFC/test riêng chứng minh an toàn.

### 12.4 Không bật `CONFIG_FREERTOS_IN_IRAM` mặc định

Từ IDF 6.0, đa số FreeRTOS functions mặc định chuyển từ IRAM sang Flash để giảm IRAM usage. Giữ default này cho gateway. Chỉ bật `CONFIG_FREERTOS_IN_IRAM=y` nếu latency benchmark chứng minh cần thiết và IRAM budget vẫn pass. Đây là optimization khác với SRAM/PSRAM allocation và phải được benchmark độc lập.

---

# PHẦN B — FLASH / OTA

## 13. Phase P8 — Chuyển Flash 16 MiB sang custom OTA partition

### 13.1 Bỏ single-app partition

Không giữ:

```ini
CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y
```

Thay bằng custom partition:

```ini
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
```

### 13.2 `partitions.csv` đề xuất

```csv
# Name,     Type, SubType, Offset, Size,     Flags
nvs,        data, nvs,     ,      128K,
otadata,    data, ota,     ,      8K,
phy_init,   data, phy,     ,      4K,
nvs_keys,   data, nvs_keys,,      4K,
ota_0,      app,  ota_0,   ,      5M,
ota_1,      app,  ota_1,   ,      5M,
storage,    data, spiffs,  ,      0x580000,
coredump,   data, coredump,,      256K,
```

Để offset trống để partition generator tự căn chỉnh.

### 13.3 Ý nghĩa layout

```text
NVS               128 KiB
OTA metadata         8 KiB
PHY                   4 KiB
NVS keys              4 KiB
OTA A                 5 MiB
OTA B                 5 MiB
Storage             5.5 MiB
Coredump            256 KiB
Remaining/alignment ~64 KiB
```

### 13.4 Storage partition là optional feature

Nếu firmware không cần SPIFFS/LittleFS, có thể:

- giữ partition làm future reserve;
- hoặc đổi subtype/filesystem khi implementation thật xuất hiện.

Không bắt buộc mount storage chỉ vì partition tồn tại.

### 13.5 NVS 128 KiB

128 KiB là baseline hợp lý cho:

- Wi-Fi credentials;
- Device Store;
- BLE bonding;
- capability metadata;
- schema migration;
- future config.

Không dùng NVS để ghi event tần suất cao.

### 13.6 App slot acceptance

Mỗi OTA slot 5 MiB.

CI phải fail nếu binary app vượt:

```text
4 MiB (80% slot)
```

Mục tiêu giữ ít nhất 20% growth headroom.

---

## 14. Phase P9 — OTA rollback thực sự

### 14.1 Bật bootloader rollback

```ini
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
```

### 14.2 First-boot validation flow

Firmware mới sau OTA phải kiểm tra state:

```c
const esp_partition_t *running = esp_ota_get_running_partition();
esp_ota_img_states_t state;

if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
    state == ESP_OTA_IMG_PENDING_VERIFY) {

    bool ok = gateway_post_ota_self_test();

    if (ok) {
        esp_ota_mark_app_valid_cancel_rollback();
    } else {
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
}
```

### 14.3 Self-test tối thiểu

`gateway_post_ota_self_test()` phải kiểm tra:

1. PSRAM init.
2. internal/external heap query hoạt động.
3. NVS init.
4. Device Store schema load.
5. Wi-Fi subsystem init được.
6. NimBLE init được.
7. HTTP server init được.

Không yêu cầu phải kết nối đủ 9 BLE devices trước khi mark valid; OTA validation cần bounded time.

### 14.4 Timeout

Self-test phải có timeout tổng rõ ràng, ví dụ 10–20 giây tùy Wi-Fi behavior.

Không để firmware ở `PENDING_VERIFY` vô hạn.

### 14.5 Fail policy

Bất kỳ lỗi critical nào trong self-test:

```text
mark invalid
→ reboot
→ rollback
```

Optional external service không được làm OTA rollback nếu không phải boot-critical.

---

# PHẦN C — MEMORY MEASUREMENT

## 15. Không dùng subsystem estimate làm source of truth

Không dùng bảng kiểu:

```text
Wi-Fi = X KiB
BLE = Y KiB
HTTP = Z KiB
```

để quyết định production.

Dùng checkpoint measurement.

### 15.1 Checkpoints bắt buộc

```text
M0  app start / before subsystem init
M1  after NVS/device store
M2  after Wi-Fi init/connected
M3  after HTTP server
M4  after NimBLE init, 0 links
M5  1 BLE link READY
M6  5 BLE links READY
M7  8 BLE links READY
M8  9 BLE links READY
M9  9 links + HTTP/MCP concurrent load
M10 after disconnect/reconnect churn
```

### 15.2 Snapshot fields

Mỗi checkpoint lưu:

```text
internal_free
internal_min_free
internal_largest_free_block
psram_free
psram_min_free
psram_largest_free_block
free_heap total
worker stack min free
BLE active link count
```

### 15.3 Delta analysis

Ví dụ:

```text
Wi-Fi delta      = M1.internal_free - M2.internal_free
HTTP delta       = M2.internal_free - M3.internal_free
NimBLE base      = M3.internal_free - M4.internal_free
BLE per-link avg = (M4.internal_free - M8.internal_free) / 9
```

Không coi delta tuyến tính tuyệt đối; dùng nó để nhận diện regression giữa commit.

### 15.4 Regression guard

Nếu một commit làm:

```text
M8 internal minimum giảm > 10 KiB
```

mà không có feature justification, CI/review phải yêu cầu giải thích.

---

## 16. Production memory thresholds

### 16.1 Hard fail conditions

Không release nếu bất kỳ stress test nào có:

```text
internal_free < 64 KiB
internal_largest_free_block < 24 KiB
PSRAM allocation failure trên required path
OOM
abort
WDT
heap corruption
stack overflow
```

### 16.2 Preferred production condition

```text
internal_free >= 80 KiB
internal_min_free >= 64 KiB
internal_largest_free_block >= 32 KiB
worker stack min free >= 1 KiB
PSRAM free >= 20% physical PSRAM
```

Không cần bắt buộc PSRAM free >5 MiB. PSRAM được phép sử dụng nhiều hơn trong tương lai miễn là không fragmentation/OOM và vẫn có headroom.

---

# PHẦN D — `sdkconfig.defaults` TARGET

## 17. Baseline production config đề xuất

Đây là **template**, phải merge với config hiện tại thay vì copy mù quáng.

```ini
# ============================================================
# Target / flash
# ============================================================
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"

# ============================================================
# PSRAM
# ============================================================
CONFIG_SPIRAM=y
CONFIG_SPIRAM_BOOT_HW_INIT=y
CONFIG_SPIRAM_BOOT_INIT=y
CONFIG_SPIRAM_MEMTEST=y
CONFIG_SPIRAM_IGNORE_NOTFOUND=n
CONFIG_SPIRAM_PRE_CONFIGURE_MEMORY_PROTECTION=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_TYPE_AUTO=y

# Select according to actual board hardware:
# CONFIG_SPIRAM_MODE_QUAD=y
# CONFIG_SPIRAM_MODE_OCT=y

# Initial tuning values; benchmark before freezing production.
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=2048
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=49152

# Optional A/B experiment only after baseline passes:
# CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP is not set
# CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY is not set

# Do not move task stacks externally in baseline.
# Keep external stack option disabled/not used.

# ============================================================
# NimBLE
# ============================================================
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL=y
CONFIG_BT_NIMBLE_MAX_CONNECTIONS=9
CONFIG_BT_NIMBLE_ROLE_CENTRAL=y
CONFIG_BT_NIMBLE_ROLE_OBSERVER=y
CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=n
CONFIG_BT_NIMBLE_ROLE_BROADCASTER=n
CONFIG_BT_NIMBLE_GATT_CLIENT=y
CONFIG_BT_NIMBLE_SECURITY_ENABLE=y
CONFIG_BT_NIMBLE_SM_LEGACY=y
CONFIG_BT_NIMBLE_SM_SC=y
CONFIG_BT_NIMBLE_NVS_PERSIST=y
CONFIG_BT_NIMBLE_MAX_BONDS=16
CONFIG_BT_NIMBLE_MAX_CCCDS=16
CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=256

# ============================================================
# OTA safety
# ============================================================
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y

# ============================================================
# Wi-Fi / HTTP
# ============================================================
CONFIG_ESP_WIFI_SOFTAP_SUPPORT=y
CONFIG_HTTPD_WS_SUPPORT=n

# ============================================================
# RTOS
# ============================================================
CONFIG_FREERTOS_HZ=1000
```

Và `sdkconfig.defaults.esp32s3`:

```ini
CONFIG_BT_CTRL_BLE_MAX_ACT=10
```

### 17.1 Không bật tự động

Không bật chỉ để "tối ưu":

```text
PSRAM 120 MHz khi chưa xác nhận mode/benchmark (QUAD có track riêng; OCTAL vẫn experimental)
external task stack
NimBLE external allocator
large global BSS external relocation
BLE >9 connections
```

mà chưa có benchmark riêng.

---

# PHẦN E — HTTP / APPLICATION ALLOCATION RULES

## 18. Allocation matrix

| Data/object | Memory class | Lý do |
|---|---|---|
| BLE connection state | INTERNAL_REQUIRED | control path |
| ACK/session state | INTERNAL_REQUIRED | latency-sensitive |
| FreeRTOS task stack | Internal | baseline policy |
| queue/mutex object | Internal/default | nhỏ, control |
| HTTP body 256–1024 B | stack/internal | bounded, short-lived |
| async HTTP context nhỏ | default/internal | control object |
| CBOR frame nhỏ | internal/default | BLE working set |
| large HTTP output | EXTERNAL_REQUIRED/PREFERRED | tránh SRAM peak |
| diagnostics snapshot lớn | EXTERNAL_REQUIRED | non-critical |
| future cache >4 KiB | EXTERNAL_REQUIRED | bulk data |
| NVS metadata | Flash/NVS | persistent |
| OTA image | Flash OTA slot | persistent firmware |

---

## 19. Degraded-mode policy

Khi optional PSRAM allocation fail:

**Được phép:**

- trả `ESP_ERR_NO_MEM`;
- trả HTTP 503/507 tùy semantic;
- giảm cache;
- bỏ diagnostics snapshot;
- tắt optional feature tạm thời.

**Không được phép:**

- allocate lại 100–500 KiB từ internal SRAM;
- loop retry vô hạn;
- reboot vì optional cache thiếu memory;
- làm BLE/Wi-Fi mất memory headroom.

---

# PHẦN F — TEST PLAN

## 20. Build-time verification

Chạy trong environment ESP-IDF v6.1-rc1:

```sh
idf.py --version
idf.py set-target esp32s3
idf.py update-dependencies
idf.py reconfigure
idf.py build
idf.py size
idf.py size-components
```

Bắt buộc kiểm tra:

- `idf.py --version` đúng v6.1-rc1;
- flash detected/build configured 16 MiB;
- partition CSV hợp lệ;
- app binary < 4 MiB;
- PSRAM config hiện trong generated sdkconfig;
- component dùng `esp_psram.h` khai báo dependency `esp_psram`;
- NimBLE memory mode = internal;
- NimBLE max connections = 9;
- controller max activity = 10;
- rollback enabled.

---

## 21. Boot test

### 21.1 Không có credentials

Expected:

- NVS init;
- PSRAM ready;
- provisioning mode chạy;
- không init full BLE gateway nếu current architecture vẫn tách provisioning boot;
- `/api/status` trả memory fields.

### 21.2 Có credentials

Expected:

- Wi-Fi connect;
- gateway modules init;
- PSRAM ready;
- NimBLE ready;
- HTTP ready;
- internal heap pass thresholds.

---

## 22. BLE scaling test

Chạy tuần tự:

```text
1 device
5 devices
8 devices
9 devices
```

Tại mỗi mức:

- chờ tất cả READY;
- chạy BLE scan nếu architecture cho phép;
- gửi command round-robin;
- refresh capabilities;
- disconnect/reconnect một thiết bị;
- thu Mx snapshot.

Không có test 10 simultaneous links trong production acceptance.

---

## 23. HTTP/MCP concurrency test

Tại 9 BLE links:

- poll `/api/status`;
- poll `/api/devices`;
- gọi MCP `list_tools`;
- gửi REST commands;
- gửi MCP commands;
- cố tình saturate executor queue;
- verify 503/409/timeout behavior;
- theo dõi internal min/largest block.

Pass:

```text
No OOM
No WDT
No heap corruption
No stack overflow
Internal floor maintained
BLE links remain stable
```

---

## 24. Reconnect churn test

Trong tối thiểu 30–60 phút:

1. giữ Wi-Fi active;
2. chạy 8–9 BLE links;
3. power-cycle peripheral luân phiên;
4. để gateway reconnect;
5. gửi command trong lúc reconnect;
6. poll status liên tục vừa phải;
7. kiểm tra min heap không trượt dần theo thời gian.

Nếu `internal_free` giảm đều sau mỗi reconnect cycle → nghi memory leak.

---

## 25. Memory fragmentation test

Workload:

- nhiều request JSON nhỏ;
- command timeout;
- reconnect;
- capability refresh;
- BLE scan;
- allocate/free optional external buffers khác size.

Theo dõi:

```text
internal_free
internal_largest_free_block
psram_free
psram_largest_free_block
```

Pass nếu sau workload dài:

- free heap gần baseline ổn định;
- largest block không giảm đơn điệu;
- không xuất hiện allocation failure bất thường.

---

## 26. OTA test

### 26.1 Happy path

- build version A;
- OTA version B vào inactive slot;
- reboot;
- self-test B pass;
- mark valid;
- reboot thêm lần nữa;
- vẫn boot B.

### 26.2 Rollback path

Tạo test firmware B fail self-test có kiểm soát:

- OTA B;
- boot B;
- mark invalid;
- reboot;
- phải quay lại A.

### 26.3 Power-loss path

Sau boot image mới ở `PENDING_VERIFY`, reset/power-cycle trước khi mark valid.

Expected: bootloader rollback theo policy.

---

# PHẦN G — CI / REVIEW GUARDS

## 27. CI checks đề xuất

CI phải parse/kiểm tra ít nhất:

1. exact ESP-IDF version = v6.1-rc1;
2. firmware image size;
3. partition layout;
4. compile warnings;
5. unit tests;
6. forbidden config regression;
7. dependency lock được resolve bằng toolchain target.

### 27.1 Forbidden regression examples

Fail/review-required nếu thấy:

```text
CONFIG_BT_NIMBLE_MAX_CONNECTIONS > 9
CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y
CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=n
```

trừ khi có RFC riêng.

### 27.2 Source review rule

Search code cho allocation lớn:

```sh
grep -R "malloc(" components main

grep -R "calloc(" components main
```

Bất kỳ allocation > vài KiB hoặc size runtime không bounded phải được review memory class.

---

# PHẦN H — FILE-BY-FILE CHANGE LIST

## 27A. `main/idf_component.yml` + `dependencies.lock`

**Change:**

- nâng IDF compatibility floor lên prerelease 6.1;
- giữ `espressif/cjson` vì built-in JSON component không còn trong IDF 6.x;
- regenerate `dependencies.lock` bằng v6.1-rc1.

**Verify:** clean configure/build không resolve built-in `json`, cJSON version đúng lock file.

## 28. `sdkconfig.defaults`

**Change:**

- enable PSRAM;
- choose actual PSRAM mode;
- 80 MHz baseline;
- starting allocator thresholds;
- NimBLE internal allocator;
- custom partition;
- rollback.

**Verify:** generated `sdkconfig`.

---

## 29. `sdkconfig.defaults.esp32s3`

Giữ:

```ini
CONFIG_BT_CTRL_BLE_MAX_ACT=10
```

Không tăng chỉ để thử 10 links.

---

## 30. `partitions.csv`

**New file.**

Dùng layout OTA A/B ở mục 13.

---

## 31. `components/command_executor/command_executor.c`

**Change:**

- sửa stack HWM unit;
- giữ worker count 2;
- giữ queue 2 ban đầu;
- expose metric đúng.

**Verify:** stress command path.

---

## 32. `components/memory_policy/*`

**New component.**

**Change:**

- implement allocation classes;
- PSRAM verify;
- tests.

---

## 33. `components/gateway_status/include/gateway_status.h`

**Change:** thêm internal/PSRAM telemetry fields.

Không xóa `free_heap` để giữ compatibility.

---

## 34. `components/gateway_status/gateway_status.c` + `CMakeLists.txt`

**Change:** dùng `heap_caps_get_*` và `esp_psram_is_initialized()`; thêm `esp_psram` vào `PRIV_REQUIRES`/`REQUIRES` vì IDF 6.1 yêu cầu component dependency rõ ràng cho PSRAM API/config.

**Verify:** REST/MCP trả số liệu đúng.

---

## 35. `components/web_server/*`

**Change ngay:**

- không cần đổi request body buffer hiện tại;
- kiểm tra endpoint nào có response có khả năng lớn/unbounded.

**Change khi cần:**

- streaming chunked JSON cho endpoint lớn;
- external buffer cho payload lớn.

Không đổi global cJSON allocator ở phase đầu.

---

## 36. OTA boot integration

Vị trí có thể là `main/` hoặc component riêng `ota_manager`.

Yêu cầu:

```text
read OTA state
→ if pending verify
→ run bounded self-test
→ valid OR rollback
```

Không để logic này phân tán giữa Web/dispatcher.

---

# PHẦN I — EXECUTION ORDER CHO AI AGENT

## 37. Thứ tự bắt buộc

AI agent nên triển khai theo đúng thứ tự:

### Step 1 — Migrate/pin ESP-IDF v6.1-rc1

- pin exact tag;
- fullclean/reconfigure;
- update component dependencies/lock;
- build production + test app;
- resolve IDF 6.x API/build changes.

### Step 2 — New v6.1-rc1 baseline snapshot

- record commit SHA + `idf.py --version`;
- chạy `idf.py size` / `size-components`;
- lưu M0–M4 runtime baseline nếu có board.

### Step 3 — Fix stack metric

- sửa `command_executor`;
- unit test/build.

### Step 4 — Enable PSRAM

- xác minh QUAD/OCT hardware;
- update sdkconfig;
- boot verification.

### Step 5 — Add memory policy

- component + tests;
- chưa refactor tất cả allocation một lần;
- chỉ migrate các large application allocations rõ ràng.

### Step 6 — Lock NimBLE policy

- internal allocator;
- 9 links;
- activity 10.

### Step 7 — Add runtime telemetry

- gateway status fields;
- REST/MCP visible;
- snapshot M0–M4.

### Step 8 — HTTP/cJSON audit

- giữ small path;
- sửa only large/unbounded responses.

### Step 9 — Stack tuning

- chỉ sau khi HWM đúng;
- giảm từng 512 B nếu có headroom.

### Step 10 — Partition OTA A/B

- add `partitions.csv`;
- verify app slot size.

### Step 11 — Rollback implementation

- boot validation;
- happy/fail/power-loss tests.

### Step 12 — Full stress

- 1/5/8/9 links;
- HTTP/MCP concurrency;
- reconnect churn;
- fragmentation test.

### Step 13 — Freeze production thresholds

Sau khi có measurement thực, commit:

- chosen `ALWAYSINTERNAL`;
- chosen `RESERVE_INTERNAL`;
- actual stack sizes;
- final hard memory thresholds.

---

# PHẦN J — ACCEPTANCE CHECKLIST

## 38. Production readiness

### Toolchain / migration

- [ ] CI/dev environment pin đúng ESP-IDF `v6.1-rc1`.
- [ ] Production app và unit-test app build sạch trên v6.1-rc1.
- [ ] `dependencies.lock` được regenerate bằng toolchain target.
- [ ] Không phụ thuộc built-in `json`; cJSON dùng Component Manager.
- [ ] Component dùng `esp_psram.h` khai báo dependency `esp_psram`.
- [ ] Baseline size/heap/stack đã đo lại sau migration.

### Memory

- [ ] PSRAM được detect đúng 8 MiB-class hardware và init thành công.
- [ ] PSRAM mode QUAD/OCT đã xác minh từ board/module, không đoán.
- [ ] NimBLE Host dùng internal allocator.
- [ ] Không có large optional buffer fallback âm thầm sang internal SRAM.
- [ ] `gateway_status` expose internal free/min/largest.
- [ ] `gateway_status` expose PSRAM free/min/largest.
- [ ] Internal free không xuống dưới 64 KiB trong stress test.
- [ ] Largest internal block không xuống dưới 24 KiB.
- [ ] Không heap corruption/OOM.

### BLE

- [ ] `CONFIG_BT_NIMBLE_MAX_CONNECTIONS=9`.
- [ ] `CONFIG_BT_CTRL_BLE_MAX_ACT=10`.
- [ ] 9 devices READY ổn định.
- [ ] reconnect hoạt động dưới HTTP load.
- [ ] scan/reconnect không phá memory floor.

### FreeRTOS

- [ ] Stack HWM unit đã sửa thành bytes.
- [ ] Executor worker HWM >= 1 KiB preferred.
- [ ] Không stack overflow.
- [ ] Không external task stack ở baseline.

### HTTP/MCP

- [ ] Request body đều bounded.
- [ ] Không response unbounded dùng full cJSON tree + full print.
- [ ] Queue full trả lỗi có kiểm soát.
- [ ] 9 BLE links + concurrent HTTP/MCP vẫn pass memory floor.

### Flash/OTA

- [ ] Flash config 16 MiB.
- [ ] custom partition table được dùng.
- [ ] OTA A = 5 MiB.
- [ ] OTA B = 5 MiB.
- [ ] App binary < 4 MiB.
- [ ] rollback bootloader enabled.
- [ ] first boot OTA self-test có timeout.
- [ ] happy-path OTA pass.
- [ ] forced-fail rollback pass.
- [ ] power-loss rollback pass.

---

# PHẦN K — CÁC QUYẾT ĐỊNH KHÔNG ĐƯỢC TỰ Ý THAY ĐỔI

## 39. Architecture guardrails

Không thay các điểm sau mà không có benchmark/RFC:

1. Không tăng BLE simultaneous links trên 9.
2. Không chuyển NimBLE sang PSRAM.
3. Không chuyển FreeRTOS task stacks sang PSRAM.
4. Không bật PSRAM 120 MHz production khi chưa xác nhận PSRAM mode và chạy track benchmark riêng; với OCTAL trên v6.1-rc1 vẫn coi là experimental.
5. Không dùng large `malloc()` không có memory class.
6. Không làm cJSON global allocator change mà chưa test toàn firmware.
7. Không bỏ minimum/largest heap telemetry và chỉ giữ total free heap.
8. Không quay lại single-app partition.
9. Không gọi OTA A/B là "rollback-safe" nếu chưa bật và test rollback.
10. Không dùng PSRAM free >5 MiB làm KPI; KPI là stability, fragmentation và headroom.

---

# PHẦN L — KẾT QUẢ MONG ĐỢI SAU TRIỂN KHAI

Sau khi hoàn thành toàn bộ plan, gateway có kiến trúc tài nguyên như sau:

```text
ESP32-S3 / 16 MiB Flash / 8 MiB PSRAM
│
├── BLE Central
│   ├── 9 simultaneous links max
│   ├── NimBLE internal allocator
│   └── controller activity budget 10
│
├── Internal SRAM
│   ├── BLE/Wi-Fi control path
│   ├── all production task stacks
│   ├── command executor
│   ├── dispatcher/ACK state
│   └── >=64 KiB hard runtime floor
│
├── PSRAM
│   ├── application bulk allocations
│   ├── large response/cache workspace
│   └── no silent required→internal fallback
│
├── HTTP/MCP
│   ├── bounded request bodies
│   ├── small cJSON path unchanged
│   └── streaming for large responses
│
├── Telemetry
│   ├── internal current/min/largest
│   ├── PSRAM current/min/largest
│   └── stack min-free bytes
│
└── Flash
    ├── 128 KiB NVS
    ├── OTA metadata
    ├── 5 MiB OTA A
    ├── 5 MiB OTA B
    ├── ~5.5 MiB storage reserve
    ├── 256 KiB coredump
    └── boot rollback validation
```

---

## 40. Definition of Done

Plan được coi là hoàn tất khi:

1. Firmware và test app build sạch trên ESP-IDF v6.1-rc1 / ESP32-S3, với CI pin exact tag.
2. Component dependencies/lock đã migrate cho IDF 6.1-rc1 (`esp_psram`, external cJSON).
3. PSRAM đúng hardware và được verify ở runtime.
4. Stack telemetry được sửa.
5. Memory policy tồn tại và large application allocations tuân theo policy.
6. Gateway status expose đầy đủ memory telemetry.
7. NimBLE = internal, max links = 9.
8. HTTP/MCP không có unbounded full-tree response path.
9. 9 BLE links + Wi-Fi + REST/MCP stress pass.
10. Internal memory floors pass.
11. OTA A/B partition pass.
12. OTA rollback pass cả happy/fail/power-loss cases.
13. Các giá trị tuning cuối cùng được ghi lại từ benchmark thật, không chỉ từ estimate.
---

## 41. Tài liệu tham chiếu kỹ thuật

Baseline code và target toolchain được đối chiếu với:

- `hailp-vn38/esp-ble-gateway` commit `5baf93517f448144958e612531b5752652f49965`.
- `sdkconfig.defaults` của dự án.
- `sdkconfig.defaults.esp32s3` của dự án.
- `components/command_executor/command_executor.c`.
- `components/command_executor/Kconfig.projbuild`.
- `components/gateway_status/gateway_status.c`.
- `components/gateway_status/include/gateway_status.h`.
- `components/web_server/web_http.c` / `web_http.h`.
- ESP-IDF v6.1-rc1 NimBLE Kconfig: internal/external/default allocator mode; ESP32-S3 max concurrent connection range vẫn tối đa 9.
- ESP-IDF v6.1-rc1 ESP32-S3 controller Kconfig: `BT_CTRL_BLE_MAX_ACT` range 1–10; mỗi activity được mô tả khoảng 828 B.
- ESP-IDF v6.1-rc1 external RAM Kconfig: `SPIRAM_BOOT_HW_INIT`, `CONFIG_SPIRAM_USE_MALLOC`, `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`, `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL`, memory test/fail policy.
- ESP-IDF v6.1-rc1 ESP32-S3 PSRAM configuration: QUAD/OCT mode phải khớp phần cứng; QUAD 120 MHz được đánh dấu stable, OCTAL 120 MHz experimental.
- ESP-IDF 6.x migration: PicolibC default, FreeRTOS placement changes, removed `esp_spiram.h`, removed built-in JSON component.
- ESP-IDF OTA rollback flow: `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`, `ESP_OTA_IMG_PENDING_VERIFY`, `esp_ota_mark_app_valid_cancel_rollback()`, `esp_ota_mark_app_invalid_rollback_and_reboot()`.

---

**Trạng thái tài liệu:** Ready for implementation on pinned ESP-IDF v6.1-rc1 after completing Phase P-1 migration gate.  
**Ưu tiên triển khai:** P-1 IDF migration → P0 stack metric → P1 PSRAM → P2 memory policy → P3 NimBLE lock → P4 telemetry → P5 HTTP audit → P6/P7 stack tuning → P8/P9 OTA → full stress.
