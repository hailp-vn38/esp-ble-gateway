# Wi-Fi Provisioning Refactor Specification

**Project:** ESP32 BLE Gateway  
**Repository:** `hailp-vn38/esp-ble-gateway`  
**Component:** `components/wifi_provisioning`  
**Target:** ESP32-S3 / ESP-IDF native  
**Document type:** Implementation specification  
**Version:** 2.0  
**Date:** 2026-08-25  
**Status:** Ready for implementation after review gate in Section 23

---

## 1. Purpose

Tài liệu này là specification để refactor `components/wifi_provisioning` theo codebase hiện tại. Mục tiêu là sửa các lỗi đã tìm thấy mà **không thay đổi application lifecycle ngoài phạm vi cần thiết**.

Refactor phải đạt các mục tiêu sau:

1. captive DNS xử lý DNS packet đúng và không còn bug `QDCOUNT`;
2. DNS task có lifecycle start/stop deterministic, không race khi restart;
3. Wi-Fi provisioning có state machine rõ ràng, bao gồm trạng thái chờ reboot sau khi credential được lưu;
4. không tắt SoftAP/DNS trước khi provisioning Web UI kịp nhận trạng thái thành công;
5. giữ compatibility với `web_wifi_api.c` hiện tại;
6. cleanup resource an toàn theo ownership;
7. SoftAP identity không bị trùng giữa nhiều gateway;
8. có test matrix đủ để triển khai và review code.

Tài liệu này **không** thiết kế lại toàn bộ networking subsystem và **không** chuyển gateway sang runtime hot-transition từ provisioning server sang full gateway server.

---

## 2. Current application constraints

Specification này dựa trên lifecycle hiện tại của application:

```text
app_main()
   |
   v
wifi_prov_init()
   |
   +-- provisioning --> start provisioning web server --> return
   |
   +-- connected ----> init device store / dispatcher / BLE / gateway Web / MCP
```

Khi boot vào provisioning, application hiện không khởi động BLE central, command dispatcher, gateway Web UI hoặc MCP. Vì vậy provisioning thành công **phải reboot** để đi qua normal boot path và khởi động đầy đủ gateway services.

`web_wifi_api.c` hiện cũng dùng workflow:

```text
wifi_prov_test_and_save()
        |
        +-- success --> wifi_prov_schedule_restart(4000 ms)
```

Refactor phải giữ behavior tương thích với flow này.

---

## 3. Scope

### 3.1 Files thay đổi bắt buộc

- `components/wifi_provisioning/wifi_prov.c`
- `components/wifi_provisioning/dns_hijack.c`
- `components/wifi_provisioning/include/wifi_prov.h`
- `components/wifi_provisioning/include/dns_hijack.h`
- `components/wifi_provisioning/CMakeLists.txt`
- `components/wifi_provisioning/Kconfig` — file mới

### 3.2 Files được dùng để compatibility-check nhưng không bắt buộc sửa

- `components/web_server/web_wifi_api.c`
- `main/main.c`

### 3.3 Ngoài phạm vi

- đổi provisioning Web server thành gateway Web server mà không reboot;
- runtime hot-start BLE/MCP sau provisioning;
- captive portal OS-specific HTTP redirect handlers;
- TLS / authentication;
- BLE subsystem;
- device registry;
- MCP implementation;
- thay đổi cấu trúc NVS toàn dự án.

---

## 4. Locked design decisions

Các quyết định trong mục này là **bắt buộc**, không để implementation tự chọn phương án khác.

### 4.1 Provisioning thành công vẫn reboot

Flow chuẩn:

```text
PROVISIONING
     |
     v
TESTING
     |
     +---- failure ----> PROVISIONING
     |
     +---- success
              |
              v
           SAVE NVS
              |
              v
      RESTART_PENDING
              |
              +-- SoftAP vẫn ON
              +-- captive DNS vẫn ON
              +-- provisioning Web API vẫn reachable
              |
              v
        delayed restart
              |
              v
            BOOT
              |
              v
     BOOT_CONNECTING
              |
              v
         CONNECTED
              |
              v
      full gateway services
```

Không chuyển `APSTA -> STA` ngay trong credential worker.

### 4.2 Public compatibility APIs được giữ

Trong refactor này **không xóa**:

```c
int wifi_prov_test_and_save(const char *ssid, const char *password);
int wifi_prov_save_and_connect(const char *ssid, const char *password);
int wifi_prov_schedule_restart(unsigned delay_ms);
```

Lý do: `web_wifi_api.c` hiện đang gọi trực tiếp các API này.

Có thể thêm API mới dùng `esp_err_t`, nhưng API legacy phải tiếp tục compile và giữ mapping result hiện tại cho tới một refactor integration riêng.

### 4.3 Error model nội bộ dùng `esp_err_t`

Internal helpers và API mới dùng `esp_err_t`.

Legacy public APIs kiểu `int` là compatibility wrappers và map lỗi về các code hiện tại:

| Legacy result | Meaning |
|---:|---|
| `0` | success |
| `-1` | invalid argument / generic init error |
| `-2` | invalid state / not provisioning |
| `-3` | operation busy |
| `-4` | Wi-Fi operation could not start |
| `-5` | connect timeout / credential verification failed |
| `-6` | NVS persistence failed |

Không thêm magic negative code mới nếu chưa update Web API mapping.

### 4.4 Workflow state và physical link state là hai khái niệm khác nhau

- `wifi_prov_state_t`: state của provisioning workflow.
- `s_sta_has_ip`: fact hẹp cho biết STA hiện có IPv4 từ `IP_EVENT_STA_GOT_IP`.

Không encode physical link state hoàn toàn bằng workflow enum.

### 4.5 Runtime disconnect không tự đổi Web mode

Sau khi application đã boot vào full gateway mode, Wi-Fi disconnect không được tự mở provisioning portal vì application không có logic hot-swap gateway HTTP server sang provisioning HTTP server.

Refactor chỉ cho phép auto-fallback sang provisioning trong **boot-connect phase**.

Runtime disconnect behavior:

```text
CONNECTED
    |
    v
RECONNECTING
    |
    +-- success --> CONNECTED
    |
    +-- exhausted --> FAILED
```

Từ `FAILED`, application/reboot policy sẽ được xử lý ở work item khác. Sau reboot, boot-connect failure có thể vào provisioning bình thường.

---

## 5. Target state model

### 5.1 Enum

```c
typedef enum {
    WIFI_PROV_STATE_UNINITIALIZED = 0,
    WIFI_PROV_STATE_BOOT_CONNECTING,
    WIFI_PROV_STATE_PROVISIONING,
    WIFI_PROV_STATE_TESTING,
    WIFI_PROV_STATE_RESTART_PENDING,
    WIFI_PROV_STATE_CONNECTED,
    WIFI_PROV_STATE_RECONNECTING,
    WIFI_PROV_STATE_FAILED,
} wifi_prov_state_t;
```

### 5.2 State semantics

| State | Meaning | SoftAP | STA connect | DNS | Expected next state |
|---|---|---:|---:|---:|---|
| `UNINITIALIZED` | component chưa ready | off | off | off | boot state |
| `BOOT_CONNECTING` | đang thử credential lưu trong NVS | off | yes | off | `CONNECTED` / `PROVISIONING` |
| `PROVISIONING` | portal active | on | no active test | on | `TESTING` |
| `TESTING` | giữ AP, thử credential STA | on | yes | on | `PROVISIONING` / `RESTART_PENDING` |
| `RESTART_PENDING` | credential đã verify+save, chờ reboot | on | yes, IP expected | on | reboot |
| `CONNECTED` | normal STA mode sau boot | off | connected | off | `RECONNECTING` |
| `RECONNECTING` | runtime reconnect | off | retry | off | `CONNECTED` / `FAILED` |
| `FAILED` | runtime networking không phục hồi | off | no active attempt | off | external action/reboot |

### 5.3 State ownership

Chỉ một helper được phép write state:

```c
static void set_state(wifi_prov_state_t new_state);
```

Helper phải log transition:

```text
state: PROVISIONING -> TESTING
state: TESTING -> RESTART_PENDING
```

Không write `s_state` trực tiếp rải rác trong code sau refactor.

### 5.4 Derived queries

```c
bool wifi_prov_is_connected(void)
{
    return s_sta_has_ip;
}
```

```c
bool wifi_prov_is_provisioning(void)
{
    wifi_prov_state_t state = wifi_prov_get_state();
    return state == WIFI_PROV_STATE_PROVISIONING ||
           state == WIFI_PROV_STATE_TESTING ||
           state == WIFI_PROV_STATE_RESTART_PENDING;
}
```

`RESTART_PENDING` vẫn được coi là provisioning để provisioning server tiếp tục hoạt động cho tới reboot.

### 5.5 State synchronization on ESP32-S3

ESP32-S3 là dual-core; `volatile` không phải synchronization primitive. `s_state` và `s_sta_has_ip` có thể được đọc/ghi từ ESP event task và application/worker task.

Dùng critical section ngắn cho state/fact snapshot:

```c
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;

static void set_state(wifi_prov_state_t new_state)
{
    wifi_prov_state_t old_state;
    portENTER_CRITICAL(&s_state_lock);
    old_state = s_state;
    s_state = new_state;
    portEXIT_CRITICAL(&s_state_lock);

    if (old_state != new_state) {
        ESP_LOGI(TAG, "state: %s -> %s",
                 wifi_prov_state_name(old_state),
                 wifi_prov_state_name(new_state));
    }
}
```

`wifi_prov_get_state()`, `wifi_prov_is_connected()` và helper update `s_sta_has_ip` phải đọc/ghi bằng cùng synchronization rule. Không giữ critical section khi gọi ESP-IDF API, logging, NVS hoặc blocking wait.

### 5.6 Intentional disconnect vs retry

State một mình không đủ để phân biệt `STA_DISCONNECTED` do RF/network failure với disconnect do component chủ động gọi `esp_wifi_disconnect()`. Giữ một flag semantic hẹp:

```c
static bool s_sta_retry_enabled;
```

Rules:

- set `true` ngay trước một boot/test/runtime connect attempt mà disconnect được phép trigger retry;
- set `false` **trước** intentional `esp_wifi_disconnect()`;
- event handler gặp `STA_DISCONNECTED` khi flag `false` chỉ clear `s_sta_has_ip` và return, không reconnect;
- khi `TESTING -> RESTART_PENDING`, set `false` vì không cần reconnect trước scheduled reboot;
- access flag bằng cùng state critical-section rule.

Helper bắt buộc:

```c
static void stop_sta_attempt(void)
{
    set_sta_retry_enabled(false);
    esp_wifi_disconnect();
}
```

Boot timeout/failure phải gọi `stop_sta_attempt()` trước `enter_provisioning()` để connect attempt cũ không tiếp tục chạy sau khi portal đã mở.

---

## 6. Wi-Fi event model

### 6.1 Event bits

Dùng EventGroup riêng cho Wi-Fi operation:

```c
#define PROV_EVT_STA_GOT_IP     BIT0
#define PROV_EVT_STA_FAILED     BIT1
#define PROV_EVT_AP_STARTED     BIT2
```

Mỗi connect attempt phải clear `STA_GOT_IP | STA_FAILED` trước khi gọi `esp_wifi_connect()`.

### 6.2 Event handler constraints

ESP event handler chỉ được:

- update fact nhỏ như `s_sta_has_ip`;
- update retry counter;
- set/clear EventGroup bits;
- gọi non-blocking Wi-Fi APIs cần thiết như `esp_wifi_connect()`;
- schedule worker nếu cần.

Không được:

- chờ semaphore dài;
- chờ EventGroup;
- start/stop DNS với blocking wait;
- ghi NVS;
- delay.

### 6.3 `WIFI_EVENT_STA_DISCONNECTED`

Handler phải phân biệt context theo state.

#### Khi `BOOT_CONNECTING`

- nếu `s_sta_retry_enabled == false`, clear physical IP fact và return;
- retry tối đa `CONFIG_WIFI_PROV_STA_BOOT_RETRY_COUNT`;
- exhausted -> set `PROV_EVT_STA_FAILED`;
- không tự start SoftAP trong event handler;
- `wifi_prov_init()` sau khi wait failure sẽ gọi `stop_sta_attempt()` rồi `enter_provisioning()`.

#### Khi `TESTING`

- nếu `s_sta_retry_enabled == false`, clear physical IP fact và return;
- retry tối đa configured test retry count;
- exhausted -> set `PROV_EVT_STA_FAILED`;
- không schedule fallback task;
- credential worker đang chờ sẽ gọi `stop_sta_attempt()` nếu cần và restore `PROVISIONING`.

#### Khi `CONNECTED` hoặc `RECONNECTING`

- `s_sta_has_ip = false`;
- nếu `s_sta_retry_enabled == false`, return;
- chuyển `RECONNECTING`;
- bounded reconnect;
- exhausted -> set retry disabled, transition `FAILED`;
- không start SoftAP.

#### Khi `PROVISIONING` hoặc `RESTART_PENDING`

STA disconnect không được làm tắt AP/DNS. Với `RESTART_PENDING`, reboot vẫn diễn ra theo scheduled task.

### 6.4 `IP_EVENT_STA_GOT_IP`

- set `s_sta_has_ip = true`;
- reset retry counter;
- set `PROV_EVT_STA_GOT_IP`;
- nếu state `RECONNECTING`, transition `CONNECTED`;
- nếu state `BOOT_CONNECTING` hoặc `TESTING`, state final được owner operation quyết định sau khi waiter resume.

### 6.5 `WIFI_EVENT_AP_START`

Set `PROV_EVT_AP_STARTED` để `enter_provisioning()` biết AP netif đã ready trước khi bind captive DNS.

---

## 7. Boot flow implementation

### 7.1 Preconditions

Application phải gọi và hoàn tất:

```c
nvs_flash_init();
```

trước `wifi_prov_init()`.

`wifi_prov_init()` không chịu trách nhiệm erase/reinitialize global NVS partition.

### 7.2 Initialization sequence

```text
wifi_prov_init()
  |
  +-- initialize esp_netif if needed
  +-- ensure default event loop exists
  +-- create operation mutex
  +-- create Wi-Fi event group
  +-- create STA/AP default netifs
  +-- esp_wifi_init
  +-- WIFI_STORAGE_RAM
  +-- register handlers
  +-- load credential from component NVS
  |
  +-- credential exists
  |      |
  |      +-- mode STA
  |      +-- configure STA
  |      +-- start Wi-Fi
  |      +-- state BOOT_CONNECTING
  |      +-- connect + bounded wait
  |             |
  |             +-- success --> state CONNECTED --> return ESP_OK
  |             +-- failure --> enter_provisioning() --> return ESP_OK
  |
  +-- no credential
         |
         +-- mode APSTA
         +-- configure AP
         +-- start Wi-Fi
         +-- enter_provisioning()
         +-- return ESP_OK
```

### 7.3 Boot timeout behavior

Nếu wait hết `CONFIG_WIFI_PROV_STA_BOOT_TIMEOUT_MS` mà chưa có `GOT_IP`, coi như boot-connect failure và chuyển provisioning.

Không lưu/xóa credential cũ tự động. User có thể nhập credential mới trong provisioning.

---

## 8. Provisioning entry

### 8.1 `enter_provisioning()` contract

```c
static esp_err_t enter_provisioning(void);
```

Postconditions khi trả `ESP_OK`:

- Wi-Fi mode là `WIFI_MODE_APSTA`;
- SoftAP config đã apply;
- AP đã emit `WIFI_EVENT_AP_START` hoặc AP netif xác nhận ready;
- AP IPv4 đã lấy được từ `s_ap_netif`;
- captive DNS đã bind đúng AP IPv4 port 53;
- state là `PROVISIONING`;
- `wifi_prov_is_provisioning() == true`.

Nếu DNS start fail, provisioning qua IP vẫn có thể hoạt động. Policy:

- log `ESP_LOGW`;
- vẫn set `PROVISIONING`;
- return `ESP_OK` nếu AP hoạt động;
- expose DNS failure qua log, không làm toàn bộ provisioning fail.

### 8.2 AP readiness

Không bind DNS ngay sau `esp_wifi_set_mode()` mà chưa biết AP ready.

Helper phải dùng rule cụ thể sau:

```c
static esp_err_t wait_for_ap_ready(TickType_t timeout)
{
    if (s_ap_netif != NULL && esp_netif_is_netif_up(s_ap_netif)) {
        return ESP_OK;
    }

    xEventGroupClearBits(s_wifi_events, PROV_EVT_AP_STARTED);

    /* Caller applies APSTA mode/config before or immediately after this point. */
    if (s_ap_netif != NULL && esp_netif_is_netif_up(s_ap_netif)) {
        return ESP_OK;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events, PROV_EVT_AP_STARTED, pdFALSE, pdFALSE, timeout);

    return (bits & PROV_EVT_AP_STARTED) ? ESP_OK : ESP_ERR_TIMEOUT;
}
```

Required ordering trong `enter_provisioning()`:

```text
check current AP-up state
clear AP_STARTED only if not already up
set/configure APSTA
wait AP_STARTED or observe netif-up
get AP IP
start DNS(ap_ip)
```

Việc check `esp_netif_is_netif_up()` trước và sau clear event ngăn missed-event race khi AP đã start trước waiter.

### 8.3 AP IP

Không hard-code `192.168.4.1` trong DNS component.

Wi-Fi layer lấy:

```c
esp_netif_ip_info_t ap_ip_info;
esp_netif_get_ip_info(s_ap_netif, &ap_ip_info);
```

và truyền `ap_ip_info.ip` cho DNS start.

Default ESP-IDF có thể vẫn là `192.168.4.1`, nhưng DNS không được phụ thuộc constant này.

---

## 9. Credential test-and-save flow

### 9.1 Legacy API contract

```c
int wifi_prov_test_and_save(const char *ssid, const char *password);
```

Preconditions:

- state phải là `PROVISIONING`;
- SSID dài `1..32` bytes;
- password dài `0..64` bytes;
- operation mutex available.

### 9.2 Flow

```text
PROVISIONING
   |
   v
lock operation
   |
   v
state TESTING
   |
   v
clear STA result bits
   |
   v
configure STA credential
   |
   v
esp_wifi_connect()
   |
   v
wait GOT_IP / FAILED / timeout
   |
   +-- failure
   |      |
   |      +-- stop STA attempt
   |      +-- state PROVISIONING
   |      +-- keep AP + DNS active
   |      +-- unlock
   |      +-- return -5
   |
   +-- success
          |
          +-- save credential NVS
          |
          +-- save failed
          |      |
          |      +-- stop STA attempt
          |      +-- state PROVISIONING
          |      +-- keep AP + DNS
          |      +-- unlock
          |      +-- return -6
          |
          +-- save success
                 |
                 +-- state RESTART_PENDING
                 +-- keep AP + DNS
                 +-- keep STA connection/IP until reboot
                 +-- unlock
                 +-- return 0
```

### 9.3 Important sequencing rule

Sau success **không** gọi:

```c
dns_hijack_stop();
esp_wifi_set_mode(WIFI_MODE_STA);
```

trong `wifi_prov_test_and_save()`.

Lý do: provisioning HTTP client đang kết nối qua SoftAP và cần poll `/api/wifi` để nhận trạng thái `succeeded` trước delayed restart.

### 9.4 Alias API

```c
int wifi_prov_save_and_connect(const char *ssid, const char *password)
{
    return wifi_prov_test_and_save(ssid, password);
}
```

Giữ alias cho compatibility. Có thể annotate comment `// legacy alias`.

---

## 10. Restart scheduling

### 10.1 API

Giữ:

```c
int wifi_prov_schedule_restart(unsigned delay_ms);
```

### 10.2 Preconditions

- component initialized;
- state nên là `RESTART_PENDING`.

Để giữ backward compatibility, nếu state khác nhưng component initialized, function có thể vẫn schedule restart; phải log warning.

### 10.3 Idempotency

Gọi nhiều lần chỉ tạo một restart task.

```text
first call  -> schedule task
second call -> return success, no second task
```

### 10.4 Failure behavior

Nếu `xTaskCreate()` fail:

- clear `s_restart_scheduled`;
- không thay đổi `RESTART_PENDING`;
- AP/DNS vẫn hoạt động;
- Web API có thể báo `Wi-Fi saved but restart could not be scheduled` như hiện tại.

### 10.5 Delay

Delay policy vẫn do Web layer quyết định. Component không hard-code 4000 ms.

---

## 11. Captive DNS protocol fixes

### 11.1 Root cause của current `QDCOUNT` bug

Không được kiểm tra hai byte `QDCOUNT` riêng lẻ.

Sai:

```c
if (query[4] == 0 || query[5] == 0)
    return -1;
```

Đúng:

```c
static uint16_t read_u16_be(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

uint16_t qdcount = read_u16_be(&packet[4]);
if (qdcount != 1)
    return DNS_PARSE_UNSUPPORTED;
```

### 11.2 Header validation

Parser phải validate tối thiểu:

- packet length >= 12;
- `QR == 0` — chỉ nhận query;
- `OPCODE == 0` — standard QUERY;
- `QDCOUNT == 1`;
- QNAME labels không vượt 63 bytes;
- QNAME không vượt packet;
- QTYPE/QCLASS tồn tại đủ 4 bytes sau QNAME.

### 11.3 QCLASS

Chỉ synthesize captive answer khi:

```text
QCLASS = IN (1)
```

QCLASS khác -> response `NOTIMP` hoặc empty `NOERROR`. Chọn cố định trong implementation này: **NOTIMP**.

### 11.4 QTYPE policy

| QTYPE | Response |
|---|---|
| `A (1)` | synthetic A answer = current AP IPv4 |
| `AAAA (28)` | `NOERROR`, `ANCOUNT=0` |
| other | `NOERROR`, `ANCOUNT=0` |

Không inject A record vào AAAA question.

### 11.5 AdGuard special-case phải được preserve

Các query:

```text
local.adguard.org
local.adguard.com
```

phải trả `NXDOMAIN`, không trả captive A record.

Behavior này có trong implementation hiện tại và phải được giữ sau refactor.

### 11.6 DNS response flags

Response không được advertise recursive resolver capability.

- `QR = 1`
- `OPCODE = 0`
- `AA = 0`
- `TC = 0`
- `RD` copy từ request
- `RA = 0`
- `Z = 0`
- `RCODE` theo response type

Không dùng flags kiểu `0x8180` vì bit `RA=1` là sai cho captive DNS này.

### 11.7 Answer TTL

Giữ TTL ngắn, ví dụ 30 giây.

### 11.8 Bounds safety

Mọi append vào packet phải check:

```c
required_len <= DNS_MAX_PACKET_LEN
```

Parser không được dereference byte ngoài `query_len`.

---

## 12. Captive DNS lifecycle redesign

### 12.1 Public API mới

`dns_hijack.h`:

```c
#ifndef DNS_HIJACK_H
#define DNS_HIJACK_H

#include "esp_err.h"
#include "esp_netif_ip_addr.h"

esp_err_t dns_hijack_start(const esp_ip4_addr_t *redirect_ip);
esp_err_t dns_hijack_stop(void);
bool dns_hijack_is_running(void);

#endif
```

Nếu include type trên không phù hợp với ESP-IDF version của repo, có thể dùng `esp_ip4_addr_t` từ header tương ứng; public contract vẫn là truyền IPv4 value, không hard-code trong DNS module.

### 12.2 DNS lifecycle states

```c
typedef enum {
    DNS_STATE_STOPPED = 0,
    DNS_STATE_STARTING,
    DNS_STATE_RUNNING,
    DNS_STATE_STOPPING,
} dns_state_t;
```

### 12.3 Synchronization primitives

DNS module sở hữu:

```c
static SemaphoreHandle_t s_dns_mutex;
static EventGroupHandle_t s_dns_events;
```

Event bits:

```c
#define DNS_EVT_RUNNING BIT0
#define DNS_EVT_STOPPED BIT1
#define DNS_EVT_START_FAILED BIT2
```

### 12.4 Start contract

`dns_hijack_start()` chỉ trả `ESP_OK` khi DNS task đã:

1. create socket;
2. bind socket vào `redirect_ip:53` thành công;
3. set `DNS_EVT_RUNNING`.

Nếu task create thành công nhưng bind fail, caller phải nhận lỗi, không được nhận false-positive success.

Pseudo-flow:

```text
dns_hijack_start(ip)
  |
  +-- validate ip
  +-- ensure sync primitives
  +-- lock
  +-- RUNNING with same ip -> unlock -> ESP_OK
  +-- RUNNING with different ip -> unlock -> stop -> restart
  +-- STOPPING -> unlock -> wait STOPPED -> retry start
  +-- set STARTING
  +-- copy redirect ip
  +-- clear lifecycle bits
  +-- create task
  +-- unlock
  +-- wait RUNNING | START_FAILED with timeout
```

### 12.5 Stop contract

`dns_hijack_stop()` chỉ trả `ESP_OK` sau khi:

- recv loop đã thoát;
- socket đã close;
- task đã cleanup;
- state = `STOPPED`;
- port UDP 53 được release khỏi task cũ.

Pseudo-flow:

```text
dns_hijack_stop()
  |
  +-- lock
  +-- STOPPED -> unlock -> ESP_OK
  +-- set STOPPING
  +-- snapshot socket
  +-- unlock
  +-- shutdown(socket)
  +-- wait DNS_EVT_STOPPED with timeout
  +-- timeout -> ESP_ERR_TIMEOUT
```

Không giữ `s_dns_mutex` trong lúc chờ task set `DNS_EVT_STOPPED`.

Nếu stop timeout, lifecycle **không được giả định là STOPPED**. Giữ state `STOPPING` (hoặc một failure state nội bộ tương đương) và reject `dns_hijack_start()` mới bằng `ESP_ERR_INVALID_STATE` cho tới khi task cũ thực sự signal `DNS_EVT_STOPPED`. Điều này ngăn một task mới được tạo trong khi task cũ vẫn có thể sở hữu socket.

### 12.6 Task cleanup rule

DNS task phải có một cleanup path duy nhất:

```text
close socket if valid
lock DNS lifecycle mutex
clear socket handle
clear task handle
set state STOPPED
unlock DNS lifecycle mutex
set DNS_EVT_STOPPED
vTaskDelete(NULL)
```

Mọi access tới DNS lifecycle state, task handle, socket handle và redirect IP snapshot phải tuân theo `s_dns_mutex`. Ngoại lệ duy nhất là EventGroup bits dùng để signaling. `dns_hijack_stop()` phải release mutex trước khi wait `DNS_EVT_STOPPED`, vì DNS task cần mutex để hoàn tất cleanup.

Không có nhiều return path bỏ qua cleanup.

### 12.7 Idempotency

- start twice -> một task;
- stop twice -> success;
- stop/start nhanh -> start mới chỉ bind sau task cũ đã `STOPPED`.

---

## 13. SoftAP identity and Kconfig

### 13.1 New `Kconfig`

Tạo `components/wifi_provisioning/Kconfig`:

```text
menu "Wi-Fi Provisioning"

config WIFI_PROV_AP_PREFIX
    string "Provisioning SoftAP SSID prefix"
    default "ESP-GW"

config WIFI_PROV_AP_PASSWORD
    string "Provisioning SoftAP password"
    default "gateway123"

config WIFI_PROV_AP_MAX_CONNECTIONS
    int "Maximum SoftAP clients"
    range 1 8
    default 4

config WIFI_PROV_STA_BOOT_RETRY_COUNT
    int "Saved credential boot retry count"
    range 0 20
    default 5

config WIFI_PROV_STA_BOOT_TIMEOUT_MS
    int "Saved credential boot timeout (ms)"
    range 1000 120000
    default 30000

config WIFI_PROV_STA_TEST_TIMEOUT_MS
    int "Credential test timeout (ms)"
    range 1000 120000
    default 20000

endmenu
```

Nếu ESP-IDF component Kconfig convention trong repo dùng `Kconfig.projbuild`, chọn đúng convention của project, nhưng không hard-code các giá trị trên trong `.c` sau refactor.

### 13.2 SSID generation

SSID:

```text
<PREFIX>-A1B2C3
```

suffix là 3 byte cuối STA MAC.

Implementation requirement:

- tổng SSID <= 32 bytes;
- nếu prefix quá dài, truncate có kiểm soát để vẫn giữ `-A1B2C3`;
- log generated SSID, không log password.

### 13.3 Authentication mode

Nếu configured password rỗng:

```c
WIFI_AUTH_OPEN
```

Nếu không rỗng:

```c
WIFI_AUTH_WPA2_PSK
```

Validation: WPA2 password phải đáp ứng giới hạn ESP-IDF/Wi-Fi config; nếu Kconfig cho string không hợp lệ, component phải fail init với log rõ ràng thay vì silently tạo AP không dùng được.

---

## 14. NVS contract

### 14.1 Namespace and keys

Giữ compatibility:

```text
namespace: wifi_cfg
keys:
  ssid
  pass
```

Không migrate storage format trong refactor này.

### 14.2 Save sequence

```text
open NVS read-write
set ssid
set pass
commit
close
```

Credentials chỉ được save **sau `IP_EVENT_STA_GOT_IP`** trong credential test.

### 14.3 Failure semantics

Nếu bất kỳ NVS operation nào fail:

- không transition `RESTART_PENDING`;
- return legacy `-6`;
- state quay `PROVISIONING`;
- AP/DNS tiếp tục chạy;
- stop/disconnect STA test connection để tránh state mơ hồ.

### 14.4 Atomicity wording

Refactor này **không tuyên bố** SSID/password pair là application-level transactional record độc lập. NVS commit được dùng đúng API, nhưng migration sang single versioned blob là work item riêng nếu cần stronger pair-consistency guarantees.

### 14.5 Clear credentials API

Có thể thêm:

```c
esp_err_t wifi_prov_clear_credentials(void);
```

Nhưng behavior runtime không được tự hot-switch HTTP mode. Trong full gateway mode, function chỉ erase credentials và return; caller quyết định reboot.

Nếu gọi khi đang provisioning, có thể erase và tiếp tục provisioning.

---

## 15. Resource ownership and cleanup

### 15.1 Global subsystem ownership

`esp_netif_init()` và default event loop có thể đã được module khác tạo.

Component **không được deinit global subsystem chỉ vì init call trả `ESP_ERR_INVALID_STATE`**.

Không teardown default event loop hoặc global netif subsystem trong component cleanup trừ khi codebase có ownership contract rõ ràng.

### 15.2 Resources component sở hữu trực tiếp

Track tối thiểu:

```c
static bool s_wifi_initialized;
static bool s_wifi_started;
static bool s_wifi_handler_registered;
static bool s_ip_handler_registered;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static EventGroupHandle_t s_wifi_events;
static SemaphoreHandle_t s_operation_mutex;
```

### 15.3 Cleanup order

Trên init failure:

```text
stop DNS if started
stop Wi-Fi if started
unregister IP handler if registered
unregister Wi-Fi handler if registered
deinit Wi-Fi driver if initialized
destroy default Wi-Fi netifs created by component
delete event group
delete mutex
reset local state/facts
```

Các netif được tạo bởi `esp_netif_create_default_wifi_sta()` / `esp_netif_create_default_wifi_ap()` phải được hủy bằng:

```c
esp_netif_destroy_default_wifi(s_sta_netif);
esp_netif_destroy_default_wifi(s_ap_netif);
```

Sau destroy phải set pointer về `NULL`. API này cũng detach interface và clear default Wi-Fi handlers gắn với netif; do đó cleanup order phải được test để không double-unregister application handlers.

### 15.4 Init idempotency

Nếu `wifi_prov_init()` bị gọi lần hai trong cùng boot, trả `ESP_ERR_INVALID_STATE` qua API mới hoặc legacy `-1`; không tạo duplicate netif/handler/task.

---

## 16. Operation mutex and concurrency

### 16.1 Serialized operations

`wifi_prov_scan()` và `wifi_prov_test_and_save()` dùng chung `s_operation_mutex`.

Không cho phép:

- scan chạy đồng thời credential test;
- hai credential test đồng thời.

### 16.2 Lock timeout

Giữ behavior tương thích khoảng 1000 ms hoặc đưa thành internal constant.

Legacy mapping:

```text
mutex timeout -> -3
```

### 16.3 Event handler không lấy operation mutex

Event loop callback không được chờ operation mutex để tránh inversion/deadlock.

### 16.4 Fallback task

Sau refactor, boot fallback không cần `s_fallback_scheduled` task nếu `wifi_prov_init()` tự wait connect và gọi `enter_provisioning()` trong calling context.

Nếu code structure vẫn cần fallback worker, worker phải chỉ được dùng trong boot phase và phải có explicit ownership/state check. Preferred implementation: **loại bỏ fallback task**.

---

## 17. Scanning behavior

### 17.1 Preconditions

Scan chỉ hợp lệ khi:

```text
state == PROVISIONING
```

Trong `TESTING` hoặc `RESTART_PENDING`, trả invalid state/busy để không can thiệp credential connect.

Legacy result giữ:

```text
-2 invalid state
-3 busy
-4 scan driver failure
```

### 17.2 Limits

- internal raw record cap có thể giữ 32;
- public default `WIFI_PROV_MAX_SCAN_RESULTS = 16`;
- deduplicate theo SSID như hiện tại;
- hidden SSID có thể bỏ qua;
- không overflow caller buffer.

### 17.3 Ordering

Nếu ESP-IDF scan trả RSSI order không đảm bảo, implementation có thể sort descending RSSI; đây là optional, không phải acceptance blocker.

---

## 18. Public headers after refactor

### 18.1 `wifi_prov.h`

```c
#ifndef WIFI_PROV_H
#define WIFI_PROV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define WIFI_PROV_MAX_SCAN_RESULTS 16

typedef enum {
    WIFI_PROV_STATE_UNINITIALIZED = 0,
    WIFI_PROV_STATE_BOOT_CONNECTING,
    WIFI_PROV_STATE_PROVISIONING,
    WIFI_PROV_STATE_TESTING,
    WIFI_PROV_STATE_RESTART_PENDING,
    WIFI_PROV_STATE_CONNECTED,
    WIFI_PROV_STATE_RECONNECTING,
    WIFI_PROV_STATE_FAILED,
} wifi_prov_state_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t authmode;
} wifi_prov_ap_record_t;

/* Existing compatibility API */
int wifi_prov_init(void);
bool wifi_prov_is_connected(void);
bool wifi_prov_is_provisioning(void);
wifi_prov_state_t wifi_prov_get_state(void);
const char *wifi_prov_state_name(wifi_prov_state_t state);
int wifi_prov_scan(wifi_prov_ap_record_t *records,
                   size_t max_records,
                   size_t *out_count);
int wifi_prov_test_and_save(const char *ssid, const char *password);
int wifi_prov_save_and_connect(const char *ssid, const char *password);
int wifi_prov_schedule_restart(unsigned delay_ms);
void wifi_prov_get_ip(char *out_ip, size_t out_ip_len);

/* Optional new API; do not require Web layer migration in this refactor */
esp_err_t wifi_prov_clear_credentials(void);

#endif
```

Không đổi `wifi_prov_init()` / scan / credential legacy return type trong cùng refactor để tránh unnecessary integration churn.

### 18.2 `dns_hijack.h`

```c
#ifndef DNS_HIJACK_H
#define DNS_HIJACK_H

#include <stdbool.h>
#include "esp_err.h"
#include "esp_netif_ip_addr.h"

esp_err_t dns_hijack_start(const esp_ip4_addr_t *redirect_ip);
esp_err_t dns_hijack_stop(void);
bool dns_hijack_is_running(void);

#endif
```

---

## 19. Suggested internal structure

### 19.1 `wifi_prov.c`

Recommended ordering:

```text
constants / config validation
state + owned resources
state helpers
NVS helpers
STA/AP config helpers
Wi-Fi event handler
connect wait helper
provisioning enter helper
scan API
credential API
restart API
status API
init + cleanup
```

Suggested helpers:

```c
static void set_state(wifi_prov_state_t state);
static esp_err_t validate_config(void);
static esp_err_t load_wifi_credentials(...);
static esp_err_t save_wifi_credentials(...);
static esp_err_t configure_sta(...);
static esp_err_t configure_softap(...);
static esp_err_t ensure_apsta_mode(void);
static esp_err_t wait_for_ap_ready(void);
static esp_err_t enter_provisioning(void);
static esp_err_t start_sta_attempt(...);
static esp_err_t wait_sta_result(TickType_t timeout_ticks);
static void stop_sta_attempt(void);
static void cleanup_owned_resources(void);
```

### 19.2 `dns_hijack.c`

Recommended ordering:

```text
constants
lifecycle state/resources
byte helpers
DNS question parser
response builders
DNS task
sync primitive init
start / stop / is_running
```

Parser should be separable enough that unit tests can exercise packet functions without creating sockets.

---

## 20. Logging requirements

### 20.1 Must log

- state transitions;
- generated SoftAP SSID;
- AP IPv4 used for captive DNS;
- DNS start/bind success/failure;
- DNS stop timeout;
- boot credential connect retries;
- credential testing start/result;
- NVS save failure reason;
- restart scheduled/already scheduled;
- runtime reconnect exhausted.

### 20.2 Must not log

- Wi-Fi password;
- raw credential payload;
- NVS password value.

SSID may be logged.

---

## 21. Test strategy

Tests được chia thành ba lớp; không trộn unit và hardware integration.

### 21.1 Layer A — pure DNS parser unit tests

Không cần Wi-Fi hardware/socket.

| ID | Input | Expected |
|---|---|---|
| DNS-U01 | valid `A example.com`, `QDCOUNT=1` | parse success |
| DNS-U02 | `QDCOUNT=0` | unsupported/reject safely |
| DNS-U03 | `QDCOUNT=2` | unsupported/reject safely |
| DNS-U04 | malformed label length > remaining bytes | reject, no OOB |
| DNS-U05 | compressed QNAME pointer in request | reject safely if unsupported |
| DNS-U06 | `QR=1` packet | reject as non-query |
| DNS-U07 | non-zero OPCODE | NOTIMP/reject per builder contract |
| DNS-U08 | `A/IN` | one A answer |
| DNS-U09 | `AAAA/IN` | NOERROR, zero answers |
| DNS-U10 | other type/IN | NOERROR, zero answers |
| DNS-U11 | class != IN | NOTIMP |
| DNS-U12 | `local.adguard.org` | NXDOMAIN |
| DNS-U13 | `local.adguard.com` | NXDOMAIN |
| DNS-U14 | response flags | QR=1, RA=0, RD copied |
| DNS-U15 | redirect IP changed | answer uses passed IP, not 192.168.4.1 constant |

### 21.2 Layer B — DNS task/component tests

Chạy FreeRTOS/lwIP environment phù hợp.

| ID | Test | Expected |
|---|---|---|
| DNS-C01 | start once | bind success, running true |
| DNS-C02 | start twice same IP | one task/socket |
| DNS-C03 | stop once | returns after socket closed |
| DNS-C04 | stop twice | success/idempotent |
| DNS-C05 | stop then immediate start | no duplicate bind, new task runs |
| DNS-C06 | bind port occupied | start returns error, running false |
| DNS-C07 | start task create fails | no leaked STARTING state |
| DNS-C08 | stop timeout injection | returns `ESP_ERR_TIMEOUT`, logs error |

### 21.3 Layer C — Wi-Fi hardware integration tests

#### WIFI-I01 — factory fresh boot

1. erase NVS;
2. boot;
3. state becomes `PROVISIONING`;
4. unique SoftAP appears;
5. DNS A query resolves to actual AP IPv4;
6. provisioning HTTP page reachable.

#### WIFI-I02 — valid saved credential boot

1. valid NVS credentials;
2. reboot;
3. state `BOOT_CONNECTING -> CONNECTED`;
4. no SoftAP;
5. DNS not running;
6. `wifi_prov_is_connected() == true`.

#### WIFI-I03 — invalid saved credential boot

1. invalid NVS credential;
2. boot retry/timeout exhausted;
3. state -> `PROVISIONING`;
4. SoftAP + DNS available.

#### WIFI-I04 — valid new credential

1. start in provisioning;
2. submit valid credential;
3. state `PROVISIONING -> TESTING -> RESTART_PENDING`;
4. credential saved after `GOT_IP`;
5. SoftAP remains visible before scheduled restart;
6. DNS remains running before restart;
7. `/api/wifi` can report success;
8. scheduled reboot occurs;
9. new boot reaches `CONNECTED` and full gateway services start.

#### WIFI-I05 — wrong password

1. provisioning;
2. submit wrong password;
3. timeout/retries exhausted;
4. state returns `PROVISIONING`;
5. SoftAP remains;
6. DNS remains;
7. wrong credential is not committed.

#### WIFI-I06 — NVS save failure after GOT_IP

1. inject/mock NVS write failure if test harness supports;
2. credential connect succeeds;
3. save fails;
4. state returns `PROVISIONING`;
5. STA test disconnected;
6. AP/DNS remain.

#### WIFI-I07 — scan serialization

- start scan then submit credential concurrently;
- only one operation proceeds;
- other receives busy/conflict path;
- no driver crash.

#### WIFI-I08 — restart scheduling idempotency

- after `RESTART_PENDING`, call schedule twice;
- only one restart task created.

#### WIFI-I09 — runtime disconnect

1. boot normal and reach `CONNECTED`;
2. disconnect router;
3. state -> `RECONNECTING`;
4. no SoftAP starts;
5. reconnect success -> `CONNECTED`, or exhausted -> `FAILED`;
6. no provisioning HTTP mode assumption is introduced.

#### WIFI-I10 — multiple gateways

- boot two gateways in provisioning nearby;
- SSIDs differ by MAC suffix.

### 21.4 Build compatibility tests

Build toàn project, không chỉ component.

Must verify:

- `web_wifi_api.c` compiles unchanged;
- `main/main.c` compiles unchanged;
- no missing legacy symbol;
- component Kconfig values resolve;
- no new warning from signature mismatch.

---

## 22. Implementation sequence

Thực hiện theo thứ tự này để giảm regression surface.

### Phase 1 — DNS parser correctness

1. add endian helpers;
2. fix `QDCOUNT`;
3. parse QTYPE/QCLASS;
4. correct response flags (`RA=0`);
5. preserve AdGuard NXDOMAIN;
6. replace hard-coded answer IP with passed redirect IP;
7. add Layer A tests.

**Gate:** không sang Phase 2 nếu DNS-U01..U15 chưa pass.

### Phase 2 — DNS lifecycle

1. change DNS public API to `esp_err_t` + redirect IP;
2. add lifecycle enum + mutex + event group;
3. make start wait for bind result;
4. make stop wait for cleanup;
5. unify task cleanup path;
6. add Layer B tests.

**Gate:** repeated start/stop không duplicate task/socket.

### Phase 3 — Wi-Fi state model

1. extend enum with `UNINITIALIZED`, `BOOT_CONNECTING`, `PROVISIONING`, `TESTING`, `RESTART_PENDING`, `RECONNECTING`;
2. add `set_state()`;
3. add `s_sta_has_ip` as physical fact;
4. add narrow `s_sta_retry_enabled` to suppress retries on intentional disconnect;
5. remove/reduce broad booleans `s_provisioning_active`, `s_testing_credentials`, `s_connect_requested` where state can replace them;
6. remove fallback task if boot flow can own fallback synchronously;
7. adjust event handler by state/retry intent.

**Gate:** state transitions in WIFI-I01..I05 match spec.

### Phase 4 — Provisioning success sequencing

1. credential success -> `RESTART_PENDING`, not `CONNECTED`;
2. keep AP/DNS active;
3. retain legacy restart API;
4. verify current Web worker still polls success then restarts;
5. verify reboot normal path starts full gateway.

**Gate:** WIFI-I04 passes end-to-end.

### Phase 5 — Hardening

1. add Kconfig;
2. generate MAC-suffixed SSID;
3. cleanup ownership paths;
4. optional clear-credentials API;
5. runtime reconnect state;
6. full build + hardware suite.

---

## 23. Definition of Done

Refactor chỉ được merge khi **tất cả** điều kiện sau đúng.

### 23.1 Functional

- [ ] DNS standard query với `QDCOUNT=1` được xử lý đúng.
- [ ] DNS AAAA không nhận invalid synthetic A answer.
- [ ] DNS response không set `RA=1`.
- [ ] AdGuard special-case vẫn NXDOMAIN.
- [ ] DNS answer dùng actual AP IPv4.
- [ ] DNS start chỉ success sau bind success.
- [ ] DNS stop chỉ success sau task/socket cleanup.
- [ ] Không race start/stop đã biết.
- [ ] Boot valid credential -> `CONNECTED`, STA only.
- [ ] Boot invalid/missing credential -> `PROVISIONING`.
- [ ] Valid new credential -> `RESTART_PENDING`, AP/DNS vẫn active đến reboot.
- [ ] Wrong credential -> quay `PROVISIONING`, không persist.
- [ ] NVS failure -> quay `PROVISIONING`, không state mơ hồ.
- [ ] Runtime disconnect không tự mở provisioning portal.

### 23.2 Compatibility

- [ ] `web_wifi_api.c` build unchanged.
- [ ] `main/main.c` build unchanged.
- [ ] `wifi_prov_test_and_save()` còn tồn tại.
- [ ] `wifi_prov_save_and_connect()` còn tồn tại.
- [ ] `wifi_prov_schedule_restart()` còn tồn tại.
- [ ] legacy error codes vẫn đúng với Web error mapping hiện tại.

### 23.3 Reliability

- [ ] init failure cleanup không leak owned resources.
- [ ] init lần hai không duplicate handler/netif/task.
- [ ] scan/test credentials serialized.
- [ ] no password logging.
- [ ] repeated DNS start/stop stable.

### 23.4 Verification

- [ ] Layer A tests pass.
- [ ] Layer B tests pass.
- [ ] WIFI-I01..I10 pass hoặc có documented exception được reviewer chấp nhận.
- [ ] full project build clean.
- [ ] manual provisioning từ phone/laptop pass.

---

## 24. Non-goals / follow-up work

Các work item sau **không được lén đưa vào refactor này**:

1. hot-transition provisioning Web server -> gateway Web server không reboot;
2. tự start BLE/MCP sau provisioning success;
3. full captive portal detection endpoint cho Android/iOS/macOS/Windows;
4. credential record migration sang versioned NVS blob;
5. TLS/auth;
6. dynamic long-running runtime network recovery strategy;
7. factory-reset UX ở Web UI.

Nếu muốn bỏ reboot trong tương lai, cần một design riêng bao gồm ít nhất:

- lifecycle manager ở application layer;
- stop provisioning HTTP server;
- start gateway modules;
- start gateway Web server + MCP;
- xử lý existing socket/client teardown;
- failure rollback.

---

## 25. Implementation notes for reviewer

Reviewer nên tập trung vào các invariants sau:

### Invariant A — provisioning reachability

Trong mọi state:

```text
PROVISIONING / TESTING / RESTART_PENDING
```

SoftAP không được biến mất do credential worker.

### Invariant B — persistence timing

Credential không được commit trước `GOT_IP`.

### Invariant C — DNS lifecycle

Không bao giờ tồn tại hai DNS task cùng có khả năng bind/own UDP 53.

### Invariant D — boot-only fallback

Chỉ boot-connect failure được tự chuyển sang provisioning. Runtime disconnect không đổi HTTP service mode ngầm.

### Invariant E — compatibility

Current Web provisioning worker phải compile và giữ behavior success -> delayed restart.

---

## 26. Final architecture after this refactor

```text
                          +---------------------------+
                          |       app_main()          |
                          +-------------+-------------+
                                        |
                              wifi_prov_init()
                                        |
                  +---------------------+----------------------+
                  |                                            |
          provisioning boot                              connected boot
                  |                                            |
        AP + DNS + provisioning UI                  STA + full gateway services
                  |
          credential submitted
                  |
              TESTING
           +------+------+
           |             |
         fail          success
           |             |
     PROVISIONING     save NVS
                         |
                  RESTART_PENDING
                  AP + DNS remain
                         |
                    delayed reboot
                         |
                      BOOT
                         |
                    CONNECTED
                         |
                full gateway services
```

Refactor này sửa correctness, lifecycle và state consistency mà không buộc application phải hỗ trợ hot reconfiguration của toàn bộ gateway stack.
