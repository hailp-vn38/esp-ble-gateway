# Web Dashboard Settings Development Spec

**Version:** 1.1  
**Status:** Implementation Ready — reviewed baseline  
**Date:** 2026-08-28  
**Project:** ESP32 BLE Gateway  
**Repository:** `hailp-vn38/esp-ble-gateway`  
**Target:** ESP32-S3 / ESP-IDF 6.1-rc1  

## Revision 1.1 — review update

Bản v1.1 giữ nguyên security model của v1.0 và bổ sung các thay đổi bắt buộc sau khi review lại tài liệu đối chiếu code hiện tại:

- module hóa Web UI source vì `dashboard.html` hiện đã khoảng 89.8 KB và chứa cả markup lẫn JavaScript;
- module hóa backend `web_server`, đặc biệt tách `web_gateway_api.c` khỏi mô hình một file chứa device/command/capability/MCP exposure;
- chọn **build-time assembly/bundling**, không fetch HTML partial hoặc JavaScript module riêng ở runtime; số module source không được làm tăng số HTTP URI handler;
- thêm `www_src/` làm source-of-truth và sinh asset vào build directory; generated asset không được chỉnh tay hoặc commit như source;
- cập nhật Tailwind content scan để quét các file HTML/JS đã tách;
- bỏ route `/i18n.js` khỏi thiết kế v1.0: i18n được bundle vào dashboard/login generated asset;
- tính lại route budget: baseline khoảng 23 + 7 route mới = khoảng 30 handler; giữ target `WEB_GATEWAY_MAX_URI_HANDLERS = 36`;
- thêm Phase 0 refactor Web UI/Web Server trước khi phát triển Settings để giảm rủi ro merge và regression;
- chốt `403 Forbidden` cho current-password sai trong sensitive operation và `409 Conflict` khi gọi login trong auth-disabled mode.

## Revision 1.1a — post-review refinements

Bổ sung các điểm làm rõ sau review:

- **PBKDF2 iterations benchmark target**: Thêm target ≤300ms cho 1 lần verify password trên ESP32-S3; nếu benchmark vượt ngưỡng, giảm iterations thay vì chấp nhận UX chậm. (§9.3)
- **NVS atomicity cho enable auth**: Khi enable auth lần đầu, nếu commit `enabled=1` thành công nhưng credential persist fail, rollback `enabled` về 0 và trả lỗi. Không để auth ở trạng thái ON mà không có credential. (§11.2, §35.1)
- **MCP token prefix static**: Token prefix `mcp_` là tiện ích dev UX, không phải security mechanism. Nếu attacker xác nhận gateway đang chạy MCP, nó không làm tăng attack surface vì `/mcp` endpoint đã detectable. Giữ nguyên prefix để improve developer experience. (§20.1)
- **Route budget verification**: Bắt buộc đếm exact route count từ source code (không ước tính) trước khi update `WEB_GATEWAY_MAX_URI_HANDLERS`. Nếu baseline thực tế >23, cập nhật budget tương ứng. (§28)
- **Origin check fallback**: Khi không có `Origin` header trong request (legitimate cross-origin hoặc same-origin browser behavior), cho phép request thay vì reject. Chỉ reject khi `Origin` có mặt và mismatch với `Host`. (§23.4)
- **Generated asset embedding**: Verify rằng `EMBED_FILES` trong CMakeLists.txt accept file paths từ `${CMAKE_CURRENT_BINARY_DIR}` thay vì source tree. Nếu ESP-IDF không hỗ trợ, fallback sang copy generated files vào `www/` trước embed. (§26.7, §33)
- **Session timeout warning**: Khi gateway không có RTC battery, tất cả sessions mất sau reboot. UI Settings nên hiển thị brief note rằng "active sessions will end when gateway restarts" khi auth ON. (§10.4)

---

# 1. Mục tiêu

Tài liệu này định nghĩa kiến trúc và kế hoạch triển khai **Gateway Settings v1** cho Web Dashboard của ESP32 BLE Gateway.

Phạm vi chức năng của Settings v1:

1. cấu hình ngôn ngữ Web UI;
2. cấu hình Web UI chạy **có hoặc không cần đăng nhập**;
3. khi bật Web Auth: quản lý username, password, đổi password và browser session;
4. quản lý MCP access token: xem trạng thái, tạo token, rotate token và hiển thị hướng dẫn kết nối MCP;
5. giữ nguyên System Information, Network Status và Restart hiện có;
6. loại bỏ cơ chế `MCP Admin Token` hiện tại trong browser;
7. chuyển API quản lý MCP tool exposure sang dùng Web Auth/session;
8. **không tạo tab MCP riêng**;
9. MCP tool exposure của từng device tiếp tục nằm trong **Device Detail**.

Mục tiêu kiến trúc cuối cùng:

```text
                        ESP32 BLE Gateway
                               |
              +----------------+----------------+
              |                                 |
         Web Dashboard                         MCP
              |                                 |
      optional Web Auth                    MCP Bearer Token
              |                                 |
              v                                 v
        Web API Layer                      /mcp endpoint
              |                                 |
              +---------- shared services ------+
                         / command system
```

---

# 2. Không nằm trong phạm vi Settings v1

Không triển khai trong giai đoạn này:

- tab MCP riêng trong sidebar;
- danh sách MCP tools toàn gateway trong Settings;
- chọn command expose MCP trong Settings;
- multi-user;
- user roles / RBAC;
- OAuth;
- cloud account;
- HTTPS/TLS;
- password recovery qua email/cloud;
- remote administration qua Internet;
- browser remember-password custom;
- authentication cho provisioning captive portal;
- thay đổi protocol MCP hoặc tool catalog ngoài phần token authentication;
- thay đổi luồng Device Detail MCP exposure đã triển khai.

Các command của device được expose thành MCP tool tiếp tục quản lý tại:

```text
My Devices
    -> Device Detail
        -> Device Commands
            -> MCP tools
```

Settings chỉ quản lý **MCP access vào gateway**, không quản lý **tool nào được expose**.

---

# 3. Baseline hiện tại của project

Tài liệu này được xây dựng dựa trên implementation hiện tại trên branch `main`.

## 3.1 Web Dashboard

Dashboard hiện có ba navigation item chính:

```text
My Devices
Add Device
Gateway Settings
```

Đây là navigation target cần giữ.

`Gateway Settings` hiện đã hiển thị:

- System Information;
- Network Status;
- Restart Gateway;
- một card `MCP Admin Token`.

Dashboard hiện phần lớn nằm trong:

```text
components/web_server/www/dashboard.html
```

File hiện khoảng **89,767 bytes** và chứa đồng thời:

- navigation + toàn bộ view markup;
- Device Detail;
- Scanner;
- Settings;
- modal;
- API wrapper;
- state/navigation/UI logic;
- device/scanner/settings JavaScript.

Đây đã là monolith frontend. Settings v1 không được tiếp tục mở rộng trực tiếp file này mà phải refactor source thành module trước hoặc cùng lúc với implementation.

Backend cũng có dấu hiệu tương tự: `web_gateway_api.c` hiện khoảng **31,411 bytes** và chứa chung device API, command API, capability API và MCP exposure API. Settings v1 không được tiếp tục đưa thêm handler vào file này.

## 3.2 MCP tool exposure

Device Detail đã có UI chọn các command được expose thành MCP tools.

Backend exposure API hiện dùng:

```text
GET /api/mcp/exposures
PUT /api/mcp/exposures
```

và mutation hiện được bảo vệ bằng `web_admin_auth`.

## 3.3 Web admin auth hiện tại

Hiện project có:

```text
components/web_server/web_admin_auth.c
components/web_server/web_admin_auth.h
```

với:

```text
namespace: web_admin
key:       token
```

Dashboard lưu token này vào browser `localStorage` dưới dạng:

```text
mcp_admin_token
```

Token này chỉ là token quản trị Web API cho MCP exposure, không phải MCP token mà Agent dùng gọi `/mcp`.

Cơ chế này phải được loại bỏ sau khi Web Auth hoàn tất.

## 3.4 MCP auth hiện tại

`components/mcp_endpoint/mcp_auth.c` hiện xác thực:

```http
Authorization: Bearer <token>
```

Token runtime được đọc từ:

```text
NVS namespace: mcp
key:           token
```

và fallback về:

```text
CONFIG_MCP_AUTH_TOKEN
```

Nếu không có token, implementation hiện tại chạy dev mode không authentication.

Settings v1 phải quản lý đúng **MCP access token này**, không dùng `web_admin_auth` làm MCP credential.

## 3.5 HTTP route budget

`components/web_server/web_server.c` hiện cấu hình:

```c
#define WEB_GATEWAY_MAX_URI_HANDLERS 28
```

Baseline route count hiện được project theo dõi khoảng 23 handlers, bao gồm assets, gateway APIs, system APIs, BLE APIs, MCP endpoint và exposure APIs.

Settings/Auth mới sẽ làm số route vượt 28 nếu không cập nhật budget.

---

# 4. Nguyên tắc kiến trúc bắt buộc

## 4.1 Web Auth và MCP Auth độc lập

Không được dùng một credential cho cả Dashboard và MCP.

```text
Web Dashboard
    -> username + password
    -> browser session

MCP Client / AI Agent
    -> Bearer token
    -> /mcp
```

`Web Auth OFF` không được tự động tắt MCP authentication.

Ví dụ cấu hình hợp lệ:

```text
Web UI authentication: OFF
MCP token: configured
```

hoặc:

```text
Web UI authentication: ON
MCP token: configured
```

## 4.2 Web Auth là optional

Web UI phải hỗ trợ hai mode:

```text
OPEN MODE
Web auth = OFF
-> browser truy cập dashboard trực tiếp
-> Web APIs không yêu cầu session

PROTECTED MODE
Web auth = ON
-> browser phải login
-> Web APIs yêu cầu session
```

Default khi nâng cấp firmware:

```text
Web auth = OFF
```

để không khóa người dùng hiện tại khỏi dashboard.

## 4.3 MCP tool exposure không thuộc Settings

Giữ boundary:

```text
Settings / MCP Access
= Agent dùng credential nào để vào gateway

Device Detail / MCP tools
= Agent được phép gọi command nào của device
```

Không đưa device list hoặc command exposure về Settings.

## 4.4 Web layer không truy cập trực tiếp secret storage của MCP

`web_server` không được tự `nvs_open("mcp", ...)`.

Phải gọi public API của `mcp_endpoint` hoặc một MCP auth management service.

Ví dụ:

```text
web_settings_api
       |
       v
mcp_auth_token_get_status()
mcp_auth_token_rotate()
       |
       v
mcp_endpoint owns NVS
```

## 4.5 Không lưu secret trong browser localStorage

Sau refactor không lưu:

- MCP admin token;
- MCP bearer token;
- Web password;
- Web session token trong JavaScript storage.

Web session sử dụng `HttpOnly` cookie.

MCP token chỉ được hiển thị khi generate/rotate để user copy sang MCP client.

---

## 4.6 Web UI source phải modular, runtime phải gọn

Phân biệt rõ hai khái niệm:

```text
SOURCE MODULARITY
= nhiều file nhỏ cho developer bảo trì

RUNTIME ASSET COUNT
= ít HTTP asset/route cho ESP32 phục vụ browser
```

Không được giải quyết file HTML dài bằng cách để browser runtime fetch hàng loạt:

```text
/views/settings.html
/views/devices.html
/js/settings.js
/js/devices.js
...
```

vì cách này làm tăng:

- URI handlers;
- HTTP requests;
- coupling với asset registration;
- startup/route budget;
- failure surface trên thiết bị nhúng.

Target bắt buộc là:

```text
modular source files
      |
      | build-time assemble / bundle
      v
1 generated dashboard asset
1 generated login asset
      |
      v
ESP32 embedded assets
```

Generated asset chỉ là build output, không phải source-of-truth.

---

# 5. UX tổng thể của Gateway Settings

Navigation sidebar giữ:

```text
ESP32 Gateway

My Devices
Add Device
Gateway Settings
```

Không thêm `MCP` tab.

Trang `Gateway Settings` gồm các card/section:

```text
Gateway Settings
|
+-- General
|    +-- Language
|
+-- Web Access
|    +-- Require login ON/OFF
|    +-- Username
|    +-- Change password
|    +-- Logout khi auth đang bật
|
+-- MCP Access
|    +-- Endpoint
|    +-- Authentication status
|    +-- Generate token / Rotate token
|    +-- Connection guide
|
+-- System Information
|
+-- Network Status
|
+-- Danger Zone
     +-- Restart Gateway
```

System Information, Network Status và Restart hiện có được giữ lại.

---

# 6. General Settings - Language

## 6.1 Supported languages v1

```text
auto
en
vi
```

UI:

```text
Language
[ Auto                     v ]

Auto
English
Tiếng Việt
```

## 6.2 Storage decision

Language là preference của browser/UI, không phải behavior của gateway firmware.

Settings v1 lưu language vào browser `localStorage`, không ghi NVS:

```text
key: esp32-gateway.language
```

Lợi ích:

- không ghi flash cho UI preference;
- nhiều browser có thể dùng ngôn ngữ khác nhau;
- không cần thêm API backend chỉ để đổi ngôn ngữ;
- đổi ngôn ngữ áp dụng ngay, không reboot.

Đây là dữ liệu không nhạy cảm nên `localStorage` phù hợp.

## 6.3 Auto language

Khi:

```text
language = auto
```

frontend đọc:

```javascript
navigator.language
```

Mapping tối thiểu:

```text
vi-* -> vi
khác -> en
```

Fallback:

```text
en
```

## 6.4 i18n architecture

Không tạo nhiều bản HTML:

```text
dashboard_en.html
dashboard_vi.html
```

Dùng một DOM + translation dictionary.

Đề xuất source file:

```text
components/web_server/www_src/shared/i18n.js
```

File này được build-time bundle vào generated dashboard/login asset; v1.1 **không register `/i18n.js` runtime route**.

Cấu trúc:

```javascript
const I18N = {
    en: {
        "nav.devices": "My Devices",
        "nav.add_device": "Add Device",
        "nav.settings": "Gateway Settings",
        "settings.web_access": "Web Access"
    },
    vi: {
        "nav.devices": "Thiết bị",
        "nav.add_device": "Thêm thiết bị",
        "nav.settings": "Cài đặt Gateway",
        "settings.web_access": "Truy cập Web"
    }
};
```

HTML ưu tiên:

```html
<span data-i18n="nav.settings">Gateway Settings</span>
```

Frontend helper:

```text
i18n.init()
i18n.setLanguage(language)
i18n.t(key)
i18n.apply(document)
```

## 6.5 API error localization

Backend không gửi message làm khóa logic UI.

Response lỗi mới phải có machine-readable code:

```json
{
  "success": false,
  "error": {
    "code": "auth_invalid_credentials"
  }
}
```

Frontend dịch:

```text
errors.auth_invalid_credentials
```

`message` có thể giữ cho debug/backward compatibility nhưng UI không được phụ thuộc vào exact English text.

---

# 7. Web Authentication Architecture

## 7.1 Component mới

Tạo component độc lập:

```text
components/web_auth/
|
+-- CMakeLists.txt
+-- Kconfig.projbuild
+-- include/
|    +-- web_auth.h
+-- web_auth.c
+-- web_auth_store.c
+-- web_auth_password.c
+-- web_auth_session.c
+-- test/
     +-- CMakeLists.txt
     +-- test_web_auth.c
```

Boundary:

```text
web_server
    -> HTTP routes / cookies / JSON

web_auth
    -> credential policy
    -> password verification
    -> session management
    -> NVS persistence
```

`web_auth` không phụ thuộc vào dashboard HTML.

## 7.2 Public API đề xuất

```c
typedef struct {
    bool enabled;
    bool credentials_configured;
    char username[33];
} web_auth_status_t;

typedef enum {
    WEB_AUTH_OK = 0,
    WEB_AUTH_REQUIRED,
    WEB_AUTH_INVALID_CREDENTIALS,
    WEB_AUTH_RATE_LIMITED,
    WEB_AUTH_NOT_CONFIGURED,
    WEB_AUTH_STORAGE_ERROR,
} web_auth_result_t;

esp_err_t web_auth_init(void);

esp_err_t web_auth_get_status(web_auth_status_t *out);

web_auth_result_t web_auth_login(const char *username,
                                 const char *password,
                                 char *session_token,
                                 size_t session_token_size);

web_auth_result_t web_auth_validate_session(const char *session_token);

void web_auth_logout(const char *session_token);
void web_auth_invalidate_all_sessions(void);

web_auth_result_t web_auth_enable(const char *username,
                                  const char *current_password,
                                  const char *new_password);

web_auth_result_t web_auth_disable(const char *current_password);

web_auth_result_t web_auth_change_username(const char *current_password,
                                           const char *new_username);

web_auth_result_t web_auth_change_password(const char *current_password,
                                           const char *new_password);
```

Exact function naming có thể thay đổi nhưng boundary phải giữ.

---

# 8. Web Auth NVS schema

Namespace:

```text
web_auth
```

Keys v1:

| Key | Type | Mục đích |
|---|---|---|
| `ver` | u8 | schema version |
| `enabled` | u8 | 0/1 |
| `username` | string | administrator username |
| `pwd_salt` | blob 16 B | random salt |
| `pwd_hash` | blob 32 B | derived password hash |
| `pwd_iter` | u32 | PBKDF2 iteration count |

Không lưu plaintext password.

Không lưu browser session vào NVS.

## 8.1 Credential persistence khi auth OFF

Khi user disable Web Auth:

```text
enabled = 0
```

nhưng giữ:

```text
username
pwd_salt
pwd_hash
pwd_iter
```

Lợi ích:

- user có thể bật lại auth mà không phải tạo account mới;
- disable auth không đồng nghĩa xóa credential.

Nếu muốn xóa credential hoàn toàn, để phase sau hoặc factory reset.

---

# 9. Username / password policy

## 9.1 Username

V1 chỉ hỗ trợ một administrator.

Rules:

```text
length: 3..32 bytes
allowed: A-Z a-z 0-9 . _ -
```

UI có thể prefill:

```text
admin
```

nhưng backend không hardcode password.

## 9.2 Password

Rules:

```text
minimum: 8 bytes
maximum: 64 bytes
```

Không yêu cầu rule phức tạp kiểu uppercase/symbol vì dễ gây UX kém và không tăng bảo mật đáng kể cho LAN gateway.

Backend luôn enforce độ dài.

## 9.3 Password hashing

Dùng primitive từ mbedTLS, không tự viết password hash algorithm.

Target:

```text
PBKDF2-HMAC-SHA256
salt:        16 random bytes
output:      32 bytes
iterations:  configurable
```

Kconfig đề xuất:

```text
CONFIG_WEB_AUTH_PBKDF2_ITERATIONS
```

default ban đầu:

```text
60000
```

Target benchmark trên ESP32-S3:

```text
1 lần verify password ≤ 300ms
```

Nếu benchmark vượt 300ms, giảm iterations (tối thiểu 10,000) thay vì chấp nhận UX login chậm. Ghi lại iterations thực tế đã test trong release notes.

Unit test không được hardcode assumption về thời gian thực thi.

## 9.4 Random generation

Salt, session token và MCP token phải dùng ESP-IDF CSPRNG:

```c
esp_fill_random(...)
```

Không dùng:

```c
rand()
random()
time-based token
MAC-derived token
```

---

# 10. Web Session

## 10.1 Session token

Sau login thành công tạo:

```text
32 random bytes
-> base64url without padding
-> ~43 characters
```

Browser nhận cookie:

```http
Set-Cookie: GWSESSION=<token>; HttpOnly; SameSite=Strict; Path=/
```

Gateway hiện chạy HTTP nên không set `Secure` cho đến khi HTTPS được triển khai.

## 10.2 Session storage

Session chỉ tồn tại RAM.

Đề xuất:

```c
#define WEB_AUTH_MAX_SESSIONS 4

typedef struct {
    bool active;
    uint8_t token_hash[32];
    int64_t created_us;
    int64_t last_seen_us;
} web_auth_session_t;
```

Không cần lưu raw session token trong RAM; lưu SHA-256 token.

## 10.3 Timeout

Kconfig đề xuất:

```text
CONFIG_WEB_AUTH_SESSION_IDLE_MINUTES     default 30
CONFIG_WEB_AUTH_SESSION_MAX_HOURS        default 12
CONFIG_WEB_AUTH_MAX_SESSIONS             default 4
```

Session invalid khi:

```text
idle timeout exceeded
OR absolute lifetime exceeded
OR user logout
OR password changed
OR username changed
OR auth disabled
OR gateway reboot
```

## 10.4 Reboot behavior

Vì session ở RAM:

```text
Gateway reboot
-> all sessions lost
-> nếu auth ON, browser phải login lại
```

Đây là behavior mong muốn.

UI Settings khi auth ON nên hiển thị brief note:

```text
Active sessions will end when the gateway restarts.
```

để user hiểu rõ behavior expected.

---

# 11. Web Auth state transitions

## 11.1 Default / upgrade

Sau firmware update nếu chưa có `web_auth` namespace:

```text
enabled = false
credentials_configured = false
```

Dashboard hoạt động giống hiện tại.

## 11.2 OFF -> ON lần đầu

Nếu chưa có credential:

UI yêu cầu:

```text
Username
Password
Confirm password
```

Backend flow:

```text
validate inputs
    -> generate salt
    -> derive hash
    -> persist credential (username + pwd_salt + pwd_hash + pwd_iter)
    -> verify persistence succeeded
    -> set enabled=1
    -> commit
```

Không được set `enabled=1` trước khi credential persist thành công.

Nếu credential persist thành công nhưng `enabled=1` commit fail:

```text
-> rollback enabled về 0
-> trả lỗi auth_storage_error
-> credential vừa persist sẽ được clean ở boot tiếp theo hoặc factory reset
```

Mục tiêu: tránh lockout do NVS write failure.

## 11.3 OFF -> ON khi credential đã tồn tại

Nếu auth trước đó từng được bật rồi tắt:

UI yêu cầu:

```text
Current password
```

Backend verify credential cũ trước khi bật lại.

Điều này giảm nguy cơ một client khác trên LAN mở dashboard khi auth OFF rồi bật auth và khóa owner khỏi gateway.

## 11.4 ON -> OFF

Phải yêu cầu:

```text
Current password
```

sau khi thành công:

```text
enabled=0
invalidate all sessions
```

Response trả success trước khi browser chuyển sang open mode.

## 11.5 Change username

Khi auth ON:

```text
session valid
AND current password valid
```

Sau khi đổi username:

```text
invalidate all sessions
```

browser quay về login.

Khi auth OFF nhưng credentials tồn tại, vẫn yêu cầu current password để đổi username.

## 11.6 Change password

Khi auth ON:

```text
session valid
AND current password valid
AND new password valid
```

Sau khi persist hash mới:

```text
invalidate all sessions
```

UI redirect `/login`.

Khi auth OFF nhưng credentials tồn tại, vẫn yêu cầu current password.

---

# 12. Login brute-force protection

Implement rate limiter đơn giản trong RAM.

Baseline:

```text
5 failed logins / 60 seconds
```

Sau khi vượt ngưỡng:

```text
HTTP 429 Too Many Requests
```

Không cần persistent ban state qua reboot.

Không log:

- password;
- password hash;
- session token;
- MCP token.

Có thể log:

```text
login failed
login rate limited
auth enabled
auth disabled
password changed
```

nhưng không log credential value.

---

# 13. HTTP auth middleware

Tạo helper tại web layer:

```c
web_auth_result_t web_auth_require_request(httpd_req_t *req);
```

Behavior:

```text
if Web Auth OFF:
    return OK

if Web Auth ON:
    read GWSESSION cookie
    validate session
    return OK / REQUIRED
```

Không duplicate auth logic trong từng handler.

Có thể đặt HTTP-cookie parsing trong:

```text
components/web_server/web_auth_http.c
```

và core credential/session logic vẫn ở `web_auth` component.

---

# 14. Route access policy

## 14.1 Public routes

Luôn public:

```text
GET /login
POST /api/auth/login
GET /dashboard.css
GET /icons.css
GET /assets/Phosphor.woff2
GET /favicon.ico
```

`POST /api/auth/login` chỉ meaningful khi auth ON.

Nếu auth OFF và client vẫn gọi login API, backend phải trả cố định:

```text
409 Conflict
auth_disabled
```

Không dùng redirect cho API JSON.

## 14.2 Dashboard root

`GET /`:

```text
Web Auth OFF
    -> dashboard

Web Auth ON + valid session
    -> dashboard

Web Auth ON + invalid/no session
    -> 303 /login
```

## 14.3 Protected Web APIs

Khi auth ON, toàn bộ Web API quản trị phải yêu cầu session:

```text
/api/status
/api/devices
/api/command
/api/capabilities
/api/capabilities/refresh
/api/ble/*
/api/mcp/exposures
/api/settings
/api/auth/config
/api/auth/password
/api/auth/logout
/api/mcp/token
/api/restart hoặc system mutation tương ứng
```

Khi auth OFF, middleware trả OK và các API hoạt động không login.

## 14.4 MCP endpoint

`/mcp` không dùng `GWSESSION`.

Nó tiếp tục dùng MCP bearer auth riêng.

```text
POST /mcp
GET /mcp
DELETE /mcp
```

không được đi qua Web Auth middleware.

---

# 15. Login page

Tạo:

```text
components/web_server/www_src/login/shell.html
```

UI tối giản:

```text
+----------------------------------+
|          ESP32 Gateway           |
|                                  |
| Username                         |
| [____________________________]   |
|                                  |
| Password                         |
| [____________________________]   |
|                                  |
|          [ Sign In ]             |
+----------------------------------+
```

Yêu cầu:

- responsive;
- dùng cùng dashboard CSS/icon assets nếu phù hợp;
- không hiển thị firmware/network details trước login;
- không lưu password vào localStorage/sessionStorage;
- `autocomplete="username"` cho username;
- `autocomplete="current-password"` cho password;
- Enter submit form;
- lỗi login dùng error code dịch theo i18n;
- rate-limit response hiển thị rõ nhưng không reveal username có tồn tại hay không.

Error message chung:

```text
Invalid username or password
```

không tách:

```text
username does not exist
password incorrect
```

---

# 16. Web Access Settings UI

## 16.1 Auth OFF

```text
Web Access
------------------------------------------------
Require login                            [ OFF ]

Dashboard access
Open on local network

Anyone who can reach this gateway on the local
network can use the dashboard and Web APIs.

Credentials
Not configured

[ Enable authentication ]
------------------------------------------------
```

Nếu credential cũ đã tồn tại:

```text
Credentials
Configured for: admin

[ Enable authentication ]
[ Change password ]
```

## 16.2 Auth ON

```text
Web Access
------------------------------------------------
Require login                             [ ON ]

Dashboard access
Password protected

Username
admin

[ Change username ]
[ Change password ]
[ Disable authentication ]
[ Log out ]
------------------------------------------------
```

## 16.3 Warning khi OFF

Phải hiển thị rõ:

```text
When login is disabled, anyone on the local
network who can reach this gateway can change
settings and control connected devices.
```

Đây là expected behavior, không phải lỗi.

---

# 17. Web Auth API

## 17.1 POST /api/auth/login

Request:

```json
{
  "username": "admin",
  "password": "example-password"
}
```

Success:

```http
HTTP/1.1 200 OK
Set-Cookie: GWSESSION=<opaque>; HttpOnly; SameSite=Strict; Path=/
Content-Type: application/json
Cache-Control: no-store
```

```json
{
  "success": true
}
```

Invalid:

```http
401 Unauthorized
```

```json
{
  "success": false,
  "error": {
    "code": "auth_invalid_credentials"
  }
}
```

Rate limited:

```http
429 Too Many Requests
```

```json
{
  "success": false,
  "error": {
    "code": "auth_rate_limited"
  }
}
```

## 17.2 POST /api/auth/logout

Success:

```text
invalidate current session
expire GWSESSION cookie
```

Response:

```json
{
  "success": true
}
```

## 17.3 PUT /api/auth/config

Dùng cho:

- enable authentication;
- disable authentication;
- change username.

Không dùng endpoint này để đổi password thông thường.

### Enable first time

```json
{
  "enabled": true,
  "username": "admin",
  "new_password": "..."
}
```

### Re-enable existing credentials

```json
{
  "enabled": true,
  "current_password": "..."
}
```

### Disable

```json
{
  "enabled": false,
  "current_password": "..."
}
```

### Change username

```json
{
  "username": "gateway-admin",
  "current_password": "..."
}
```

Backend phải reject ambiguous payload hoặc unsupported combination.

## 17.4 PUT /api/auth/password

Request:

```json
{
  "current_password": "...",
  "new_password": "..."
}
```

Success:

```text
persist new hash
invalidate all sessions
expire current cookie
```

Response:

```json
{
  "success": true,
  "reauth_required": true
}
```

---

# 18. Settings API

## 18.1 GET /api/settings

Endpoint trả runtime state cần cho Settings page, không trả secrets.

Response đề xuất:

```json
{
  "success": true,
  "web_auth": {
    "enabled": true,
    "credentials_configured": true,
    "username": "admin"
  },
  "mcp": {
    "endpoint": "http://192.168.1.120/mcp",
    "auth_mode": "token",
    "token_configured": true,
    "token_hint": "...8f2a",
    "token_source": "runtime"
  }
}
```

Không trả:

```text
password hash
password salt
session token
full MCP token
```

Language không cần backend vì được lưu per-browser.

## 18.2 Cache policy

Các auth/settings response phải có:

```http
Cache-Control: no-store
```

---

# 19. MCP Access Settings

## 19.1 UI

```text
MCP Access
------------------------------------------------
Endpoint
http://192.168.1.120/mcp
[ Copy ]

Authentication
Protected by access token

Access token
Configured                         ...8f2a
[ Rotate Token ]

Connection
URL
http://192.168.1.120/mcp

Authorization
Bearer <MCP_TOKEN>

Transport
Streamable HTTP

[ Copy connection info ]
------------------------------------------------

MCP currently uses HTTP. Use it only on a trusted LAN.
```

Nếu chưa có token:

```text
Authentication
Unprotected / development mode

No MCP access token is configured.

[ Generate Token ]
```

Không thêm nút `Disable authentication` trong v1.

Nếu user muốn MCP không token, behavior legacy vẫn tồn tại khi chưa tạo token; UI phải warning rõ.

## 19.2 Endpoint generation

Frontend có thể dùng:

```javascript
window.location.origin + '/mcp'
```

Backend `GET /api/settings` cũng có thể trả endpoint để UI không tự suy đoán.

Không hardcode IP `192.168.x.x`.

---

# 20. MCP token lifecycle

## 20.1 Generate / rotate

Cả generate và rotate dùng cùng operation:

```http
POST /api/mcp/token
```

Backend:

```text
generate 32 random bytes
    -> base64url no padding
    -> prefix "mcp_"
    -> calculate SHA-256
    -> persist token hash + hint
    -> commit
    -> erase legacy plaintext NVS token if present
    -> return plaintext token exactly once
```

Token example:

```text
mcp_Kg7s...
```

Không cần exact length validation ở UI; backend sở hữu format.

Prefix `mcp_` là tiện ích dev UX, không phải security mechanism. Nếu attacker xác nhận gateway đang chạy MCP, nó không làm tăng attack surface vì `/mcp` endpoint đã detectable.

## 20.2 Response

```json
{
  "success": true,
  "token": "mcp_Kg7s...",
  "token_hint": "...7fa2"
}
```

Response phải:

```http
Cache-Control: no-store
```

UI mở modal:

```text
New MCP access token

mcp_Kg7s............................

Copy this token now. The gateway will not display
it again after this dialog is closed.

[ Copy ] [ Done ]
```

Sau khi dialog đóng, frontend phải xóa variable/token text khỏi state nếu có thể.

Không persist token vào localStorage.

## 20.3 Rotation semantics

Sau rotate thành công:

```text
old token -> invalid immediately
new token -> valid immediately
```

Không giữ grace period trong v1.

UI phải cảnh báo:

```text
Rotating the token will disconnect MCP clients
using the current token.
```

## 20.4 MCP token NVS schema mới

Namespace vẫn:

```text
mcp
```

Keys đề xuất:

| Key | Type | Mục đích |
|---|---|---|
| `token_ver` | u8 | storage version |
| `token_sha` | blob 32 B | SHA-256 of token |
| `token_hint` | string | masked suffix for UI |
| `token_gen` | u32 | generation counter |

Không lưu plaintext runtime token sau migration.

## 20.5 Legacy migration

Current implementation có plaintext:

```text
mcp/token
```

Migration order lúc `mcp_endpoint` init:

```text
1. if token_sha exists:
       use hashed runtime token

2. else if legacy NVS "token" exists:
       hash plaintext token
       persist token_sha + token_hint + token_ver
       erase legacy "token"
       commit

3. else if CONFIG_MCP_AUTH_TOKEN != "":
       use Kconfig token as firmware fallback

4. else:
       no token -> current dev/unprotected mode
```

Nếu migration write fail:

- không erase legacy token;
- tiếp tục verification bằng legacy token cho boot hiện tại;
- log error không chứa token.

## 20.6 Kconfig token behavior

Giữ:

```text
CONFIG_MCP_AUTH_TOKEN
```

làm development / firmware fallback.

Nếu Kconfig token đang active và user Rotate từ Web UI:

```text
runtime NVS token hash takes precedence
```

`GET /api/settings` có thể trả:

```text
token_source = firmware | runtime | none
```

nhưng không trả value.

---

# 21. MCP auth public management API

Không để `web_server` truy cập `mcp_auth.c` internal static state trực tiếp.

Thêm public API trong MCP component, ví dụ:

```c
typedef enum {
    MCP_AUTH_SOURCE_NONE = 0,
    MCP_AUTH_SOURCE_KCONFIG,
    MCP_AUTH_SOURCE_RUNTIME,
} mcp_auth_source_t;

typedef struct {
    bool configured;
    mcp_auth_source_t source;
    char token_hint[16];
    uint32_t generation;
} mcp_auth_status_t;

esp_err_t mcp_auth_get_status(mcp_auth_status_t *out);

esp_err_t mcp_auth_rotate_token(char *plaintext_out,
                                size_t plaintext_out_size,
                                mcp_auth_status_t *status_out);
```

Token verification trong `/mcp` và token management Web API phải dùng cùng storage/verification implementation.

Không duplicate SHA/token storage logic ở `web_server`.

---

# 22. Loại bỏ MCP Admin Token cũ

Sau khi Web Auth middleware hoạt động:

xóa:

```text
components/web_server/web_admin_auth.c
components/web_server/web_admin_auth.h
```

xóa Kconfig:

```text
CONFIG_WEB_ADMIN_AUTH_TOKEN
```

xóa NVS dependency runtime:

```text
web_admin/token
```

xóa frontend:

```text
localStorage['mcp_admin_token']
input-admin-token
token-status
settings.saveToken()
settings.clearToken()
settings.toggleTokenVisibility()
```

`GET/PUT /api/mcp/exposures` chuyển từ:

```text
web_admin_auth_check()
```

sang:

```text
web_auth_require_request()
```

Semantics:

```text
Web Auth ON
-> exposure API cần valid session

Web Auth OFF
-> exposure API open giống phần còn lại của dashboard
```

Điều này là intentional vì khi user chọn Web UI không cần auth, dashboard đang ở trusted-LAN admin mode.

---

# 23. CSRF / browser security

Gateway hiện chạy HTTP, vì vậy Web Auth không cung cấp confidentiality trên LAN.

Security scope:

```text
Web Auth
= access control cho browser/API
!= encryption
```

## 23.1 Required headers

Giữ security headers hiện có và bổ sung cho auth/settings response:

```text
Cache-Control: no-store
X-Content-Type-Options: nosniff
Referrer-Policy: no-referrer
```

## 23.2 Cookie

```text
HttpOnly
SameSite=Strict
Path=/
```

## 23.3 CORS

Không thêm wildcard CORS:

```text
Access-Control-Allow-Origin: *
```

cho admin APIs.

## 23.4 Origin check cho mutation

Đề xuất state-changing Web API khi auth ON kiểm tra `Origin` nếu header có mặt:

```text
Origin có trong request:
    Origin host == request Host
    mismatch -> 403 Forbidden

Origin không có trong request:
    cho phép (legitimate same-origin hoặc non-CORS browser request)
```

Không dùng Host/Origin như authentication; session vẫn là authentication source.

---

# 24. Recovery / lockout

Không triển khai `Forgot password` qua network.

Nếu user mất password khi auth ON, recovery v1 là physical/local recovery của gateway, tối thiểu:

```text
factory reset
```

Factory reset phải xóa `web_auth` namespace theo cơ chế reset configuration hiện tại của project.

Nếu project có physical reset flow qua `board_io`, có thể bổ sung trong phase sau:

```text
long press physical button
-> disable Web Auth / factory reset
```

Không tạo unauthenticated HTTP endpoint để reset password.

---

# 25. Dashboard frontend changes

## 25.1 State mới

Frontend thêm:

```javascript
state.settings = {
    webAuth: null,
    mcp: null
};
```

Không thêm MCP tool catalog global state vào Settings.

## 25.2 API wrapper

Bỏ:

```javascript
_getToken() {
    return localStorage.getItem('mcp_admin_token') || '';
}
```

và bỏ auto-attach:

```http
Authorization: Bearer <mcp_admin_token>
```

Browser session cookie được `fetch()` gửi same-origin tự động.

Nên gọi:

```javascript
fetch(path, {
    credentials: 'same-origin',
    ...
})
```

để semantics rõ ràng.

## 25.3 Global 401 handling

Nếu API trả:

```text
401 auth_required
```

frontend:

```text
window.location.href = '/login'
```

Không redirect khi error 401 đến từ `/mcp` vì dashboard không gọi MCP endpoint trực tiếp trong flow này.

## 25.4 MCP token modal

Plaintext token chỉ tồn tại trong modal generate/rotate.

Sau modal close:

```javascript
state.generatedMcpToken = null;
```

nếu có state tạm.

Không auto-log token.

---

# 26. Web UI & Web Server modularization

Đây là thay đổi bắt buộc của v1.1 trước khi tiếp tục mở rộng Settings.

## 26.1 Mục tiêu

Refactor để đạt đồng thời:

```text
Developer side:
small files + clear ownership + easy review/test

ESP32 runtime side:
few embedded assets + few URI handlers + no runtime template engine
```

Không đưa framework SPA, Node runtime hoặc template engine vào ESP32.

## 26.2 Source-of-truth frontend mới

Tạo cây source đề xuất:

```text
components/web_server/
|
+-- www_src/
|   |
|   +-- dashboard/
|   |   +-- shell.html
|   |   |
|   |   +-- views/
|   |   |   +-- devices.html
|   |   |   +-- device_detail.html
|   |   |   +-- scanner.html
|   |   |   +-- settings.html
|   |   |
|   |   +-- partials/
|   |   |   +-- sidebar.html
|   |   |   +-- modals.html
|   |   |   +-- restart_overlay.html
|   |   |
|   |   +-- js/
|   |       +-- core/
|   |       |   +-- state.js
|   |       |   +-- api.js
|   |       |   +-- nav.js
|   |       |   +-- ui.js
|   |       |
|   |       +-- features/
|   |           +-- devices.js
|   |           +-- scanner.js
|   |           +-- device_detail.js
|   |           +-- settings.js
|   |           +-- mcp_exposure.js
|   |
|   +-- login/
|   |   +-- shell.html
|   |   +-- login.js
|   |
|   +-- shared/
|       +-- i18n.js
|       +-- common.js
|
+-- tools/
|   +-- build_webui.py
|
+-- www/
    +-- dashboard.css
    +-- icons.css
    +-- assets/
```

`www_src/` là source-of-truth cho HTML/JS mới.

Không tạo lại một `www/dashboard.html` dài rồi tiếp tục chỉnh tay.

## 26.3 Build-time assembly

`build_webui.py` phải tạo output vào build directory:

```text
${CMAKE_CURRENT_BINARY_DIR}/dashboard.html
${CMAKE_CURRENT_BINARY_DIR}/login.html
```

Flow:

```text
shell.html
   + views/*.html
   + partials/*.html
   + ordered JS modules
   + shared/i18n.js
          |
          v
   build_webui.py
          |
          +--> dashboard.html
          +--> login.html
```

ESP32 chỉ embed output cuối.

Không runtime fetch HTML partial.

Không register route cho từng JS source module.

## 26.4 Include/placeholder format

Builder có thể dùng syntax tối giản, ví dụ:

```html
<!-- @include views/devices.html -->
<!-- @include views/device_detail.html -->
<!-- @include views/scanner.html -->
<!-- @include views/settings.html -->
<!-- @include partials/modals.html -->
```

JavaScript được concatenate theo manifest/order cố định rồi inject vào một script block trong generated dashboard.

Không dựa vào thứ tự filesystem/glob để quyết định JS execution order.

Ví dụ manifest trong Python hoặc file text:

```text
shared/common.js
shared/i18n.js
dashboard/js/core/state.js
dashboard/js/core/api.js
dashboard/js/core/nav.js
dashboard/js/core/ui.js
dashboard/js/features/devices.js
dashboard/js/features/scanner.js
dashboard/js/features/device_detail.js
dashboard/js/features/mcp_exposure.js
dashboard/js/features/settings.js
```

## 26.5 Compatibility với JavaScript hiện tại

Dashboard hiện dùng các global object và inline callbacks như:

```text
nav.switchTab(...)
devices.refreshCapabilities(...)
settings.restartGateway(...)
```

Để giảm scope/refactor risk, migration v1.1 được phép giữ compatibility bằng:

```javascript
window.nav = nav;
window.devices = devices;
window.settings = settings;
```

sau khi module được bundle.

Không bắt buộc chuyển toàn bộ `onclick` sang event delegation trong Settings v1.

Tuy nhiên code **mới** nên ưu tiên `addEventListener()` và không tăng thêm inline event handler nếu không cần thiết.

Việc xóa toàn bộ `'unsafe-inline'` khỏi CSP là hardening phase riêng.

## 26.6 Tailwind integration

Hiện Tailwind chỉ scan:

```text
./components/web_server/www/dashboard.html
```

Sau khi tách source, bắt buộc đổi `tailwind.config.js` để scan source modules, tối thiểu:

```javascript
content: [
    './components/web_server/www_src/**/*.html',
    './components/web_server/www_src/**/*.js'
]
```

Nếu không cập nhật, production CSS có thể thiếu class nằm trong partial/JS mới dù UI chạy đúng ở source.

Generated build output không nên là input chính của Tailwind vì source modules mới là canonical dependency.

## 26.7 CMake build dependency

CMake phải rebuild generated asset khi bất kỳ HTML/JS source thay đổi.

Pattern đề xuất:

```cmake
file(GLOB_RECURSE WEBUI_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/www_src/*.html"
    "${CMAKE_CURRENT_SOURCE_DIR}/www_src/*.js"
)

set(webui_builder "${CMAKE_CURRENT_SOURCE_DIR}/tools/build_webui.py")
set(dashboard_generated "${CMAKE_CURRENT_BINARY_DIR}/dashboard.html")
set(login_generated "${CMAKE_CURRENT_BINARY_DIR}/login.html")

add_custom_command(
    OUTPUT "${dashboard_generated}" "${login_generated}"
    COMMAND "${python}" "${webui_builder}"
            --source "${CMAKE_CURRENT_SOURCE_DIR}/www_src"
            --dashboard-out "${dashboard_generated}"
            --login-out "${login_generated}"
    DEPENDS ${WEBUI_SOURCES} "${webui_builder}"
    VERBATIM
)
```

Exact CLI có thể khác, nhưng dependency semantics trên là bắt buộc.

Không generate file vào source tree.

## 26.8 Builder validation

`build_webui.py` phải fail build nếu:

- include target không tồn tại;
- include recursion/cycle xảy ra;
- placeholder chưa được resolve;
- JS manifest trỏ tới file không tồn tại;
- generated dashboard/login rỗng;
- output không chứa marker/root element bắt buộc.

Khuyến nghị kiểm tra thêm duplicate critical DOM IDs trong assembled HTML.

Builder không cần minifier hoặc Node dependency ở v1.1.

## 26.9 Runtime asset policy

Sau assembly, gateway vẫn chỉ phục vụ số asset nhỏ:

```text
GET /
GET /login
GET /dashboard.css
GET /icons.css
GET /assets/Phosphor.woff2
GET /favicon.ico
```

Không có:

```text
GET /views/*.html
GET /js/*.js
GET /i18n.js
```

Source module count không được ảnh hưởng URI handler budget.

Nếu sau này cần cache JS riêng, chỉ tách thành **một** generated `dashboard.js` bundle, không expose từng source module.

## 26.10 Generated asset ownership

Generated files phải có banner nếu là text output:

```text
GENERATED FILE - DO NOT EDIT
source: components/web_server/www_src/
```

Khuyến nghị không commit generated `dashboard.html`/`login.html` vào repository.

Nếu build system bắt buộc cần committed artifact, CI phải verify artifact khớp source generation; tuy nhiên target ưu tiên vẫn là generate trong build directory.

## 26.11 Backend Web API modularization

Frontend modularization không đủ. `web_gateway_api.c` hiện đã gom quá nhiều domain.

Target split:

```text
components/web_server/
|
+-- web_server.c              # HTTP server lifecycle/config only
+-- web_http.c                # shared HTTP/JSON helpers
+-- web_auth_http.c           # cookie/session request integration
|
+-- web_gateway_api.c         # aggregator/registrar only
+-- web_device_api.c          # /api/devices
+-- web_command_api.c         # /api/command
+-- web_capability_api.c      # /api/capabilities*
+-- web_exposure_api.c        # /api/mcp/exposures
+-- web_auth_api.c            # /api/auth/*
+-- web_settings_api.c        # /api/settings + /api/mcp/token
|
+-- web_assets.c              # gateway/provisioning asset registrar
+-- web_modules.h
```

Không nhất thiết đổi public route contract khi tách file.

## 26.12 Registration boundary

`web_gateway_api.c` sau refactor chỉ orchestration:

```c
esp_err_t web_gateway_api_register(httpd_handle_t server)
{
    ESP_RETURN_ON_ERROR(web_device_api_register(server), TAG, "device API");
    ESP_RETURN_ON_ERROR(web_command_api_register(server), TAG, "command API");
    ESP_RETURN_ON_ERROR(web_capability_api_register(server), TAG, "capability API");
    ESP_RETURN_ON_ERROR(web_exposure_api_register(server), TAG, "exposure API");
    return ESP_OK;
}
```

Exact macro/helper tùy codebase, nhưng mọi child registrar phải propagate error.

Không được để route registration failure trở thành silent partial server.

## 26.13 Shared helper ownership

Logic dùng chung không copy giữa API modules.

Ví dụ:

```text
JSON body parsing       -> web_http.c
API error response      -> web_http.c
auth cookie parsing     -> web_auth_http.c
dispatch result mapping -> web_command_api/common helper
BLE address parsing     -> local helper hoặc shared helper nếu >=2 owner
```

Không tạo `web_utils.c` như một dumping ground không ownership.

## 26.14 Module dependency rule

Dependency direction target:

```text
web_server lifecycle
        |
        v
route registrars / HTTP adapters
        |
        v
web_auth / gateway services / mcp public API
```

Không để:

```text
mcp_endpoint -> web_server
web_auth core -> esp_http_server request objects
service layer -> dashboard-specific JSON schema
```

`web_auth` core tiếp tục độc lập HTTP; `web_auth_http.c` là adapter.

## 26.15 Refactor sequencing

Thực hiện refactor theo thứ tự ít rủi ro:

```text
1. split dashboard source without behavior change
2. build generated dashboard and compare behavior
3. split existing web_gateway_api handlers without route change
4. verify existing tests + dashboard
5. mới bắt đầu thêm Web Auth / Settings / MCP token UI
```

Không trộn một commit lớn gồm vừa move toàn bộ code vừa đổi behavior security nếu có thể tránh.

## 26.16 Test requirements cho modular build

Phải có ít nhất:

```text
build_webui.py succeeds with valid source
missing include -> build fails
missing JS module -> build fails
unresolved placeholder -> build fails
output contains devices view
output contains device detail view
output contains scanner view
output contains settings view
output contains required API/bootstrap script
```

Integration test phải xác nhận generated dashboard vẫn load với CSP hiện tại và không request các source partial path.

---

# 27. Settings layout chi tiết

Đề xuất responsive grid giữ style hiện tại.

Desktop:

```text
+---------------------------+ +---------------------------+
| General                   | | Web Access                |
| Language                  | | Auth / username/password  |
+---------------------------+ +---------------------------+

+---------------------------+ +---------------------------+
| MCP Access                | | Network Status            |
| Endpoint/token/guide      | | SSID/IP/MAC/RSSI          |
+---------------------------+ +---------------------------+

+---------------------------+ +---------------------------+
| System Information        | | Danger Zone               |
| FW/uptime/heap            | | Restart Gateway           |
+---------------------------+ +---------------------------+
```

Mobile:

```text
General
Web Access
MCP Access
Network Status
System Information
Danger Zone
```

Không cần nested Settings tabs ở v1.

---

# 28. HTTP route plan

Các route mới tối thiểu của Settings/Auth v1.1:

| Method | URI | Purpose |
|---|---|---|
| GET | `/login` | generated login page |
| POST | `/api/auth/login` | create Web session |
| POST | `/api/auth/logout` | destroy session |
| PUT | `/api/auth/config` | enable/disable/change username |
| PUT | `/api/auth/password` | change password |
| GET | `/api/settings` | Settings runtime state |
| POST | `/api/mcp/token` | generate/rotate MCP token |

Không có runtime route cho `i18n.js`, HTML partial hoặc source JS module. Các source này được build-time bundle theo §26.

Baseline khoảng:

```text
23 existing
+7 new
=30 handlers
```

Tăng/giữ target:

```c
#define WEB_GATEWAY_MAX_URI_HANDLERS 36
```

Budget mục tiêu:

```text
~30 used
~6 reserve
```

Bắt buộc đếm exact route count từ source code (không ước tính) trước khi update `WEB_GATEWAY_MAX_URI_HANDLERS`. Nếu baseline thực tế >23, cập nhật budget tương ứng. Không được tăng route chỉ vì frontend được tách source.

Mọi registrar phải propagate `esp_err_t`; không được bỏ qua lỗi `httpd_register_uri_handler()`.

---

# 29. Module registration plan

`web_modules.h` public registrar surface thêm:

```c
esp_err_t web_auth_api_register(httpd_handle_t server);
esp_err_t web_settings_api_register(httpd_handle_t server);
```

Các registrar domain-level cho device/command/capability/exposure có thể để internal header của `web_server`, tránh mở rộng public include không cần thiết.

Gateway registrar sequence đề xuất:

```text
web_assets_register_gateway
web_auth_api_register
web_settings_api_register
web_gateway_api_register
web_system_api_register_gateway
web_ble_api_register
MCP registration theo flow hiện tại
```

Trong đó `web_gateway_api_register()` chỉ aggregate các existing gateway-domain API sau khi tách file:

```text
web_device_api_register
web_command_api_register
web_capability_api_register
web_exposure_api_register
```

`web_auth_init()` phải hoàn tất trước khi server nhận request.

Provisioning server không register Web Auth APIs trong v1.

---

# 30. Provisioning mode

Settings v1 không thay đổi captive provisioning flow.

Trong provisioning mode:

```text
Web Auth ignored
login page not used
MCP token management unavailable
Dashboard APIs unavailable
```

Provisioning tiếp tục chỉ phục vụ setup Wi-Fi và các captive probe routes hiện tại.

Sau provisioning và gateway chuyển sang normal mode, Web Auth policy mới có hiệu lực.

Không dùng Web Auth password làm Wi-Fi provisioning password.

---

# 31. API error codes

New standardized codes:

```text
auth_required
auth_disabled
auth_invalid_credentials
auth_rate_limited
auth_current_password_required
auth_current_password_invalid
auth_credentials_not_configured
auth_username_invalid
auth_password_invalid
auth_storage_error
mcp_token_generation_failed
mcp_token_persist_failed
invalid_request
internal_error
```

HTTP mapping:

| Error | HTTP |
|---|---:|
| auth_required | 401 |
| auth_invalid_credentials | 401 |
| auth_current_password_invalid | 403 |
| auth_rate_limited | 429 |
| invalid request | 400 |
| state conflict | 409 |
| storage/internal | 500 |

Dùng `403` cho valid/open Web context nhưng current password sai trong sensitive configuration operation; login credential sai dùng `401`.

---

# 32. File changes

## 32.1 Tạo mới - Web Auth

```text
components/web_auth/CMakeLists.txt
components/web_auth/Kconfig.projbuild
components/web_auth/include/web_auth.h
components/web_auth/web_auth.c
components/web_auth/web_auth_store.c
components/web_auth/web_auth_password.c
components/web_auth/web_auth_session.c
components/web_auth/test/CMakeLists.txt
components/web_auth/test/test_web_auth.c
```

## 32.2 Tạo mới - Web Server API modules

```text
components/web_server/web_auth_api.c
components/web_server/web_auth_http.c
components/web_server/web_settings_api.c
components/web_server/web_device_api.c
components/web_server/web_command_api.c
components/web_server/web_capability_api.c
components/web_server/web_exposure_api.c
```

Sau refactor, `web_gateway_api.c` chỉ còn vai trò registrar/aggregator cho các gateway-domain API.

## 32.3 Tạo mới - modular Web UI source

```text
components/web_server/www_src/dashboard/shell.html
components/web_server/www_src/dashboard/views/devices.html
components/web_server/www_src/dashboard/views/device_detail.html
components/web_server/www_src/dashboard/views/scanner.html
components/web_server/www_src/dashboard/views/settings.html
components/web_server/www_src/dashboard/partials/sidebar.html
components/web_server/www_src/dashboard/partials/modals.html
components/web_server/www_src/dashboard/partials/restart_overlay.html
components/web_server/www_src/dashboard/js/core/state.js
components/web_server/www_src/dashboard/js/core/api.js
components/web_server/www_src/dashboard/js/core/nav.js
components/web_server/www_src/dashboard/js/core/ui.js
components/web_server/www_src/dashboard/js/features/devices.js
components/web_server/www_src/dashboard/js/features/scanner.js
components/web_server/www_src/dashboard/js/features/device_detail.js
components/web_server/www_src/dashboard/js/features/mcp_exposure.js
components/web_server/www_src/dashboard/js/features/settings.js
components/web_server/www_src/login/shell.html
components/web_server/www_src/login/login.js
components/web_server/www_src/shared/common.js
components/web_server/www_src/shared/i18n.js
components/web_server/tools/build_webui.py
```

Exact split có thể thay đổi nhẹ theo boundaries thật của JavaScript, nhưng không quay lại monolithic `dashboard.html`.

## 32.4 Sửa

```text
components/web_server/CMakeLists.txt
components/web_server/Kconfig.projbuild
components/web_server/tailwind.config.js
components/web_server/web_server.c
components/web_server/web_modules.h
components/web_server/web_assets.c
components/web_server/web_gateway_api.c
components/web_server/test/CMakeLists.txt

components/mcp_endpoint/mcp_auth.c
components/mcp_endpoint/Kconfig.projbuild
components/mcp_endpoint/CMakeLists.txt
components/mcp_endpoint/include/*   // public MCP auth management API
components/mcp_endpoint/test/*

test/CMakeLists.txt
```

## 32.5 Xóa sau migration

```text
components/web_server/web_admin_auth.c
components/web_server/web_admin_auth.h
components/web_server/www/dashboard.html
```

Xóa `CONFIG_WEB_ADMIN_AUTH_TOKEN` khỏi `components/web_server/Kconfig.projbuild`.

`www/dashboard.html` chỉ được xóa sau khi generated dashboard đã build và serve ổn định. Không thay bằng một generated file dài committed vào source tree.

Không xóa `web_admin_auth` trước khi exposure API đã chuyển sang Web Auth.

---

# 33. CMake changes

`components/web_server/CMakeLists.txt` cần:

- thêm source API modules mới;
- thêm `web_auth` dependency;
- thêm custom command build `dashboard.html` + `login.html` từ `www_src/`;
- khai báo mọi `www_src/**/*.html` và `www_src/**/*.js` là dependency của generated assets;
- embed generated dashboard/login từ `${CMAKE_CURRENT_BINARY_DIR}`;
- giữ CSS/icon/font assets hiện có;
- bỏ `web_admin_auth.c` sau migration;
- không embed riêng từng JS source module.

Pseudo target:

```cmake
file(GLOB_RECURSE WEBUI_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/www_src/*.html"
    "${CMAKE_CURRENT_SOURCE_DIR}/www_src/*.js"
)

set(webui_builder "${CMAKE_CURRENT_SOURCE_DIR}/tools/build_webui.py")
set(dashboard_generated "${CMAKE_CURRENT_BINARY_DIR}/dashboard.html")
set(login_generated "${CMAKE_CURRENT_BINARY_DIR}/login.html")

add_custom_command(
    OUTPUT "${dashboard_generated}" "${login_generated}"
    COMMAND "${python}" "${webui_builder}"
            --source "${CMAKE_CURRENT_SOURCE_DIR}/www_src"
            --dashboard-out "${dashboard_generated}"
            --login-out "${login_generated}"
    DEPENDS ${WEBUI_SOURCES} "${webui_builder}"
    VERBATIM
)
```

`EMBED_FILES` dùng generated outputs thay vì source monolith.

Lưu ý: Nếu ESP-IDF `EMBED_FILES` không accept paths từ `${CMAKE_CURRENT_BINARY_DIR}`, fallback sang copy generated files vào `www/` directory trước khi embed. Verify build pipeline trong Phase 0.

`components/web_auth/CMakeLists.txt` dependencies tối thiểu dự kiến:

```text
nvs_flash
mbedtls
esp_timer
esp_system
```

và FreeRTOS nếu session state cần mutex.

`tailwind.config.js` phải scan:

```text
www_src/**/*.html
www_src/**/*.js
```

Root `test/CMakeLists.txt` thêm:

```text
web_auth
```

vào `TEST_COMPONENTS`.

Build phải fail nếu generated dashboard/login không được tạo; không fallback âm thầm sang monolithic stale asset.

---

# 34. Concurrency / locking

Web server có thể xử lý nhiều request theo task/server context; auth/session state không được giả định luôn single-call sequence.

`web_auth_session` phải bảo vệ session table bằng mutex hoặc critical section phù hợp.

Không giữ mutex trong lúc chạy PBKDF2 lâu.

Flow login:

```text
read credential snapshot
    -> release store lock
    -> PBKDF2 verify
    -> session lock
    -> allocate session
```

Password change:

```text
verify current password
    -> derive new hash outside session lock
    -> persist credential
    -> invalidate sessions
```

NVS write được serialize trong auth store service.

---

# 35. NVS failure semantics

## 35.1 Enable auth

Nếu credential persist fail:

```text
auth remains OFF
```

## 35.2 Disable auth

Chỉ report success khi `enabled=0` đã commit thành công.

Nếu commit fail:

```text
auth remains logically ON
current sessions remain valid
```

## 35.3 Change password

Nếu new hash persist fail:

```text
old password remains valid
sessions are NOT invalidated
```

Chỉ invalidate session sau commit thành công.

## 35.4 MCP rotate

Nếu new token hash persist fail:

```text
old MCP token remains valid
new plaintext token must NOT be returned as success
```

Rotation phải có fail-closed atomic semantics từ góc nhìn client.

---

# 36. MCP token verification refactor

Current auth path tải plaintext token rồi constant-time compare string.

Target runtime-hash path:

```text
Authorization Bearer token
       |
       v
SHA-256(presented token)
       |
       v
constant-time compare 32-byte digest
       |
       +-- match -> authorized
       +-- mismatch -> 401
```

Kconfig fallback có thể tiếp tục string compare vì token compile-time vẫn plaintext trong firmware image.

Khi runtime hashed token tồn tại, nó có precedence và Kconfig token không còn authorize request.

Không allocate/copy runtime plaintext token từ NVS vì plaintext không còn được lưu.

---

# 37. Security caveat - HTTP

Cả Web password và MCP Bearer token hiện truyền trên plaintext HTTP.

Vì vậy Settings phải hiển thị cảnh báo ngắn:

```text
This gateway currently uses HTTP.
Use it only on a trusted local network.
```

Web Auth v1 giúp chặn người không có credential gọi dashboard/API, nhưng không chống được attacker có khả năng sniff/replay traffic trên cùng LAN.

Không mô tả Web Auth là "secure remote access".

---

# 38. Test plan - Web UI modular build

Các test/build checks bắt buộc:

| Case | Expected |
|---|---|
| valid source tree | generated dashboard/login thành công |
| missing HTML include | build fail |
| unresolved include marker | build fail |
| missing JS manifest entry target | build fail |
| edit one view | generated dashboard rebuild |
| edit one JS feature | generated dashboard rebuild |
| Tailwind scan source modules | class trong partial/JS không bị purge |
| dashboard runtime | không request `/views/*`, `/js/*`, `/i18n.js` |
| route count | không tăng theo số frontend source modules |
| CSP | generated dashboard load đúng với policy hiện tại |

Refactor phase phải có smoke comparison với behavior trước refactor:

```text
Devices
Scanner
Device Detail
MCP exposure
Settings hiện có
Restart
```

đều hoạt động trước khi thêm behavior Settings mới.

---

# 39. Test plan - web_auth component

## 39.1 Credential tests

- default state auth OFF;
- no credentials after clean NVS;
- first enable persists username/hash/salt;
- plaintext password không tồn tại trong NVS;
- correct password verifies;
- wrong password fails;
- invalid username rejected;
- short password rejected;
- oversized password rejected;
- disable keeps credentials;
- re-enable requires correct current password;
- username change requires current password;
- password change invalidates old password;
- NVS write failure không chuyển state sai.

## 39.2 Session tests

- successful login creates session;
- invalid login does not create session;
- session token uniqueness;
- session validation succeeds;
- logout invalidates session;
- idle timeout invalidates session;
- absolute timeout invalidates session;
- password change invalidates sessions;
- username change invalidates sessions;
- auth disable invalidates sessions;
- max session capacity behavior deterministic;
- reboot/init resets RAM session state.

## 39.3 Rate limit tests

- first allowed failures accepted;
- threshold triggers rate limit;
- successful auth behavior after window reset;
- no password/token in logs/test diagnostics.

---

# 40. Test plan - Web HTTP APIs

## 40.1 Auth OFF

Verify:

```text
GET /              -> dashboard
GET /api/settings  -> 200
GET /api/devices   -> normal behavior
PUT exposure       -> allowed without session
```

## 40.2 Auth ON, no session

Verify:

```text
GET /              -> redirect /login
GET /api/settings  -> 401
GET /api/devices   -> 401
PUT exposure       -> 401
POST MCP token     -> 401
```

## 40.3 Auth ON, valid session

Verify protected APIs operate normally.

## 40.4 Login

- valid credential -> cookie;
- invalid credential -> 401;
- malformed JSON -> 400;
- oversized body -> current body-limit policy;
- rate limit -> 429;
- response `Cache-Control: no-store`.

## 40.5 Logout

- valid session invalidated;
- cookie expired;
- next protected request returns 401.

---

# 41. Test plan - MCP token

- status with no runtime/Kconfig token;
- legacy plaintext NVS token migration;
- migration write failure keeps old token usable;
- generate token produces valid non-empty unique token;
- plaintext token returned once only;
- GET settings never returns full token;
- token hash stored in NVS;
- old token works before rotation;
- rotation succeeds;
- old token rejected after rotation;
- new token accepted;
- rotation persist failure leaves old token valid;
- Kconfig fallback works when runtime token absent;
- runtime token overrides Kconfig token;
- authorization comparison constant-time at digest layer;
- no token value appears in logs.

---

# 42. Test plan - i18n

- default `auto`;
- `vi-VN` selects Vietnamese;
- unsupported browser language falls back English;
- selecting `vi` persists in localStorage;
- selecting `en` persists;
- refresh keeps chosen language;
- device-generated labels are not translated by key lookup unless explicitly mapped;
- API error code maps to localized message;
- missing translation key falls back safely to English/key.

---

# 43. Integration test matrix

| Scenario | Expected |
|---|---|
| Fresh firmware | Web Auth OFF; dashboard opens |
| Enable Web Auth | credentials persist; login required |
| Reboot with auth ON | login required again; credential still valid |
| Disable auth | dashboard opens without login |
| Reboot with auth OFF | remains open |
| Re-enable auth | existing password required |
| Change password | all sessions invalidated |
| MCP no token | current dev/unprotected behavior + UI warning |
| Generate MCP token | `/mcp` requires new token |
| Rotate MCP token | old MCP clients fail immediately |
| Web Auth OFF + MCP token ON | dashboard open; `/mcp` protected |
| Web Auth ON + MCP token ON | dashboard and MCP independently protected |
| Exposure edit with Web Auth ON | requires browser session |
| Exposure edit with Web Auth OFF | allowed without login |
| Device Detail MCP tool selection | unchanged behavior |
| Language change | UI updates without reboot |
| Provisioning mode | unchanged captive portal flow |

---

# 44. Implementation phases

## Phase 0 - Web UI / Web Server modularization

1. create `www_src/` source tree;
2. split existing `dashboard.html` markup theo view/partial;
3. split existing JavaScript theo core/feature;
4. implement `build_webui.py`;
5. update CMake generated asset flow;
6. update Tailwind source scan;
7. split `web_gateway_api.c` thành domain API modules;
8. giữ nguyên route contract và behavior;
9. chạy smoke/regression tests trước khi thêm Settings mới.

Exit criteria:

```text
existing dashboard behavior unchanged
AND generated asset builds deterministically
AND no runtime partial/module routes were added
AND web_gateway_api.c is registrar/aggregator, not handler monolith
```

## Phase 1 - web_auth core

1. create `web_auth` component;
2. implement NVS schema/version;
3. implement password hash/verify;
4. implement session table;
5. implement login limiter;
6. add unit tests.

Exit criteria:

```text
web_auth component test passes independently
```

## Phase 2 - Web Auth HTTP integration

1. add `/login` asset;
2. add login/logout/config/password APIs;
3. add request auth helper;
4. protect dashboard root;
5. protect normal Web APIs when auth ON;
6. add global 401 handling frontend.

Exit criteria:

```text
open mode and protected mode both work
```

## Phase 3 - remove MCP Admin Token

1. convert exposure GET/PUT to Web Auth middleware;
2. remove Authorization injection from dashboard API wrapper;
3. remove `mcp_admin_token` localStorage;
4. remove old Settings card;
5. delete `web_admin_auth` after no references remain;
6. remove `CONFIG_WEB_ADMIN_AUTH_TOKEN`.

Exit criteria:

```text
Device Detail MCP exposure works using Web session/open-mode policy
```

## Phase 4 - MCP token management

1. refactor MCP token storage to hashed runtime token;
2. implement legacy migration;
3. implement MCP auth status API;
4. implement token rotate/generate service;
5. expose `/api/mcp/token`;
6. add MCP Access card;
7. add one-time token modal;
8. add MCP connection guide.

Exit criteria:

```text
MCP token can be created and rotated from Settings
without ever being persisted in browser storage
```

## Phase 5 - i18n

1. create/update `www_src/shared/i18n.js` dictionary/helper;
2. add language selector;
3. migrate navigation/settings strings first;
4. migrate Devices/Scanner/Device Detail strings;
5. migrate toast/error messages;
6. persist language to localStorage.

Exit criteria:

```text
entire dashboard usable in English and Vietnamese
```

## Phase 6 - route/security/regression

1. recount URI handlers;
2. increase handler budget;
3. run web/component/integration tests;
4. verify provisioning unaffected;
5. verify MCP tool exposure unaffected;
6. verify no old admin token references;
7. verify no secret logs.

---

# 45. Recommended implementation order at file level

```text
0.  components/web_server/www_src/*                 // extract current UI
1.  components/web_server/tools/build_webui.py
2.  components/web_server/tailwind.config.js
3.  components/web_server/CMakeLists.txt             // generated asset pipeline
4.  split web_device_api.c / web_command_api.c
5.  split web_capability_api.c / web_exposure_api.c
6.  reduce web_gateway_api.c to registrar/aggregator
7.  verify existing dashboard behavior
8.  components/web_auth/*
9.  test/CMakeLists.txt
10. components/web_server/web_auth_http.c
11. components/web_server/web_auth_api.c
12. components/web_server/www_src/login/*
13. components/web_server/web_assets.c
14. components/web_server/web_server.c
15. components/web_server/web_modules.h
16. protect existing Web APIs
17. convert exposure auth to Web Auth
18. remove web_admin_auth
19. refactor components/mcp_endpoint/mcp_auth.c
20. add MCP auth management public API
21. add web_settings_api.c
22. update Settings view + settings.js
23. update shared i18n
24. add/update tests
25. recount routes and verify startup
26. remove legacy source `www/dashboard.html`
```

Không thực hiện bước 8+ nếu Phase 0 chưa đạt behavior parity cơ bản.

---

# 46. Review checklist trước merge

## Web UI / Web Server modularization

- [ ] `www_src/` là source-of-truth;
- [ ] generated dashboard/login nằm trong build directory;
- [ ] không chỉnh tay generated asset;
- [ ] không có runtime HTML partial routes;
- [ ] không có runtime route cho từng JS source module;
- [ ] JS bundle order deterministic;
- [ ] Tailwind scan cả `www_src/**/*.html` và `www_src/**/*.js`;
- [ ] CMake rebuild asset khi partial/JS thay đổi;
- [ ] `web_gateway_api.c` không còn handler monolith;
- [ ] device/command/capability/exposure handlers đã tách ownership;
- [ ] route count không tăng theo số frontend module;
- [ ] old dashboard behavior pass trước khi merge security behavior.

## Architecture

- [ ] không có MCP tab mới;
- [ ] Device Detail vẫn sở hữu MCP command exposure UI;
- [ ] Web Auth và MCP Auth độc lập;
- [ ] `web_server` không đọc MCP secret NVS trực tiếp;
- [ ] old `web_admin_auth` đã được remove sau migration.

## Web Auth

- [ ] default OFF;
- [ ] auth OFF cho phép toàn dashboard/API như yêu cầu;
- [ ] auth ON bắt login;
- [ ] password không lưu plaintext;
- [ ] session RAM-only;
- [ ] session cookie HttpOnly + SameSite=Strict;
- [ ] change password invalidates session;
- [ ] disable auth yêu cầu current password;
- [ ] rate limiter hoạt động.

## MCP

- [ ] token được generate bằng CSPRNG;
- [ ] runtime token chỉ lưu hash;
- [ ] plaintext chỉ trả một lần;
- [ ] rotate invalidates old token;
- [ ] `GET /api/settings` không leak token;
- [ ] legacy `mcp/token` migration an toàn;
- [ ] Kconfig fallback behavior rõ ràng.

## Frontend

- [ ] không lưu secret trong localStorage;
- [ ] language là local preference;
- [ ] i18n được bundle build-time, không cần `/i18n.js`;
- [ ] English + Vietnamese hoạt động;
- [ ] API 401 redirect login khi cần;
- [ ] MCP Access chỉ có endpoint/token/help;
- [ ] tool enable/disable không xuất hiện trong Settings.

## Runtime

- [ ] URI handler count dưới configured max;
- [ ] mọi route registration error được propagate;
- [ ] no reboot required cho language;
- [ ] provisioning unchanged;
- [ ] BLE/device flow unchanged;
- [ ] MCP endpoint vẫn hoạt động sau token refactor.

---

# 47. Definition of Done

Settings v1 được coi là hoàn tất khi tất cả điều kiện sau đạt:

1. Sidebar vẫn chỉ có `My Devices`, `Add Device`, `Gateway Settings`.
2. Không có MCP tab riêng.
3. Device Detail tiếp tục quản lý MCP tool exposure từng command.
4. Settings cho chọn `Auto / English / Tiếng Việt` và persist theo browser.
5. Web UI có toggle `Require login`.
6. Khi toggle OFF, dashboard và Web APIs hoạt động không username/password/session.
7. Khi toggle ON, dashboard yêu cầu login.
8. Gateway hỗ trợ một username administrator và password hashed trong NVS.
9. User đổi username/password được từ Settings.
10. Password change invalidates existing sessions.
11. Browser session chỉ tồn tại RAM gateway + HttpOnly cookie.
12. `MCP Admin Token` cũ bị loại bỏ hoàn toàn.
13. MCP exposure API dùng Web Auth policy.
14. Settings hiển thị MCP endpoint và trạng thái token.
15. User generate/rotate MCP token từ Settings.
16. Plaintext MCP token chỉ hiển thị ngay sau generate/rotate.
17. Runtime MCP token không lưu plaintext trong NVS.
18. `/mcp` dùng MCP bearer token độc lập với Web Auth.
19. Route budget đủ và tất cả handlers đăng ký thành công.
20. Unit/integration tests trong tài liệu pass.
21. Không làm regress provisioning, device management, BLE command hoặc dynamic MCP tools.
22. `dashboard.html` monolith cũ không còn là source-of-truth; frontend được quản lý dưới `www_src/`.
23. Build-time assembly tạo dashboard/login deterministically mà không tạo runtime route cho từng partial/JS module.
24. `web_gateway_api.c` được giảm thành registrar/aggregator và các API domain chính có file ownership riêng.
25. Tailwind/CMake dependency theo dõi đúng source module; chỉnh một partial/JS file làm rebuild asset tương ứng.

---

# 48. Kiến trúc target cuối cùng

```text
Browser
   |
   +-- GET /
   |      |
   |      +-- web_auth OFF -----------------> Dashboard
   |      |
   |      +-- web_auth ON
   |              |
   |              +-- valid session --------> Dashboard
   |              |
   |              +-- no session -----------> /login
   |
   +-- Web API
          |
          v
   web_auth_require_request()
          |
          +-- OFF -> allow
          |
          +-- ON -> validate GWSESSION
                    |
                    v
              Web services
                    |
       +------------+-------------+
       |                          |
 Device management         MCP exposure admin
                                  |
                                  v
                         mcp_tool_exposure


AI Agent / MCP Client
        |
        | Authorization: Bearer <MCP_TOKEN>
        v
      /mcp
        |
        v
   mcp_auth_gate()
        |
        v
 MCP tools / command system
```

Security boundary cuối cùng:

```text
Web credential
    username/password
           |
           v
    browser session
           |
           v
     Dashboard APIs

MCP credential
      bearer token
           |
           v
        /mcp
```

Đây là boundary cần giữ cho các phase Dashboard tiếp theo.
