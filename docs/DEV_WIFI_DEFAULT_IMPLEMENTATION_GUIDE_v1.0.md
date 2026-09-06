# ESP32 BLE Gateway — Development Wi‑Fi Default/Fallback Implementation Guide

**Document:** `DEV_WIFI_DEFAULT_IMPLEMENTATION_GUIDE_v1.0.md`
**Project:** `hailp-vn38/esp-ble-gateway`
**Target:** ESP32-S3
**ESP-IDF baseline observed in project:** v6.1-rc1
**Version:** 1.0
**Date:** 2026-09-06

---

## 1. Mục tiêu

Bổ sung cơ chế Wi‑Fi dành cho development để sau khi:

- build firmware mới;
- flash firmware;
- `erase-flash`;
- NVS chưa có Wi‑Fi;
- dùng board mới;

gateway có thể tự thử kết nối vào Wi‑Fi development đã cấu hình sẵn, thay vì developer phải mở captive portal và nhập lại SSID/password mỗi lần.

Yêu cầu quan trọng:

1. Không phá flow Wi‑Fi provisioning hiện tại.
2. Credential do Web UI lưu vào NVS luôn có độ ưu tiên cao nhất.
3. Wi‑Fi development chỉ là **fallback khi NVS không có credential**.
4. Wi‑Fi development **không được ghi ngược vào NVS**.
5. Nếu Wi‑Fi development kết nối thất bại, gateway phải quay về captive portal.
6. Production build mặc định không chứa credential development.
7. Password thật không commit vào Git.
8. Không tăng đáng kể RAM runtime.
9. Không thay đổi public API hiện tại của `wifi_provisioning`.
10. Developer chỉ cần quy trình build/flash bình thường sau lần setup ban đầu.

---

## 2. Trạng thái project hiện tại

Các file liên quan trực tiếp:

```text
CMakeLists.txt
sdkconfig.defaults
sdkconfig.defaults.esp32s3
.gitignore

components/
└── wifi_provisioning/
    ├── CMakeLists.txt
    ├── Kconfig
    ├── wifi_prov.c
    ├── include/
    │   └── wifi_prov.h
    └── ...

test/
├── CMakeLists.txt
├── sdkconfig.defaults
├── run_tests.sh
└── main/
```

### 2.1 Credential runtime hiện tại

`components/wifi_provisioning/wifi_prov.c` đang sử dụng:

```c
static const char *NVS_NAMESPACE = "wifi_cfg";
```

Credential được lưu trong NVS bằng:

```text
namespace: wifi_cfg
ssid key:  ssid
pass key:  pass
```

Các helper hiện tại:

```c
static esp_err_t load_wifi_credentials(...);
static esp_err_t save_wifi_credentials(...);
esp_err_t wifi_prov_clear_credentials(void);
```

### 2.2 Flow boot hiện tại

Flow hiện tại có thể tóm tắt:

```text
wifi_prov_init()
    |
    +-- init esp_netif/event loop
    |
    +-- create STA + AP netif
    |
    +-- esp_wifi_init()
    |
    +-- esp_wifi_set_storage(WIFI_STORAGE_RAM)
    |
    +-- load_wifi_credentials() from NVS
            |
            +-- found
            |    |
            |    +-- WIFI_MODE_STA
            |    +-- connect
            |    |
            |    +-- GOT_IP -> CONNECTED
            |    |
            |    +-- fail -> enter_provisioning()
            |
            +-- not found
                 |
                 +-- start APSTA
                 +-- enter_provisioning()
```

Đây là flow tốt và không nên viết lại toàn bộ.

---

# 3. Kiến trúc đề xuất

Flow sau khi update:

```text
BOOT
 |
 +-- NVS có credential?
 |      |
 |      +-- YES
 |      |    |
 |      |    +-- connect NVS credential
 |      |         |
 |      |         +-- success -> CONNECTED
 |      |         |
 |      |         +-- fail -> PROVISIONING
 |      |
 |      +-- NO
 |           |
 |           +-- DEV DEFAULT enabled?
 |                  |
 |                  +-- YES
 |                  |    |
 |                  |    +-- connect DEV credential
 |                  |         |
 |                  |         +-- success -> CONNECTED
 |                  |         |
 |                  |         +-- fail -> PROVISIONING
 |                  |
 |                  +-- NO -> PROVISIONING
 |
 +-- Captive Portal
```

## 3.1 Thứ tự ưu tiên credential

Bắt buộc:

```text
1. NVS credential
2. Development sdkconfig credential
3. Captive Portal
```

Không dùng:

```text
DEV -> NVS -> Portal
```

vì development config không được override cấu hình runtime của người dùng.

---

# 4. Danh sách file cần thêm/sửa

## Bắt buộc

```text
MODIFY  components/wifi_provisioning/Kconfig
MODIFY  components/wifi_provisioning/wifi_prov.c
MODIFY  sdkconfig.defaults
MODIFY  CMakeLists.txt
MODIFY  .gitignore

ADD     sdkconfig.defaults.local.example
```

## Khuyến nghị

```text
MODIFY  components/wifi_provisioning/README.md
ADD     docs/DEV_WIFI_DEFAULT_IMPLEMENTATION_GUIDE_v1.0.md
```

## Không cần sửa

```text
components/wifi_provisioning/include/wifi_prov.h
components/wifi_provisioning/CMakeLists.txt
components/web_server/web_wifi_api.c
main/main.c
partitions.csv
```

Không thay đổi public API của `wifi_provisioning`.

---

# 5. Phase 1 — Thêm Kconfig cho Development Wi‑Fi ✅ DONE (2026-09-06)

## File

```text
components/wifi_provisioning/Kconfig
```

Kconfig hiện có các config như:

```text
WIFI_PROV_AP_PREFIX
WIFI_PROV_AP_PASSWORD
WIFI_PROV_AP_MAX_CONNECTIONS
WIFI_PROV_STA_BOOT_RETRY_COUNT
WIFI_PROV_STA_BOOT_TIMEOUT_MS
WIFI_PROV_STA_TEST_TIMEOUT_MS
WIFI_PROV_CAPTIVE_DHCP_OPTION_114
```

Thêm block sau **trước `endmenu`**.

```kconfig
config WIFI_PROV_DEV_DEFAULT_ENABLED
    bool "Enable development default Wi-Fi fallback"
    default n
    help
        Development-only Wi-Fi fallback.

        When enabled and no valid Wi-Fi credential exists in NVS,
        the gateway tries WIFI_PROV_DEV_DEFAULT_SSID and
        WIFI_PROV_DEV_DEFAULT_PASSWORD before starting the
        provisioning captive portal.

        NVS credentials always have higher priority.

        Do not enable this option in production firmware because
        credentials are embedded in the firmware image.

if WIFI_PROV_DEV_DEFAULT_ENABLED

config WIFI_PROV_DEV_DEFAULT_SSID
    string "Development default Wi-Fi SSID"
    default ""
    help
        Development Wi-Fi SSID.

        Maximum Wi-Fi SSID length is 32 bytes.

config WIFI_PROV_DEV_DEFAULT_PASSWORD
    string "Development default Wi-Fi password"
    default ""
    help
        Development Wi-Fi password.

        The value is compiled into the firmware image.
        Keep real credentials in sdkconfig.defaults.local and
        never commit that file to source control.

endif
```

Sau update, cấu trúc cuối file sẽ tương tự:

```kconfig
menu "Wi-Fi Provisioning"

config WIFI_PROV_AP_PREFIX
    ...

config WIFI_PROV_AP_PASSWORD
    ...

config WIFI_PROV_AP_MAX_CONNECTIONS
    ...

config WIFI_PROV_STA_BOOT_RETRY_COUNT
    ...

config WIFI_PROV_STA_BOOT_TIMEOUT_MS
    ...

config WIFI_PROV_STA_TEST_TIMEOUT_MS
    ...

config WIFI_PROV_CAPTIVE_DHCP_OPTION_114
    ...

config WIFI_PROV_DEV_DEFAULT_ENABLED
    bool "Enable development default Wi-Fi fallback"
    default n
    ...

if WIFI_PROV_DEV_DEFAULT_ENABLED

config WIFI_PROV_DEV_DEFAULT_SSID
    string "Development default Wi-Fi SSID"
    default ""

config WIFI_PROV_DEV_DEFAULT_PASSWORD
    string "Development default Wi-Fi password"
    default ""

endif

endmenu
```

### Acceptance criteria Phase 1

- [x] `idf.py menuconfig` hiển thị option development Wi‑Fi.
- [x] Khi disabled, production behavior không đổi.
- [x] Khi enabled, SSID/password xuất hiện trong menuconfig.
- [x] Default của feature là `n`.

---

# 6. Phase 2 — Thêm production-safe defaults ✅ DONE (2026-09-06)

## File

```text
sdkconfig.defaults
```

Project hiện đang dùng file này như production defaults.

Trong section:

```text
# ==== Wi-Fi ====
```

thêm:

```ini
# Development Wi-Fi fallback.
# Production-safe default: disabled.
CONFIG_WIFI_PROV_DEV_DEFAULT_ENABLED=n
```

Kết quả:

```ini
# ==== Wi-Fi ====
CONFIG_ESP_WIFI_SOFTAP_SUPPORT=y

# Development Wi-Fi fallback.
# Keep disabled in committed production defaults.
CONFIG_WIFI_PROV_DEV_DEFAULT_ENABLED=n
```

## Không thêm credential thật vào file này

Không commit:

```ini
CONFIG_WIFI_PROV_DEV_DEFAULT_ENABLED=y
CONFIG_WIFI_PROV_DEV_DEFAULT_SSID="OfficeWifi"
CONFIG_WIFI_PROV_DEV_DEFAULT_PASSWORD="real-password"
```

Lý do:

- `sdkconfig.defaults` đang nằm trong Git;
- password sẽ tồn tại trong Git history;
- password cũng được compile vào firmware;
- production build có nguy cơ vô tình chứa credential.

---

# 7. Phase 3 — Thêm local development defaults ✅ DONE (2026-09-06)

## File mới

```text
sdkconfig.defaults.local.example
```

File này **được commit**.

Nội dung:

```ini
# ============================================================
# LOCAL DEVELOPMENT WI-FI TEMPLATE
# ============================================================
#
# Copy:
#
#   cp sdkconfig.defaults.local.example sdkconfig.defaults.local
#
# Then edit sdkconfig.defaults.local with your actual development
# Wi-Fi credentials.
#
# NEVER put real credentials in this example file.
# ============================================================

CONFIG_WIFI_PROV_DEV_DEFAULT_ENABLED=y
CONFIG_WIFI_PROV_DEV_DEFAULT_SSID="YOUR_DEV_WIFI_SSID"
CONFIG_WIFI_PROV_DEV_DEFAULT_PASSWORD="YOUR_DEV_WIFI_PASSWORD"
```

Developer tạo file local:

```bash
cp sdkconfig.defaults.local.example sdkconfig.defaults.local
```

Sau đó sửa:

```ini
CONFIG_WIFI_PROV_DEV_DEFAULT_ENABLED=y
CONFIG_WIFI_PROV_DEV_DEFAULT_SSID="Lab-WiFi"
CONFIG_WIFI_PROV_DEV_DEFAULT_PASSWORD="xxxxxxxx"
```

File thực tế:

```text
sdkconfig.defaults.local
```

**không được commit**.

---

# 8. Phase 4 — Ignore local credential ✅ DONE (2026-09-06)

## File

```text
.gitignore
```

Thêm section:

```gitignore
# Local ESP-IDF development configuration / secrets
sdkconfig.defaults.local
```

Không dùng:

```gitignore
sdkconfig.defaults.local*
```

vì pattern đó cũng có thể ignore:

```text
sdkconfig.defaults.local.example
```

mà file example cần được commit.

### Kiểm tra

```bash
git status
```

`sdkconfig.defaults.local` không được xuất hiện.

Có thể kiểm tra trực tiếp:

```bash
git check-ignore -v sdkconfig.defaults.local
```

---

# 9. Phase 5 — Tự động nạp local defaults trong CMake ✅ DONE (2026-09-06)

## File

```text
CMakeLists.txt
```

Hiện tại:

```cmake
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

idf_build_set_property(MINIMAL_BUILD ON)
project(esp32_ble_gateway)
```

Update thành:

```cmake
cmake_minimum_required(VERSION 3.22)

# Base project defaults.
set(SDKCONFIG_DEFAULTS
    "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults"
)

# Optional developer-local overrides.
#
# This file is gitignored and may contain local development secrets.
# It must be appended AFTER sdkconfig.defaults so local values can
# override production-safe defaults.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.local")
    list(APPEND SDKCONFIG_DEFAULTS
         "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.local")
    message(STATUS "Using local sdkconfig defaults: sdkconfig.defaults.local")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# "Trim" the build. Include the minimal set of components, main,
# and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

project(esp32_ble_gateway)
```

## Tại sao phải đặt trước `project.cmake`

`SDKCONFIG_DEFAULTS` phải được thiết lập trước:

```cmake
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
```

để ESP-IDF build system nhận danh sách defaults.

## Thứ tự apply config

Với ESP32-S3 và local file tồn tại, thứ tự mong muốn là:

```text
sdkconfig.defaults
        ↓
sdkconfig.defaults.esp32s3
        ↓
sdkconfig.defaults.local
```

Do đó:

```text
production-safe defaults
        ↓
board-specific defaults
        ↓
developer-local overrides
```

`CONFIG_WIFI_PROV_DEV_DEFAULT_ENABLED=n` trong base sẽ được local override thành `y`.

## Nếu local file không tồn tại

Build production vẫn hoạt động:

```text
sdkconfig.defaults
        ↓
sdkconfig.defaults.esp32s3
```

Không có dependency bắt buộc vào file local.

---

# 10. Phase 6 — Thêm credential source resolver ✅ DONE (2026-09-06)

## File

```text
components/wifi_provisioning/wifi_prov.c
```

Không sửa public header.

## 10.1 Thêm enum internal

Đặt sau các constant/static declaration, trước nhóm helper.

```c
typedef enum {
    WIFI_CREDENTIAL_SOURCE_NONE = 0,
    WIFI_CREDENTIAL_SOURCE_NVS,
    WIFI_CREDENTIAL_SOURCE_DEV_DEFAULT,
} wifi_credential_source_t;
```

Enum này chỉ tồn tại trong `.c`, không đưa vào public API.

---

## 10.2 Thêm helper lấy development credential

Đặt trong section:

```c
/* ------------------------------------------------------------------ */
/* NVS helpers                                                         */
/* ------------------------------------------------------------------ */
```

sau `load_wifi_credentials()` hoặc tạo section riêng:

```c
/* ------------------------------------------------------------------ */
/* Boot credential helpers                                             */
/* ------------------------------------------------------------------ */
```

Code:

```c
static bool load_dev_default_credentials(char *ssid, size_t ssid_len,
                                         char *password,
                                         size_t password_len)
{
#if CONFIG_WIFI_PROV_DEV_DEFAULT_ENABLED
    const char *dev_ssid = CONFIG_WIFI_PROV_DEV_DEFAULT_SSID;
    const char *dev_password = CONFIG_WIFI_PROV_DEV_DEFAULT_PASSWORD;

    if (dev_ssid[0] == '\0') {
        return false;
    }

    strlcpy(ssid, dev_ssid, ssid_len);
    strlcpy(password, dev_password, password_len);
    return true;
#else
    (void)ssid;
    (void)ssid_len;
    (void)password;
    (void)password_len;
    return false;
#endif
}
```

### Quy tắc

Helper này:

- chỉ đọc compile-time config;
- không dùng heap;
- không mở NVS;
- không ghi NVS;
- không log password;
- chỉ copy vào buffer boot hiện có.

---

## 10.3 Thêm resolver NVS > DEV

Thêm helper:

```c
static wifi_credential_source_t resolve_boot_credentials(
    char *ssid,
    size_t ssid_len,
    char *password,
    size_t password_len)
{
    esp_err_t error =
        load_wifi_credentials(ssid, ssid_len, password, password_len);

    if (error == ESP_OK && ssid[0] != '\0') {
        return WIFI_CREDENTIAL_SOURCE_NVS;
    }

    memset(ssid, 0, ssid_len);
    memset(password, 0, password_len);

    if (load_dev_default_credentials(ssid, ssid_len,
                                     password, password_len)) {
        return WIFI_CREDENTIAL_SOURCE_DEV_DEFAULT;
    }

    return WIFI_CREDENTIAL_SOURCE_NONE;
}
```

## Tại sao resolver không thử DEV khi NVS có nhưng connection fail?

Đây là behavior có chủ ý.

Nếu NVS có:

```text
ssid = HomeWifi
```

nhưng `HomeWifi` đang tạm mất sóng, gateway không được tự nhảy sang Wi‑Fi dev.

Current runtime configuration phải có quyền ưu tiên.

Behavior:

```text
NVS exists
    |
    +-- connect success -> CONNECTED
    |
    +-- connect fail -> PROVISIONING
```

Không phải:

```text
NVS fail -> DEV Wi-Fi
```

Điều này tránh development config âm thầm che lỗi cấu hình thực tế.

---

# 11. Phase 7 — Validate development config ✅ DONE (2026-09-06)

## File

```text
components/wifi_provisioning/wifi_prov.c
```

Update:

```c
static esp_err_t validate_config(void)
```

Giữ nguyên validation SoftAP hiện có, sau đó thêm:

```c
#if CONFIG_WIFI_PROV_DEV_DEFAULT_ENABLED
    const char *dev_ssid = CONFIG_WIFI_PROV_DEV_DEFAULT_SSID;
    const char *dev_password = CONFIG_WIFI_PROV_DEV_DEFAULT_PASSWORD;

    size_t dev_ssid_len = strnlen(dev_ssid, 33);
    size_t dev_password_len = strnlen(dev_password, 65);

    if (dev_ssid_len == 0 || dev_ssid_len > 32) {
        ESP_LOGE(TAG,
                 "Development Wi-Fi SSID must contain 1..32 bytes");
        return ESP_ERR_INVALID_ARG;
    }

    if (dev_password_len > 64) {
        ESP_LOGE(TAG,
                 "Development Wi-Fi password exceeds 64 bytes");
        return ESP_ERR_INVALID_ARG;
    }
#endif
```

Full conceptual function:

```c
static esp_err_t validate_config(void)
{
    const char *password = CONFIG_WIFI_PROV_AP_PASSWORD;
    size_t length = strlen(password);

    if (length > 0 && (length < 8 || length > 63)) {
        ESP_LOGE(TAG,
                 "CONFIG_WIFI_PROV_AP_PASSWORD must be empty (open AP) "
                 "or 8..63 characters for WPA2");
        return ESP_ERR_INVALID_ARG;
    }

#if CONFIG_WIFI_PROV_DEV_DEFAULT_ENABLED
    const char *dev_ssid = CONFIG_WIFI_PROV_DEV_DEFAULT_SSID;
    const char *dev_password = CONFIG_WIFI_PROV_DEV_DEFAULT_PASSWORD;

    size_t dev_ssid_len = strnlen(dev_ssid, 33);
    size_t dev_password_len = strnlen(dev_password, 65);

    if (dev_ssid_len == 0 || dev_ssid_len > 32) {
        ESP_LOGE(TAG,
                 "Development Wi-Fi SSID must contain 1..32 bytes");
        return ESP_ERR_INVALID_ARG;
    }

    if (dev_password_len > 64) {
        ESP_LOGE(TAG,
                 "Development Wi-Fi password exceeds 64 bytes");
        return ESP_ERR_INVALID_ARG;
    }
#endif

    return ESP_OK;
}
```

> Lưu ý: không log password trong bất kỳ error path nào.

---

# 12. Phase 8 — Update `wifi_prov_init()` ✅ DONE (2026-09-06)

## File

```text
components/wifi_provisioning/wifi_prov.c
```

Đây là thay đổi chính.

## 12.1 Đoạn hiện tại cần thay

Hiện flow đang tạo:

```c
char ssid[33] = {0};
char saved_password[65] = {0};

bool has_credentials =
    load_wifi_credentials(...) == ESP_OK &&
    ssid[0] != '\0';
```

Sau đó:

```c
if (has_credentials) {
    ...
} else {
    ...
}
```

## 12.2 Thay bằng credential source

Dùng:

```c
char ssid[33] = {0};
char password[65] = {0};

wifi_credential_source_t credential_source =
    resolve_boot_credentials(ssid, sizeof(ssid),
                             password, sizeof(password));

bool has_credentials =
    credential_source != WIFI_CREDENTIAL_SOURCE_NONE;
```

Log source:

```c
if (credential_source == WIFI_CREDENTIAL_SOURCE_NVS) {
    ESP_LOGI(TAG,
             "Trying saved Wi-Fi credentials (SSID=%s)",
             ssid);
} else if (credential_source ==
           WIFI_CREDENTIAL_SOURCE_DEV_DEFAULT) {
    ESP_LOGW(TAG,
             "No saved Wi-Fi; trying development default SSID=%s",
             ssid);
}
```

Không log:

```c
ESP_LOGI(TAG, "password=%s", password);
```

---

## 12.3 Flow connect dùng chung

Không duplicate connect implementation.

Giữ cùng flow hiện tại:

```c
if (has_credentials) {
    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) goto fail;
    if (esp_wifi_start() != ESP_OK) goto fail;

    s_wifi_started = true;
    apply_power_save_policy();
    set_state(WIFI_PROV_STATE_BOOT_CONNECTING);

    if (start_sta_attempt(ssid, password) != ESP_OK) {
        stop_sta_attempt();

        if (enter_provisioning() != ESP_OK) goto fail;

        return 0;
    }

    EventBits_t bits = wait_sta_result(
        pdMS_TO_TICKS(CONFIG_WIFI_PROV_STA_BOOT_TIMEOUT_MS));

    if ((bits & PROV_EVT_STA_GOT_IP) != 0) {
        reset_retry();
        set_state(WIFI_PROV_STATE_CONNECTED);

        if (credential_source == WIFI_CREDENTIAL_SOURCE_NVS) {
            ESP_LOGI(TAG,
                     "Saved Wi-Fi verified; running in STA mode");
        } else {
            ESP_LOGW(TAG,
                     "Development default Wi-Fi connected; "
                     "credential was not persisted to NVS");
        }
    } else {
        if (credential_source == WIFI_CREDENTIAL_SOURCE_NVS) {
            ESP_LOGW(TAG,
                     "Saved credential boot connect failed; "
                     "entering provisioning");
        } else {
            ESP_LOGW(TAG,
                     "Development default Wi-Fi connect failed; "
                     "entering provisioning");
        }

        stop_sta_attempt();

        if (enter_provisioning() != ESP_OK) goto fail;
    }
} else {
    ESP_LOGI(TAG,
             "No saved or development Wi-Fi; "
             "entering provisioning mode");

    if (ensure_apsta_mode() != ESP_OK ||
        configure_softap() != ESP_OK) {
        goto fail;
    }

    if (esp_wifi_start() != ESP_OK) goto fail;

    s_wifi_started = true;
    apply_power_save_policy();

    if (enter_provisioning() != ESP_OK) goto fail;
}
```

---

# 13. Full recommended boot credential block

Để implementation ít nhầm, block từ sau `generate_ap_ssid();` có thể được chuyển thành logic sau:

```c
generate_ap_ssid();

char ssid[33] = {0};
char password[65] = {0};

wifi_credential_source_t credential_source =
    resolve_boot_credentials(ssid, sizeof(ssid),
                             password, sizeof(password));

bool has_credentials =
    credential_source != WIFI_CREDENTIAL_SOURCE_NONE;

if (credential_source == WIFI_CREDENTIAL_SOURCE_NVS) {
    ESP_LOGI(TAG,
             "Trying saved Wi-Fi credentials (SSID=%s)",
             ssid);
} else if (credential_source ==
           WIFI_CREDENTIAL_SOURCE_DEV_DEFAULT) {
    ESP_LOGW(TAG,
             "No saved Wi-Fi; trying development default SSID=%s",
             ssid);
}

if (has_credentials) {
    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) goto fail;
    if (esp_wifi_start() != ESP_OK) goto fail;

    s_wifi_started = true;
    apply_power_save_policy();
    set_state(WIFI_PROV_STATE_BOOT_CONNECTING);

    if (start_sta_attempt(ssid, password) != ESP_OK) {
        stop_sta_attempt();

        if (enter_provisioning() != ESP_OK) goto fail;

        return 0;
    }

    EventBits_t bits = wait_sta_result(
        pdMS_TO_TICKS(CONFIG_WIFI_PROV_STA_BOOT_TIMEOUT_MS));

    if ((bits & PROV_EVT_STA_GOT_IP) != 0) {
        /*
         * Keep retry enabled: runtime disconnects still use
         * the existing bounded reconnect behavior.
         */
        reset_retry();
        set_state(WIFI_PROV_STATE_CONNECTED);

        if (credential_source == WIFI_CREDENTIAL_SOURCE_NVS) {
            ESP_LOGI(TAG,
                     "Saved Wi-Fi verified; running in STA mode");
        } else {
            ESP_LOGW(TAG,
                     "Development default Wi-Fi connected; "
                     "not persisted to NVS");
        }
    } else {
        if (credential_source == WIFI_CREDENTIAL_SOURCE_NVS) {
            ESP_LOGW(TAG,
                     "Saved credential boot connect failed; "
                     "entering provisioning");
        } else {
            ESP_LOGW(TAG,
                     "Development default Wi-Fi connect failed; "
                     "entering provisioning");
        }

        stop_sta_attempt();

        if (enter_provisioning() != ESP_OK) goto fail;
    }
} else {
    ESP_LOGI(TAG,
             "No boot Wi-Fi credential available; "
             "entering provisioning mode");

    if (ensure_apsta_mode() != ESP_OK ||
        configure_softap() != ESP_OK) {
        goto fail;
    }

    if (esp_wifi_start() != ESP_OK) goto fail;

    s_wifi_started = true;
    apply_power_save_policy();

    if (enter_provisioning() != ESP_OK) goto fail;
}

return 0;
```

---

# 14. Không persist development credential

Không gọi:

```c
save_wifi_credentials(
    CONFIG_WIFI_PROV_DEV_DEFAULT_SSID,
    CONFIG_WIFI_PROV_DEV_DEFAULT_PASSWORD);
```

Development credential chỉ là:

```text
compile-time fallback
```

NVS chỉ được ghi bởi flow provisioning hiện tại:

```text
Web UI
  -> wifi_prov_test_and_save()
      -> test GOT_IP
          -> save_wifi_credentials()
```

Giữ invariant hiện tại:

```text
chỉ persist credential sau khi connection test GOT_IP thành công
```

---

# 15. Behavior sau khi Web UI provision

Giả sử local config:

```ini
DEV SSID = LabWifi
```

NVS đang trống.

Boot:

```text
NVS missing
   ↓
LabWifi
   ↓
CONNECTED
```

Sau đó user cấu hình từ provisioning/Web UI:

```text
NVS SSID = HomeWifi
```

Boot tiếp theo:

```text
NVS HomeWifi
   ↓
CONNECTED
```

Gateway không dùng `LabWifi`.

Đây là behavior mong muốn.

---

# 16. Behavior khi clear credential

Current API:

```c
wifi_prov_clear_credentials();
```

chỉ xóa:

```text
wifi_cfg/ssid
wifi_cfg/pass
```

Sau update, nếu development fallback đang enabled:

```text
clear NVS
   ↓
reboot
   ↓
NVS empty
   ↓
DEV Wi-Fi exists
   ↓
connect DEV Wi-Fi
```

Đây là behavior chủ ý cho development build.

Nếu cần test captive portal từ đầu:

### Cách 1 — disable dev fallback trong local file

```ini
CONFIG_WIFI_PROV_DEV_DEFAULT_ENABLED=n
```

Sau đó regenerate config.

### Cách 2 — tạm đổi tên local defaults

```bash
mv sdkconfig.defaults.local sdkconfig.defaults.local.disabled
rm -f sdkconfig
idf.py reconfigure
```

Production build mặc định không gặp behavior này vì:

```ini
CONFIG_WIFI_PROV_DEV_DEFAULT_ENABLED=n
```

---

# 17. Quan trọng: `sdkconfig.defaults` không tự override `sdkconfig` đã sinh

ESP-IDF dùng `sdkconfig.defaults` để tạo/default các giá trị configuration.

Nếu project đã có:

```text
sdkconfig
```

và sau đó mới tạo:

```text
sdkconfig.defaults.local
```

thì local defaults mới có thể không override giá trị đã tồn tại trong generated `sdkconfig`.

Do đó sau lần setup đầu tiên phải regenerate.

## Setup lần đầu

```bash
cp sdkconfig.defaults.local.example sdkconfig.defaults.local
```

Edit credential.

Sau đó:

```bash
rm -f sdkconfig
idf.py set-target esp32s3
idf.py reconfigure
```

Hoặc clean config theo workflow team đang dùng.

Sau đó verify:

```bash
grep WIFI_PROV_DEV_DEFAULT sdkconfig
```

Expected:

```text
CONFIG_WIFI_PROV_DEV_DEFAULT_ENABLED=y
CONFIG_WIFI_PROV_DEV_DEFAULT_SSID="..."
CONFIG_WIFI_PROV_DEV_DEFAULT_PASSWORD="..."
```

> Không paste output chứa password vào issue/log công khai.

Sau lần setup này, build bình thường:

```bash
idf.py build
idf.py flash monitor
```

hoặc:

```bash
idf.py build flash monitor
```

---

# 18. Flow development sau khi triển khai

## Lần đầu trên workstation

```bash
git clone ...
cd esp-ble-gateway

cp sdkconfig.defaults.local.example sdkconfig.defaults.local

# edit:
# CONFIG_WIFI_PROV_DEV_DEFAULT_SSID
# CONFIG_WIFI_PROV_DEV_DEFAULT_PASSWORD

rm -f sdkconfig
idf.py set-target esp32s3
idf.py build flash monitor
```

## Các lần tiếp theo

Chỉ:

```bash
idf.py build flash monitor
```

Không cần nhập lại Wi‑Fi qua portal nếu:

- NVS vẫn còn credential; hoặc
- NVS bị xóa nhưng DEV fallback đang enabled.

---

# 19. Log mong muốn

## Case A — NVS có credential

```text
I wifi_prov: Trying saved Wi-Fi credentials (SSID=HomeWifi)
I wifi_prov: state: uninitialized -> boot_connecting
I wifi_prov: Got IP: 192.168.1.x
I wifi_prov: state: boot_connecting -> connected
I wifi_prov: Saved Wi-Fi verified; running in STA mode
```

Không xuất hiện:

```text
development default
```

---

## Case B — NVS trống, DEV enabled

```text
W wifi_prov: No saved Wi-Fi; trying development default SSID=LabWifi
I wifi_prov: state: uninitialized -> boot_connecting
I wifi_prov: Got IP: 192.168.1.x
I wifi_prov: state: boot_connecting -> connected
W wifi_prov: Development default Wi-Fi connected; not persisted to NVS
```

---

## Case C — DEV credential sai

```text
W wifi_prov: No saved Wi-Fi; trying development default SSID=LabWifi
...
W wifi_prov: Development default Wi-Fi connect failed; entering provisioning
I wifi_prov: state: boot_connecting -> provisioning
I wifi_prov: Provisioning portal ready: ...
```

---

## Case D — Không NVS, DEV disabled

```text
I wifi_prov: No boot Wi-Fi credential available; entering provisioning mode
I wifi_prov: Provisioning portal ready: ...
```

---

# 20. Security requirements

## Bắt buộc

Không log:

```text
CONFIG_WIFI_PROV_DEV_DEFAULT_PASSWORD
password buffer
NVS pass value
```

Không commit:

```text
sdkconfig.defaults.local
sdkconfig
```

Không lưu password vào:

```text
README.md
docs/
issues
CI logs
serial logs
```

## Firmware warning

Khi:

```ini
CONFIG_WIFI_PROV_DEV_DEFAULT_ENABLED=y
```

SSID/password được compile vào firmware.

Do đó dev firmware phải được coi là:

```text
contains development secret
```

Nếu firmware binary bị chia sẻ, credential có thể bị recover.

Chỉ dùng Wi‑Fi riêng cho:

```text
development / lab / test
```

Không dùng password của:

```text
corporate production network
personal sensitive network
customer network
```

---

# 21. Memory impact

Thay đổi đề xuất gần như không tạo runtime memory overhead đáng kể.

Existing boot stack đã có:

```c
char ssid[33];
char password[65];
```

Ta tái sử dụng hai buffer này.

Thêm:

```text
enum credential source
helper functions
Kconfig strings in flash/rodata
```

Không thêm:

```text
FreeRTOS task
queue
event group
mutex
heap allocation
persistent RAM cache
```

Do đó phù hợp mục tiêu tối ưu memory của gateway.

---

# 22. Không thay đổi reconnect runtime

Sau khi DEV Wi‑Fi kết nối thành công:

```c
set_state(WIFI_PROV_STATE_CONNECTED);
```

và behavior hiện tại vẫn giữ:

```text
CONNECTED
  |
  +-- WIFI_EVENT_STA_DISCONNECTED
       |
       +-- RECONNECTING
       |
       +-- bounded retry
```

DEV credential sau boot được copy vào `esp_wifi` config giống credential NVS.

Không cần thêm reconnect logic riêng.

Không mở provisioning portal tự động khi runtime Wi‑Fi chập chờn; giữ behavior hiện tại.

---

# 23. Không thay đổi Web UI API

Không cần sửa:

```text
components/web_server/web_wifi_api.c
```

Các endpoint provisioning hiện tại vẫn hoạt động như cũ.

Development fallback là boot-time internal policy, không phải Web API feature.

Không expose password hoặc development config qua API.

Nếu sau này muốn UI hiển thị nguồn credential, chỉ expose:

```json
{
  "wifi_source": "nvs"
}
```

hoặc:

```json
{
  "wifi_source": "development_default"
}
```

không expose SSID/password nếu không cần thiết.

Feature telemetry này **không nằm trong scope v1.0**.

---

# 24. Test plan

## T0 — Build production defaults

Điều kiện:

```text
sdkconfig.defaults.local không tồn tại
```

Thực hiện:

```bash
rm -f sdkconfig
idf.py set-target esp32s3
idf.py build
```

Expected:

```text
build PASS
CONFIG_WIFI_PROV_DEV_DEFAULT_ENABLED=n
```

Checklist:

- [ ] Không có credential development trong generated config.
- [ ] Firmware build bình thường.
- [ ] Existing Wi‑Fi provisioning không regression.

---

## T1 — DEV build configuration

Tạo:

```text
sdkconfig.defaults.local
```

với:

```ini
CONFIG_WIFI_PROV_DEV_DEFAULT_ENABLED=y
CONFIG_WIFI_PROV_DEV_DEFAULT_SSID="<VALID_TEST_AP>"
CONFIG_WIFI_PROV_DEV_DEFAULT_PASSWORD="<VALID_PASSWORD>"
```

Regenerate:

```bash
rm -f sdkconfig
idf.py reconfigure
```

Verify:

```bash
grep WIFI_PROV_DEV_DEFAULT sdkconfig
```

Expected enabled.

Checklist:

- [ ] local file override base `n`.
- [ ] `sdkconfig.defaults.esp32s3` vẫn được apply.
- [ ] build PASS.

---

## T2 — Empty NVS + valid DEV Wi‑Fi

Điều kiện:

```text
NVS wifi_cfg/ssid missing
NVS wifi_cfg/pass missing
DEV enabled
DEV credential valid
```

Expected:

```text
boot
 -> DEV credential
 -> GOT_IP
 -> CONNECTED
```

Checklist:

- [ ] Không mở captive portal.
- [ ] Không ghi DEV credential vào `wifi_cfg`.
- [ ] `wifi_prov_is_connected() == true`.
- [ ] IP STA hợp lệ.
- [ ] Web UI hoạt động trên STA IP.

---

## T3 — Empty NVS + invalid DEV Wi‑Fi

Điều kiện:

```text
NVS empty
DEV enabled
password incorrect
```

Expected:

```text
boot
 -> try DEV
 -> bounded retries / timeout
 -> enter_provisioning()
```

Checklist:

- [ ] Không stuck ở `FAILED`.
- [ ] SoftAP hoạt động.
- [ ] DNS captive portal hoạt động.
- [ ] Web provisioning API hoạt động.

---

## T4 — NVS valid + DEV valid

Điều kiện:

```text
NVS = Wifi-A
DEV = Wifi-B
cả hai reachable
```

Expected:

```text
Wifi-A
```

Checklist:

- [ ] Không attempt Wifi-B.
- [ ] Log source = saved/NVS.
- [ ] NVS có ưu tiên cao hơn DEV.

---

## T5 — NVS invalid + DEV valid

Điều kiện:

```text
NVS = Wifi-A, password sai
DEV = Wifi-B, valid
```

Expected theo policy v1.0:

```text
try Wifi-A
 -> fail
 -> provisioning
```

Không expected:

```text
Wifi-A fail -> Wifi-B
```

Checklist:

- [ ] DEV không che lỗi NVS.
- [ ] Captive portal được mở.

---

## T6 — DEV disabled + NVS empty

```ini
CONFIG_WIFI_PROV_DEV_DEFAULT_ENABLED=n
```

Expected:

```text
boot -> provisioning
```

Đây phải giống behavior firmware hiện tại.

---

## T7 — Web UI save credential

Start từ:

```text
NVS empty
DEV fallback enabled
```

Provision một credential mới bằng flow Web UI hiện tại.

Sau khi restart:

Expected:

```text
NVS credential được dùng
DEV credential không được dùng
```

Checklist:

- [ ] `wifi_prov_test_and_save()` vẫn persist sau GOT_IP.
- [ ] restart flow không regression.
- [ ] NVS wins after reboot.

---

## T8 — Clear NVS trong DEV build

Điều kiện:

```text
DEV enabled
NVS currently valid
```

Call existing clear credential flow.

Reboot.

Expected:

```text
NVS empty
 -> DEV fallback
 -> CONNECTED
```

Đây là expected development behavior.

---

## T9 — `erase-flash`

Điều kiện:

```text
DEV firmware config enabled
```

Thực hiện:

```bash
idf.py erase-flash
idf.py flash monitor
```

Expected:

```text
NVS mất
 -> DEV fallback
 -> tự connect
```

Đây là use case chính của feature.

---

## T10 — Normal firmware flash

Provision NVS một lần.

Sau đó:

```bash
idf.py flash monitor
```

Expected:

```text
NVS credential vẫn tồn tại
```

Nếu normal `idf.py flash` làm credential biến mất, cần điều tra riêng:

```text
partition table change
NVS erase logic
custom flashing script
full image containing NVS
factory reset path
```

Development fallback không nên được dùng để che lỗi NVS bị erase ngoài ý muốn.

---

# 25. Regression test

Sau implementation phải verify các flow cũ:

- [ ] Fresh production device -> captive portal.
- [ ] Scan Wi‑Fi qua Web UI.
- [ ] Submit credential.
- [ ] Credential được test.
- [ ] Chỉ save sau GOT_IP.
- [ ] Restart pending hoạt động.
- [ ] Reboot dùng NVS.
- [ ] Runtime disconnect -> reconnect bounded.
- [ ] Clear credential xóa NVS.
- [ ] SoftAP password validation còn hoạt động.
- [ ] Web UI không thấy API contract change.
- [ ] BLE startup không bị ảnh hưởng.
- [ ] MCP/WebSocket startup sau Wi‑Fi không bị ảnh hưởng.

---

# 26. Test project

Repo hiện có test app riêng dưới:

```text
test/
```

Không copy real development Wi‑Fi credential vào:

```text
test/sdkconfig.defaults
```

Giữ test defaults deterministic.

Nếu cần compile coverage cho các `#if` mới, có thể thêm một test-specific config không chứa secret:

```ini
CONFIG_WIFI_PROV_DEV_DEFAULT_ENABLED=n
```

Hardware integration test với Wi‑Fi thật nên chạy ngoài unit-test defaults hoặc dùng dedicated lab credential injected từ local/CI secret.

Không commit lab password vào test fixture.

---

# 27. Optional hardening — build-time warning

Có thể thêm sau `TAG` hoặc gần config helper:

```c
#if CONFIG_WIFI_PROV_DEV_DEFAULT_ENABLED
#warning "Development default Wi-Fi fallback is enabled; firmware contains Wi-Fi credentials"
#endif
```

Ưu điểm:

- production/release build dễ phát hiện configuration sai.

Nhược điểm:

- development build luôn có compiler warning.

**Khuyến nghị v1.0:** chưa cần thêm `#warning`; dùng production-safe defaults + gitignored local config là đủ.

---

# 28. Optional hardening — release guard

Nếu project sau này có profile production/release riêng, có thể thêm CI guard:

```bash
if grep -q '^CONFIG_WIFI_PROV_DEV_DEFAULT_ENABLED=y' sdkconfig; then
    echo "ERROR: development Wi-Fi fallback enabled in release build"
    exit 1
fi
```

Không cần triển khai ngay nếu chưa có release pipeline.

---

# 29. Coding style / maintainability

Giữ các nguyên tắc:

## Không duplicate Wi‑Fi connect flow

Sai:

```c
if (nvs) {
    // 50 lines connect
}

if (dev) {
    // same 50 lines connect
}
```

Đúng:

```text
resolve credential source
        ↓
single existing connection flow
```

## Không thêm module riêng chỉ cho 3 compile-time strings

Không cần:

```text
components/dev_wifi/
```

Feature này thuộc boot policy của:

```text
wifi_provisioning
```

Tách component mới sẽ tăng integration/CMake complexity mà không có lợi rõ ràng.

## Không sửa public header

Không expose implementation detail:

```c
WIFI_CREDENTIAL_SOURCE_DEV_DEFAULT
```

ra ngoài component trong v1.0.

---

# 30. Final file tree

Sau triển khai:

```text
esp-ble-gateway/
├── .gitignore                           # MODIFY
├── CMakeLists.txt                       # MODIFY
├── sdkconfig.defaults                   # MODIFY
├── sdkconfig.defaults.esp32s3           # unchanged
├── sdkconfig.defaults.local.example     # ADD / tracked
├── sdkconfig.defaults.local             # LOCAL ONLY / ignored
│
├── components/
│   └── wifi_provisioning/
│       ├── CMakeLists.txt               # unchanged
│       ├── Kconfig                      # MODIFY
│       ├── wifi_prov.c                  # MODIFY
│       ├── README.md                    # recommended update
│       └── include/
│           └── wifi_prov.h              # unchanged
│
└── docs/
    └── DEV_WIFI_DEFAULT_IMPLEMENTATION_GUIDE_v1.0.md
```

---

# 31. Implementation checklist

## Configuration

- [x] Add `WIFI_PROV_DEV_DEFAULT_ENABLED`.
- [x] Add `WIFI_PROV_DEV_DEFAULT_SSID`.
- [x] Add `WIFI_PROV_DEV_DEFAULT_PASSWORD`.
- [x] Base default is disabled.
- [x] Add `sdkconfig.defaults.local.example`.
- [x] Ignore `sdkconfig.defaults.local`.

## Build

- [x] Update root `CMakeLists.txt`.
- [x] `SDKCONFIG_DEFAULTS` set before `project.cmake`.
- [x] Base defaults loaded.
- [x] `sdkconfig.defaults.esp32s3` still loaded.
- [x] Local defaults loaded last.
- [x] Missing local file does not break build.

## Wi‑Fi implementation

- [x] Add internal credential-source enum.
- [x] Add `load_dev_default_credentials()`.
- [x] Add `resolve_boot_credentials()`.
- [x] NVS checked first.
- [x] DEV used only if NVS absent.
- [x] Existing STA connect path reused.
- [x] DEV connect failure -> provisioning.
- [x] DEV credential not saved to NVS.
- [x] Password never logged.

## Regression

- [x] Existing provisioning works.
- [x] Existing Web Wi‑Fi API unchanged.
- [x] Existing state machine unchanged.
- [x] Existing reconnect behavior unchanged.
- [x] Existing restart behavior unchanged.
- [x] Production behavior unchanged when feature disabled.

## Security

- [x] No real Wi‑Fi password committed.
- [x] Local file ignored.
- [x] Production default disabled.
- [x] Release firmware checked before distribution.

---

# 32. Definition of Done

Feature được coi là hoàn tất khi tất cả điều kiện sau pass:

```text
[PASS] production build không cần sdkconfig.defaults.local
[PASS] production default has DEV_WIFI disabled
[PASS] local config tự được CMake load
[PASS] NVS credential ưu tiên DEV credential
[PASS] NVS empty + DEV valid -> auto connect
[PASS] NVS empty + DEV invalid -> captive portal
[PASS] DEV credential không persist NVS
[PASS] Web provisioning tạo NVS credential bình thường
[PASS] reboot sau provisioning dùng NVS
[PASS] erase-flash + dev firmware tự kết nối DEV Wi-Fi
[PASS] normal flash không yêu cầu nhập lại Wi-Fi nếu NVS còn
[PASS] password không xuất hiện trong serial log
[PASS] no Web API regression
[PASS] no BLE/MCP startup regression
```

---

# 33. Implementation order khuyến nghị

Thực hiện đúng thứ tự:

```text
1. Kconfig
   ↓
2. sdkconfig.defaults
   ↓
3. sdkconfig.defaults.local.example
   ↓
4. .gitignore
   ↓
5. CMakeLists.txt auto-load local config
   ↓
6. wifi_prov.c credential resolver
   ↓
7. wifi_prov_init() integration
   ↓
8. regenerate sdkconfig
   ↓
9. build
   ↓
10. hardware tests T0..T10
   ↓
11. update wifi_provisioning README
```

Không refactor state machine Wi‑Fi cùng lúc với feature này. Giữ patch nhỏ để review và rollback dễ.

---

# 34. Kết luận kiến trúc

Kiến trúc cuối:

```text
                         +----------------------+
                         |      BOOT ESP32      |
                         +----------+-----------+
                                    |
                                    v
                         +----------------------+
                         | Read NVS wifi_cfg    |
                         +----------+-----------+
                                    |
                     +--------------+--------------+
                     |                             |
                   FOUND                         EMPTY
                     |                             |
                     v                             v
              +--------------+       +--------------------------+
              | Connect NVS  |       | DEV fallback enabled ?   |
              +------+-------+       +------------+-------------+
                     |                            |
               +-----+-----+               +------+------+
               |           |               |             |
             OK          FAIL             YES            NO
               |           |               |             |
               v           v               v             v
          CONNECTED   PROVISIONING    Connect DEV   PROVISIONING
                                         |
                                   +-----+-----+
                                   |           |
                                  OK          FAIL
                                   |           |
                                   v           v
                              CONNECTED   PROVISIONING
```

Nguyên tắc cốt lõi:

```text
NVS > DEV DEFAULT > CAPTIVE PORTAL
```

và:

```text
DEV DEFAULT != runtime persisted configuration
```

Đây là thay đổi nhỏ, ít RAM, không thay đổi API, không phá state machine hiện tại và giải quyết trực tiếp nhu cầu flash/reset nhanh trong quá trình phát triển.
