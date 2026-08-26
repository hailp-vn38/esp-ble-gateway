# Wi-Fi Provisioning Captive Portal Enhancement Specification

**Project:** ESP32 BLE Gateway  
**Repository:** `hailp-vn38/esp-ble-gateway`  
**Primary component:** `components/wifi_provisioning`  
**Integration component:** `components/web_server`  
**Target:** ESP32-S3 / ESP-IDF native  
**Document type:** Implementation specification + review + test matrix  
**Version:** 3.0  
**Date:** 2026-08-26  
**Status:** Ready for implementation

---

## 1. Purpose

Tài liệu này định nghĩa bước phát triển tiếp theo cho Wi-Fi onboarding của ESP32 BLE Gateway:

> Khi gateway đang ở provisioning mode và phát SoftAP, sau khi người dùng kết nối vào SoftAP, hệ điều hành nên tự nhận diện captive portal và mở trang cấu hình Wi-Fi với xác suất cao nhất có thể trên Android, iOS/iPadOS, macOS và Windows.

Tài liệu dựa trên code hiện tại của:

- `components/wifi_provisioning/wifi_prov.c`
- `components/wifi_provisioning/dns_hijack.c`
- `components/wifi_provisioning/dns_packet.c`
- `components/wifi_provisioning/include/*.h`
- `components/wifi_provisioning/Kconfig`
- `components/wifi_provisioning/test/*`
- `components/web_server/web_assets.c`
- `components/web_server/web_server.c`
- `main/main.c`
- specification hiện tại `wifi_provisioning_refactor_spec_v2.md`

V3 này không thay thế các invariant state-machine của V2. Nó mở rộng V2 để captive portal trở thành behavior chính thức thay vì phần ngoài phạm vi.

---

## 2. Executive review

### 2.1 Kết luận

Implementation hiện tại đã có nền tảng captive portal tốt:

1. provisioning mode tạo SoftAP;
2. component lấy IPv4 của SoftAP;
3. DNS server bind UDP/53;
4. DNS A query được trả về IP SoftAP;
5. provisioning web server có root page;
6. có các captive-probe route phổ biến:
   - `/generate_204`
   - `/hotspot-detect.html`
   - `/connecttest.txt`
   - `/ncsi.txt`;
7. probe route hiện redirect về `/`;
8. provisioning và gateway HTTP mode được tách rõ;
9. sau khi lưu credential thành công, SoftAP/DNS vẫn sống tới lúc delayed reboot.

Đây là kiến trúc đúng hướng.

### 2.2 Khoảng trống chính

Còn ba khoảng trống làm giảm độ tin cậy của auto-open portal:

**CP-01 — Không có generic HTTP funnel.**  
Chỉ các probe URI đã biết được redirect. Một probe URI khác hoặc browser request bất kỳ có thể nhận 404 thay vì bị đưa về portal.

**CP-02 — Chưa quảng bá Captive Portal URI qua DHCP Option 114.**  
ESP-IDF hỗ trợ `ESP_NETIF_CAPTIVEPORTAL_URI`. Đây là phương pháp hiện đại, tiêu chuẩn hơn và bổ trợ tốt cho DNS redirect.

**CP-03 — Captive redirect hiện trả body rỗng.**  
Nên trả body HTML/text nhỏ, `Cache-Control: no-store`, và redirect semantics rõ ràng. Điều này cũng giúp debug và tương thích tốt hơn với captive-network assistant.

### 2.3 Mức ưu tiên

| ID | Mức | Nội dung |
|---|---|---|
| CP-01 | P0 | Generic provisioning-only HTTP 404 redirect |
| CP-02 | P0 | DHCP Option 114 |
| CP-03 | P1 | Redirect response body + no-store |
| CP-04 | P1 | Captive portal integration tests |
| CP-05 | P1 | Device/OS manual test matrix |
| CP-06 | P2 | Diagnostics/logging cho captive path |

---

## 3. Current flow

```text
app_main()
   |
   v
wifi_prov_init()
   |
   +-- saved credentials valid
   |       |
   |       v
   |    CONNECTED
   |       |
   |       v
   |    gateway HTTP + BLE + MCP
   |
   +-- no credentials / boot connect failed
           |
           v
      enter_provisioning()
           |
           +-- ensure APSTA
           +-- configure SoftAP
           +-- wait SoftAP ready
           +-- read AP IPv4
           +-- dns_hijack_start(AP IPv4)
           |
           v
      PROVISIONING
           |
           v
app_main(): web_server_start_provisioning()
           |
           +-- /
           +-- known captive probes
           +-- Wi-Fi API
```

### 3.1 Current DNS behavior

`dns_hijack.c`:

- tạo UDP socket;
- bind vào IP SoftAP, port 53;
- nhận DNS packet;
- gọi `dns_build_response()`;
- gửi response về client;
- lifecycle RUNNING/STOPPING được quản lý độc lập.

`dns_packet.c`:

- parse DNS header;
- chấp nhận một question;
- A record của domain thông thường trả IP SoftAP;
- AAAA và qtype khác trả NOERROR/0 answers;
- query malformed bị drop;
- một số AdGuard local domains được loại trừ.

Kết luận: không cần viết lại DNS architecture chỉ để đạt yêu cầu auto-open portal.

---

## 4. Target captive portal architecture

Target flow:

```text
                         PROVISIONING MODE
                               |
                               v
                       Configure SoftAP
                               |
                               v
                         SoftAP IPv4
                         192.168.4.1
                               |
             +-----------------+------------------+
             |                                    |
             v                                    v
      Captive DNS                           DHCP server
      UDP/53                                Option 114
      *.domain -> AP IP                     Portal URI
             |                              http://AP-IP/
             |                                    |
             +-----------------+------------------+
                               |
                               v
                     Provisioning HTTP server
                               |
               +---------------+----------------+
               |               |                |
               v               v                v
              `/`        known probes       unknown URI
               |               |                |
               |               v                v
               |          303 -> `/`       404 handler
               |                                |
               +---------------+----------------+
                               |
                               v
                          setup.html
```

### 4.1 Defense-in-depth

Hai mechanism phải chạy song song:

1. **DNS funnel + HTTP redirect**
2. **DHCP Option 114**

Không coi Option 114 là lý do xóa DNS hijack.

Lý do:

- client cũ có thể không hỗ trợ Option 114;
- captive-assistant behavior khác nhau theo OS/version;
- DNS funnel vẫn hữu ích cho browser HTTP request;
- Option 114 bổ sung khả năng phát hiện portal tiêu chuẩn hơn.

---

## 5. Locked design decisions

### 5.1 Không chuyển HTTP server vào `wifi_provisioning`

Boundary giữ nguyên:

```text
wifi_provisioning
    owns:
      - Wi-Fi driver lifecycle
      - AP/STA mode
      - AP netif
      - DHCP captive option
      - DNS captive service
      - provisioning workflow state
      - credential verification
      - reconnect policy

web_server
    owns:
      - HTTP server
      - captive HTTP redirects
      - setup page
      - REST API
      - HTTP status/header/body behavior
```

### 5.2 Captive behavior chỉ tồn tại trong provisioning HTTP mode

Không đăng ký captive 404 handler trong gateway mode.

Gateway mode phải tiếp tục trả 404 bình thường cho URI không tồn tại.

### 5.3 Provisioning thành công vẫn reboot

Không hot-switch provisioning HTTP server sang gateway HTTP server.

Flow giữ:

```text
PROVISIONING
   -> TESTING
   -> save NVS
   -> RESTART_PENDING
   -> delayed reboot
   -> normal boot
   -> CONNECTED
```

### 5.4 SoftAP + DNS + DHCP captive option phải còn hoạt động trong RESTART_PENDING

Web UI phải đủ thời gian:

- nhận API success;
- hiển thị trạng thái;
- polling trạng thái nếu cần;
- hoàn tất response trước reboot.

### 5.5 Không bắt buộc auto-popup 100%

Acceptance criterion là implementation đúng protocol và hoạt động trên test matrix.

Không được định nghĩa requirement là “mọi thiết bị bắt buộc tự popup”, vì OS có thể:

- tắt captive-network assistant;
- bật VPN;
- dùng Private DNS;
- có DNS/security filter;
- dùng HTTPS-only;
- cache network classification;
- thay đổi behavior theo OS version.

---

## 6. DHCP Option 114 design

### 6.1 ESP-IDF API

Dùng:

```c
esp_netif_dhcps_option(
    s_ap_netif,
    ESP_NETIF_OP_SET,
    ESP_NETIF_CAPTIVEPORTAL_URI,
    portal_uri,
    strlen(portal_uri));
```

Portal URI:

```text
http://<softap-ip>/
```

Ví dụ:

```text
http://192.168.4.1/
```

### 6.2 Critical lifetime requirement

ESP-IDF giữ pointer tới Captive Portal URI; application phải bảo đảm URI vẫn hợp lệ trong toàn bộ lifetime của DHCP server.

**Không được:**

```c
static esp_err_t configure_captive_dhcp(void)
{
    char uri[32];
    ...
    esp_netif_dhcps_option(..., uri, strlen(uri));
    return ESP_OK; // BUG: uri hết lifetime
}
```

### 6.3 Required storage

Thêm persistent component storage:

```c
#define WIFI_PROV_PORTAL_URI_MAX_LEN 32

static char s_captive_portal_uri[WIFI_PROV_PORTAL_URI_MAX_LEN];
```

Buffer này sống cùng component lifetime.

### 6.4 Configuration sequence

Recommended:

```text
SoftAP netif exists
   |
SoftAP IP available
   |
build s_captive_portal_uri
   |
esp_netif_dhcps_stop(s_ap_netif)
   |
set ESP_NETIF_CAPTIVEPORTAL_URI
   |
esp_netif_dhcps_start(s_ap_netif)
```

### 6.5 Error policy

DHCP Option 114 failure **không được làm provisioning chết hoàn toàn** nếu:

- SoftAP hoạt động;
- DNS hijack hoạt động;
- HTTP portal hoạt động.

Policy:

```text
Option 114 success -> normal
Option 114 failure -> warning + continue with DNS captive mode
```

Exception: lỗi cho thấy AP netif/DHCP server itself không usable thì xử lý theo lỗi SoftAP/network layer tương ứng.

### 6.6 Idempotency

Helper phải an toàn khi được gọi lại với cùng AP IP.

Recommended API nội bộ:

```c
static esp_err_t configure_captive_portal_dhcp(
    esp_netif_t *ap_netif,
    const esp_ip4_addr_t *ap_ip);
```

---

## 7. HTTP captive funnel design

### 7.1 Known probe routes

Giữ các route hiện tại:

```text
/generate_204
/hotspot-detect.html
/connecttest.txt
/ncsi.txt
```

Không xóa chúng sau khi thêm 404 handler.

Lý do: explicit routes:

- dễ log;
- dễ unit/integration test;
- giúp phân loại OS probe;
- không phụ thuộc hoàn toàn vào error handler.

### 7.2 Generic 404 handler

Khi provisioning server khởi động:

```c
httpd_register_err_handler(
    server,
    HTTPD_404_NOT_FOUND,
    captive_404_handler);
```

Chỉ đăng ký ở:

```c
web_server_start_provisioning()
```

Không đăng ký trong:

```c
web_server_start()
```

### 7.3 Redirect response

Recommended response:

```text
HTTP/1.1 303 See Other
Location: /
Cache-Control: no-store
Content-Type: text/html; charset=utf-8
```

Body tối thiểu:

```html
<!doctype html>
<meta http-equiv="refresh" content="0;url=/">
<a href="/">Open setup</a>
```

Không cần JavaScript.

### 7.4 Why 303

303 biểu đạt rõ:

- request cần chuyển tới portal landing page;
- browser thực hiện GET tới location;
- không vô tình preserve method của request trước.

Known OS probes là GET, nên 302 cũng có thể hoạt động. Tuy nhiên spec này khóa 303 để behavior rõ ràng và thống nhất.

### 7.5 Cache policy

Captive redirect:

```text
Cache-Control: no-store
```

Portal root:

```text
Cache-Control: no-cache
```

Không cho OS/browser cache captive redirect lâu dài.

### 7.6 Security headers

Redirect response không cần full CSP, nhưng nên tiếp tục:

```text
X-Content-Type-Options: nosniff
Referrer-Policy: no-referrer
```

Root setup page giữ CSP hiện tại.

---

## 8. Required file changes

## 8.1 `components/wifi_provisioning/wifi_prov.c`

### Add

```c
static char s_captive_portal_uri[32];
```

Helper:

```c
static esp_err_t configure_captive_portal_dhcp(
    esp_netif_t *ap_netif,
    const esp_ip4_addr_t *ap_ip);
```

### Modify `enter_provisioning()`

Current logical block:

```text
get AP IP
   -> start DNS
```

Target:

```text
get AP IP
   |
   +-- configure DHCP Option 114
   |
   +-- start captive DNS
```

Recommended failure handling:

```text
AP IP read fails
   -> DNS cannot be configured
   -> Option 114 cannot be configured
   -> provisioning HTTP may still be reachable by default AP IP,
      but component must log degraded mode clearly

Option 114 fails
   -> log warning
   -> continue DNS

DNS fails
   -> log warning
   -> continue Option 114 + direct IP HTTP
```

### Add diagnostic log

Example:

```text
Provisioning captive portal:
  SSID=ESP-GW-A1B2C3
  AP_IP=192.168.4.1
  DHCP_114=enabled
  DNS=running
  PORTAL=http://192.168.4.1/
```

Không log AP password.

---

## 8.2 `components/wifi_provisioning/Kconfig`

Add:

```kconfig
config WIFI_PROV_CAPTIVE_DHCP_OPTION_114
    bool "Advertise captive portal URI using DHCP Option 114"
    default y
```

Option này giúp:

- feature-toggle khi debug client compatibility;
- A/B test DNS-only vs DNS+DHCP;
- disable nếu ESP-IDF target/version cụ thể có issue.

Recommended default: `y`.

Không thêm toggle cho DNS hijack trong phase này; DNS vẫn là baseline compatibility mechanism.

---

## 8.3 `components/web_server/web_assets.c`

### Keep

- root provisioning handler;
- favicon;
- four known captive routes.

### Replace captive redirect implementation

Target helper:

```c
static esp_err_t captive_redirect_handler(httpd_req_t *request);
```

Required behavior:

- 303;
- Location `/`;
- no-store;
- non-empty body;
- security headers.

### Add 404 handler

Example API shape:

```c
esp_err_t web_assets_register_provisioning_captive_errors(
    httpd_handle_t server);
```

Hoặc đăng ký trực tiếp trong provisioning registrar nếu module boundary phù hợp.

Preferred ownership: `web_assets.c` định nghĩa handler; `web_server.c` quyết định lúc nào register.

---

## 8.4 `components/web_server/web_server.c`

After provisioning routes register successfully:

```c
httpd_register_err_handler(
    server,
    HTTPD_404_NOT_FOUND,
    captive_404_handler);
```

Architecture requirement:

```text
Gateway server:
   no captive 404 handler

Provisioning server:
   captive 404 handler enabled
```

Nếu API hiện tại không expose handler, thêm registrar/helper riêng thay vì leak static symbol không cần thiết.

### Route budget

404 error handler không phải normal URI route; tuy nhiên implementation phải xác nhận không làm thay đổi `max_uri_handlers`.

Known captive routes vẫn nằm trong provisioning route count.

---

## 8.5 `components/web_server/web_modules.h`

Nếu cần, thêm public registration function scoped cho web_server internals, ví dụ:

```c
esp_err_t web_assets_register_provisioning_errors(httpd_handle_t server);
```

Không expose API này ra public component header nếu chỉ web_server dùng.

---

## 8.6 Tests

Existing tests hiện tập trung vào:

- DNS packet parser;
- DNS lifecycle.

Cần bổ sung captive integration tests.

Suggested files:

```text
components/wifi_provisioning/test/
    test_dns_parser.c
    test_dns_lifecycle.c
    test_captive_dhcp.c          # new, nếu dependency test cho phép
```

Web side:

```text
components/web_server/test/
    test_captive_http.c          # new
```

Nếu test infrastructure không thuận tiện để instantiate real `esp_http_server`, captive HTTP test có thể bắt đầu ở integration/device test thay vì fake quá sâu ESP-IDF internals.

---

## 9. State machine invariants after enhancement

### Invariant CP-A

`WIFI_PROV_STATE_PROVISIONING` nghĩa là:

- SoftAP intended active;
- provisioning HTTP intended active từ app layer;
- captive infrastructure intended active:
  - DHCP Option 114 nếu enabled/configured;
  - DNS hijack nếu start thành công.

### Invariant CP-B

`TESTING` không được teardown:

- SoftAP;
- DNS;
- DHCP captive URI;
- provisioning HTTP.

STA credential verification chạy song song APSTA.

### Invariant CP-C

`RESTART_PENDING` vẫn giữ portal reachable tới reboot.

### Invariant CP-D

Runtime `CONNECTED -> RECONNECTING` không bật captive portal.

### Invariant CP-E

Không để Wi-Fi event callback trực tiếp start/stop HTTP server.

---

## 10. Detailed target sequence

### 10.1 Fresh device / no credentials

```text
BOOT
 |
wifi_prov_init()
 |
create STA/AP netifs
 |
Wi-Fi init/start
 |
no valid credential
 |
ensure APSTA
 |
configure SoftAP
 |
wait AP ready
 |
get AP IPv4
 |
+--> configure DHCP Option 114
|
+--> start DNS hijack
|
state = PROVISIONING
 |
return
 |
app_main()
 |
web_server_start_provisioning()
 |
register setup routes
 |
register captive 404
 |
READY
```

### 10.2 User joins SoftAP

```text
Client
 |
DHCP DISCOVER
 |
ESP32 DHCP OFFER
 |  gateway = AP IP
 |  DNS = AP IP
 |  option 114 = http://AP-IP/
 |
client classifies network
 |
+-- understands Option 114 --> opens portal URI
|
+-- performs connectivity probe
       |
       DNS probe host -> AP IP
       |
       HTTP GET probe path
       |
       explicit route OR captive 404
       |
       303 /
       |
       setup page
```

### 10.3 User submits credentials

```text
POST Wi-Fi credentials
 |
wifi_prov_test_and_save()
 |
state PROVISIONING -> TESTING
 |
STA connect under APSTA
 |
+-- fail
|    |
|    stop STA attempt
|    state -> PROVISIONING
|    AP/DNS/DHCP/HTTP remain alive
|
+-- GOT_IP
     |
     save NVS
     |
     state -> RESTART_PENDING
     |
     API success response
     |
     delayed restart
     |
     reboot
```

---

## 11. HTTP behavior table

| Request | Provisioning mode | Gateway mode |
|---|---|---|
| `GET /` | setup page | dashboard |
| `GET /generate_204` | 303 `/` | normal 404 unless explicitly used |
| `GET /hotspot-detect.html` | 303 `/` | normal 404 |
| `GET /connecttest.txt` | 303 `/` | normal 404 |
| `GET /ncsi.txt` | 303 `/` | normal 404 |
| `GET /random-path` | 303 `/` via captive 404 | normal 404 |
| `GET /favicon.ico` | 204 | normal favicon behavior |
| `GET /api/wifi/...` | API handler | mode-specific/API policy |
| invalid unsupported method | normal HTTP behavior; do not blindly hide API/method errors | normal HTTP behavior |

Important: generic captive handler áp dụng cho **404 URI not found**, không biến mọi 4xx/5xx thành redirect. API validation error phải giữ error semantics.

---

## 12. DNS behavior review

### Keep

- A wildcard behavior;
- single-question parser constraints;
- explicit malformed packet rejection;
- lifecycle synchronization;
- AdGuard local-domain exception nếu vẫn cần cho UI environment hiện tại.

### Do not add

- HTTPS interception;
- fake TLS certificates;
- TCP proxy;
- arbitrary packet forwarding.

### AAAA

Current AAAA response is NOERROR/0 answer.

Giữ behavior này trong change set hiện tại trừ khi device test chỉ ra OS cụ thể không trigger vì AAAA handling.

Không tự trả A-data trong AAAA response.

---

## 13. Failure-mode specification

| Failure | Expected behavior |
|---|---|
| SoftAP config fails | provisioning fails |
| AP IP cannot be read | log degraded/failure; no DNS/114 config possible |
| DHCPS stop returns state warning | evaluate current state, continue safely |
| Option 114 set fails | warning, DNS fallback remains |
| DHCPS restart fails | high severity; client DHCP may be affected |
| DNS task fails to start | warning, Option 114 + direct IP remain |
| HTTP server fails | provisioning unusable; app logs error |
| 404 handler registration fails | fail provisioning web-server startup rather than silently claim captive readiness |
| credential test fails | return to PROVISIONING; captive services remain |
| NVS save fails | return to PROVISIONING; captive services remain |
| delayed reboot task creation fails | remain RESTART_PENDING/recoverable, log error |
| client uses HTTPS probe | no TLS interception; rely on Option 114 / other probes |

---

## 14. Logging requirements

Minimum logs:

```text
wifi_prov:
  Generated SoftAP SSID
  SoftAP IPv4
  DHCP Option 114 configured / failed
  Captive portal URI
  DNS start state
  provisioning workflow state transition

web_server:
  provisioning server started
  captive 404 handler registered

web_assets:
  known captive probe URI
  unknown URI captive redirect (debug or rate-limited)
```

Avoid logging every DNS packet at INFO under normal production because captive clients can generate substantial traffic.

Recommended:

- lifecycle = INFO
- individual DNS query = DEBUG
- malformed DNS = DEBUG/WARN with rate limiting
- Option 114 failure = WARN/ERROR depending failure stage

---

## 15. Security considerations

Captive portal is intentionally HTTP.

Do not attempt HTTPS MITM.

Provisioning AP can remain WPA2 PSK according to project configuration.

Future hardening items, ngoài scope:

- per-device temporary AP password;
- QR onboarding;
- provisioning session token;
- CSRF protection;
- rate limiting;
- AP auto-timeout;
- physical-button gated provisioning mode.

Current enhancement must not weaken existing CSP/security headers of setup page.

---

## 16. Unit test requirements

### 16.1 DNS parser

Existing coverage should continue validating:

- valid A query;
- malformed header;
- malformed qname;
- unsupported opcode;
- multiple questions unsupported;
- non-IN class;
- AAAA behavior;
- case-insensitive domain parsing;
- response IP;
- packet bounds;
- AdGuard exception.

### 16.2 DNS lifecycle

Continue validating:

- start;
- repeated start same IP;
- restart with changed IP;
- stop;
- repeated stop;
- start while stopping;
- start failure;
- timeout behavior.

### 16.3 DHCP portal URI helper

Test where practical:

- AP IP formats correctly;
- URI includes `http://`;
- trailing `/`;
- no overflow;
- persistent buffer retains content;
- invalid netif/IP rejected;
- feature disabled path;
- idempotent repeated config.

If ESP-IDF DHCPS cannot be easily mocked without fragile tests, move API-call verification to target integration test and keep URI builder unit-tested.

---

## 17. HTTP integration test requirements

Required cases:

### HTTP-01
`GET /` -> `200`, setup HTML.

### HTTP-02
`GET /generate_204` -> `303`, `Location: /`.

### HTTP-03
`GET /hotspot-detect.html` -> `303`, `Location: /`.

### HTTP-04
`GET /connecttest.txt` -> `303`, `Location: /`.

### HTTP-05
`GET /ncsi.txt` -> `303`, `Location: /`.

### HTTP-06
`GET /this-path-does-not-exist` -> `303`, `Location: /`.

### HTTP-07
Redirect has:

```text
Cache-Control: no-store
```

### HTTP-08
Redirect body is non-empty.

### HTTP-09
Gateway-mode unknown URI remains 404.

### HTTP-10
Invalid Wi-Fi API request remains API error, not captive redirect.

---

## 18. Device/OS test matrix

A result must record:

- device model;
- OS version;
- test date;
- AP security mode;
- whether popup auto-opened;
- probe path observed;
- time from join to portal;
- whether manual browser fallback worked;
- logs.

### 18.1 Android

| Case | Scenario | Expected |
|---|---|---|
| AND-01 | Fresh join, mobile data ON | portal detected/opened or captive notification appears |
| AND-02 | Fresh join, mobile data OFF | portal detected |
| AND-03 | reconnect same AP | portal still reachable; behavior may be cached |
| AND-04 | VPN enabled | record behavior; direct AP IP must remain usable |
| AND-05 | Private DNS enabled | Option 114 should improve robustness; fallback documented |
| AND-06 | manual `http://AP-IP/` | setup page loads |

### 18.2 iPhone / iPad

| Case | Scenario | Expected |
|---|---|---|
| IOS-01 | Fresh join | captive assistant opens setup page |
| IOS-02 | reconnect after Forget Network | assistant triggers again |
| IOS-03 | cellular ON | portal remains usable |
| IOS-04 | Private Relay/network filtering context | record; manual AP IP fallback works |
| IOS-05 | credential test fails | portal stays open/reachable |
| IOS-06 | success -> restart pending | success response visible before reboot |

### 18.3 macOS

| Case | Scenario | Expected |
|---|---|---|
| MAC-01 | fresh join | captive assistant or captive indication |
| MAC-02 | browser arbitrary HTTP host | redirected to `/` |
| MAC-03 | arbitrary unknown URI | captive 404 redirects |
| MAC-04 | reconnect | portal remains reachable |
| MAC-05 | manual AP IP | setup page loads |

### 18.4 Windows 10/11

| Case | Scenario | Expected |
|---|---|---|
| WIN-01 | fresh join | network marked action needed / sign-in |
| WIN-02 | connect test probe | redirected |
| WIN-03 | NCSI request | redirected |
| WIN-04 | browser arbitrary HTTP URL | redirected |
| WIN-05 | manual AP IP | setup page loads |

---

## 19. Network-level verification

Use packet capture where practical.

### 19.1 DHCP

Verify DHCP Offer/ACK contains:

```text
Option 114
URI = http://192.168.4.1/
```

### 19.2 DNS

Query:

```text
A connectivity-check-domain
```

Expected:

```text
answer = SoftAP IPv4
```

### 19.3 HTTP

Unknown path:

```text
GET /abc123
```

Expected:

```text
303 See Other
Location: /
Cache-Control: no-store
```

### 19.4 HTTPS

Expected:

- no successful transparent redirect;
- no fake TLS response;
- client must rely on Option 114 or HTTP captive probe.

---

## 20. Acceptance criteria

Implementation is accepted when all P0 items pass:

- [ ] SoftAP provisioning still works with no saved credentials.
- [ ] Saved-credential boot success still bypasses provisioning.
- [ ] Boot credential failure still falls back to provisioning according to existing policy.
- [ ] DNS hijack still starts in provisioning.
- [ ] DHCP Option 114 is present when feature is enabled.
- [ ] Captive URI points to actual SoftAP IPv4.
- [ ] URI storage lifetime is component/static, not stack-local.
- [ ] Known captive probe routes redirect to `/`.
- [ ] Unknown HTTP URI in provisioning redirects to `/`.
- [ ] Unknown HTTP URI in gateway mode remains 404.
- [ ] Captive redirect uses no-store.
- [ ] Captive redirect has non-empty body.
- [ ] Credential testing does not tear down AP/DNS/DHCP portal.
- [ ] Failed credential test returns to provisioning and page remains reachable.
- [ ] Successful test enters RESTART_PENDING and page remains reachable until reboot.
- [ ] DNS parser/lifecycle tests remain green.
- [ ] At least one current Android device passes onboarding.
- [ ] At least one current iOS/iPadOS device passes onboarding.
- [ ] At least one Windows 10/11 device passes onboarding.
- [ ] macOS test completed if macOS is a supported onboarding client.

---

## 21. Suggested implementation order

### Phase A — DHCP captive advertisement

1. add Kconfig;
2. add persistent portal URI buffer;
3. add URI builder/helper;
4. configure Option 114 after AP IP is known;
5. verify DHCP packet;
6. test DNS regression.

### Phase B — HTTP funnel

1. improve captive redirect response;
2. add generic 404 captive handler;
3. register only in provisioning mode;
4. verify gateway 404 unchanged;
5. test known probes and random path.

### Phase C — Device validation

1. Android;
2. iOS/iPadOS;
3. Windows;
4. macOS;
5. record failures and OS-specific quirks.

### Phase D — hardening

1. logging rate limits;
2. failure-path cleanup;
3. documentation;
4. regression suite.

---

## 22. Code-shape recommendation

### `wifi_prov.c`

```c
#define WIFI_PROV_CAPTIVE_URI_MAX_LEN 32

static char s_captive_portal_uri[WIFI_PROV_CAPTIVE_URI_MAX_LEN];

static esp_err_t configure_captive_portal_dhcp(
    esp_netif_t *netif,
    const esp_ip4_addr_t *ip)
{
#if CONFIG_WIFI_PROV_CAPTIVE_DHCP_OPTION_114
    if (netif == NULL || ip == NULL || ip->addr == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char ip_text[16];
    esp_ip4addr_ntoa(ip, ip_text, sizeof(ip_text));

    int written = snprintf(
        s_captive_portal_uri,
        sizeof(s_captive_portal_uri),
        "http://%s/",
        ip_text);

    if (written <= 0 || written >= sizeof(s_captive_portal_uri)) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = esp_netif_dhcps_stop(netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        return err;
    }

    err = esp_netif_dhcps_option(
        netif,
        ESP_NETIF_OP_SET,
        ESP_NETIF_CAPTIVEPORTAL_URI,
        s_captive_portal_uri,
        strlen(s_captive_portal_uri));

    esp_err_t start_err = esp_netif_dhcps_start(netif);

    if (err != ESP_OK) return err;
    return start_err;
#else
    return ESP_OK;
#endif
}
```

Note: exact ESP-IDF error constants must be checked against the project-pinned ESP-IDF version during implementation. Do not blindly copy an error constant if unavailable.

### `web_assets.c`

```c
static esp_err_t send_captive_redirect(httpd_req_t *request)
{
    static const char body[] =
        "<!doctype html><meta http-equiv=\"refresh\" content=\"0;url=/\">"
        "<a href=\"/\">Open setup</a>";

    httpd_resp_set_status(request, "303 See Other");
    httpd_resp_set_hdr(request, "Location", "/");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    set_security_headers(request);
    return httpd_resp_send(request, body, HTTPD_RESP_USE_STRLEN);
}
```

404 handler can call the same helper.

---

## 23. Compatibility with `wifi_provisioning_refactor_spec_v2.md`

V2 explicitly placed “captive portal OS-specific HTTP redirect handlers” outside scope.

V3 changes this decision:

```text
V2:
  captive HTTP OS integration = out of scope

V3:
  captive auto-open behavior = required onboarding feature
```

All V2 state-machine rules remain authoritative unless explicitly overridden in V3.

In particular, keep:

- reboot-after-provisioning;
- AP/DNS alive during test/save/restart pending;
- boot-only auto fallback;
- runtime disconnect does not hot-open provisioning;
- legacy Wi-Fi API compatibility;
- separate physical `s_sta_has_ip` and workflow state.

---

## 24. Risks

### R1 — OS behavior changes

Mitigation:

- Option 114 + DNS + HTTP funnel;
- test matrix;
- manual AP-IP fallback.

### R2 — DHCP URI lifetime bug

Severity: high.

Mitigation:

- static/component-owned buffer;
- explicit code review item;
- target packet verification.

### R3 — Redirect leaks into gateway mode

Severity: high because it masks real 404s and breaks API/debug behavior.

Mitigation:

- registration only in `web_server_start_provisioning()`;
- gateway 404 regression test.

### R4 — Portal disappears during STA credential test

Mitigation:

- preserve APSTA;
- do not stop DNS/DHCP;
- state invariants CP-B/CP-C.

### R5 — HTTPS expectations

Mitigation:

- explicitly document no HTTPS interception;
- use standards-based DHCP advertisement.

---

## 25. Definition of Done

Change set is Done when:

1. implementation matches Section 20;
2. tests are committed/passing;
3. packet capture confirms Option 114;
4. provisioning unknown-URI funnel works;
5. gateway-mode 404 semantics are unchanged;
6. at least Android + iOS + Windows manual onboarding tests are documented;
7. existing V2 lifecycle invariants have no regression;
8. README/spec documents manual fallback:
   `http://<SoftAP-IP>/`.

---

## 26. Review conclusion

Current code should **not** be rewritten from scratch.

The correct next change is an incremental enhancement:

```text
Existing:
SoftAP + DNS hijack + known HTTP probes

Add:
DHCP Option 114
+ provisioning-only generic HTTP 404 redirect
+ stronger redirect response
+ OS/device validation
```

This gives the project a conventional, layered captive-portal implementation while preserving the existing provisioning state machine and component boundaries.

---

## 27. External technical basis

This design was cross-checked against the current ESP-IDF captive portal implementation and API behavior as of 2026-08-26:

- ESP-IDF captive portal example demonstrates DNS wildcard funnel + HTTP 404 redirect.
- The same example optionally advertises the captive portal via DHCP Option 114.
- ESP-IDF documents `ESP_NETIF_CAPTIVEPORTAL_URI` as DHCP option 114.
- ESP-IDF API documentation states that the captive portal URI pointer is application-owned and must remain valid for the DHCP server lifetime.
- ESP-IDF captive portal documentation describes DHCP Option 114 and DNS/HTTP funnel as complementary approaches.

Implementation must still compile-check against the exact ESP-IDF version pinned by this repository before merging.
