# Đánh giá flow và hiệu năng Web Authentication

**Dự án:** ESP32 BLE Gateway  
**Phạm vi:** `components/web_auth`, lớp tích hợp trong `components/web_server` và Web Dashboard  
**Ngày đánh giá:** 2026-08-29  
**Trạng thái:** Đã triển khai các sửa đổi correctness, hot path, route policy, Settings API, MCP token và gzip; còn benchmark/Unity runtime trên phần cứng

---

## 1. Mục tiêu

Tài liệu này mô tả:

- flow Web Authentication hiện tại;
- nguyên nhân khiến Web Dashboard chậm hơn sau khi tích hợp auth;
- các lỗi correctness có thể làm flow login không hoạt động;
- ảnh hưởng của từng vấn đề khi auth OFF và auth ON;
- phương hướng xử lý theo mức độ ưu tiên;
- tiêu chí nghiệm thu và kế hoạch kiểm thử trên ESP32-S3 thật.

Phân tích dựa trên source code và web asset đang được build tại thời điểm đánh giá. Các con số thời gian thực thi trên thiết bị cần được xác nhận bằng benchmark trên ESP32-S3.

---

## 2. Kết luận tổng quan

Độ trễ ban đầu không đến từ một nguyên nhân duy nhất. Có ba nhóm chi phí cộng dồn:

1. **Đăng nhập chậm:** PBKDF2 chạy đồng bộ trong HTTP handler; implementation cũ tạo/hủy PSA key ở từng iteration.
2. **Request sau đăng nhập chậm:** implementation cũ đọc lại toàn bộ cấu hình auth từ NVS cho mỗi request cần session.
3. **Dashboard baseline khởi tạo nhiều việc hơn:** UI cũ gọi tuần tự `/api/status`, `/api/settings` và `/api/mcp-token`.

Ngoài hiệu năng, flow cũ có hai lỗi correctness mức P0:

- session creator sinh token 32 ký tự nhưng validator chỉ chấp nhận 43 ký tự;
- HMAC khai báo key cố định 256 bit trong khi password có độ dài 8–64 byte, có thể làm PSA từ chối password không đúng 32 byte.

Hai lỗi P0 này đã được sửa. Session hiện dùng token base64url canonical 43 ký tự; password derivation dùng PSA PBKDF2-HMAC-SHA256 với password dài biến đổi. Build firmware và test app đều thành công. Thời gian PBKDF2 thực tế vẫn cần đo trên ESP32-S3 trước khi kết luận đạt target 300 ms hoặc cần worker async.

### 2.1 Quyết định đơn giản hóa Settings API

Worktree sau đánh giá áp dụng một read API duy nhất:

```http
GET /api/settings
```

Response được chia thành bốn nhóm ngắn gọn:

```json
{
  "success": true,
  "system": {
    "firmware": "1.0.0",
    "idf": "v5.4.4",
    "uptime_ms": 123456,
    "free_heap": 180000
  },
  "network": {
    "connected": true,
    "state": "connected",
    "ssid": "example",
    "ip": "192.168.1.100",
    "mac": "AA:BB:CC:DD:EE:FF",
    "rssi": -55
  },
  "auth": {
    "enabled": true,
    "configured": true,
    "username": "admin"
  },
  "mcp": {
    "configured": true,
    "preview": "...a1b2"
  }
}
```

Full MCP token không xuất hiện trong Settings response. Nó chỉ được trả một lần sau thao tác generate.

Token generate mới dùng format:

```text
mcp_<32 ký tự hex>
```

Tổng chiều dài là 36 ký tự, chứa 128 bit random entropy. Format này thay cho token alphanumeric 64 ký tự trước đây.

---

## 3. Thành phần liên quan

| Thành phần | Trách nhiệm |
|---|---|
| `components/web_auth/web_auth.c` | Auth state, login, enable/disable và điều phối session |
| `components/web_auth/web_auth_store.c` | Persist credential và trạng thái auth trong NVS |
| `components/web_auth/web_auth_password.c` | Hash/verify password bằng PBKDF2-HMAC-SHA256 |
| `components/web_auth/web_auth_session.c` | Sinh, hash, lưu và validate session token trong RAM |
| `components/web_server/web_auth_http.c` | Đọc cookie `GWSESSION` và bảo vệ HTTP request |
| `components/web_server/web_auth_api.c` | Login, logout và thay đổi cấu hình auth |
| `components/web_server/web_assets.c` | Bảo vệ `GET /`, redirect tới `/login`, phục vụ asset |
| `components/web_server/web_settings_api.c` | Gộp trạng thái system, network, auth và MCP token cho trang Settings |
| `components/web_server/www_src/` | Login UI và Dashboard UI |

---

## 4. Flow hiện tại

### 4.1 Khởi động gateway

Ở gateway mode:

```text
web_server_start()
    -> web_auth_init()
        -> web_auth_session_init()
        -> auth_state_load()
            -> web_auth_store_load()
                -> mở namespace NVS "web_auth"
                -> đọc và validate auth record
        -> đăng ký HTTP routes
```

Cache hiện chỉ chứa:

- `enabled`;
- `credentials_configured`;
- `username`.

Credential hash và salt không cần cache cho session validation. Chúng chỉ cần khi login hoặc thực hiện thao tác yêu cầu current password.

### 4.2 Mở dashboard khi auth OFF

```text
GET /
    -> web_auth_require_request()
        -> web_auth_get_status() từ RAM cache
        -> auth OFF, cho phép request
    -> gửi dashboard HTML

DOMContentLoaded
    -> chạy song song devices.load() và settings.load()
        -> GET /api/devices
        -> GET /api/settings
            -> trạng thái system/network/auth/MCP trong cùng response
```

Khi auth OFF, cache mới giúp tránh đọc NVS cho từng auth check. Toàn bộ dữ liệu Settings được trả trong một HTTP response.

### 4.3 Mở dashboard khi auth ON nhưng chưa có session

```text
GET /
    -> auth ON
    -> không có GWSESSION
    -> 303 See Other, Location: /login

GET /login
    -> gửi login HTML
```

Đây là flow hợp lệ, nhưng thêm ít nhất một HTTP round-trip so với khi auth OFF.

### 4.4 Đăng nhập

```text
POST /api/auth/login
    -> parse username/password
    -> đọc credential từ NVS
    -> PBKDF2 verify password, chạy đồng bộ
    -> sinh session token
    -> lưu hash của token trong RAM
    -> Set-Cookie: GWSESSION=...
    -> UI chuyển tới /
```

PBKDF2 hiện chạy trên HTTP server task. Trong thời gian verify, server không thể xử lý nhanh các request khác như CSS, HTML hoặc REST API.

Implementation derive hiện dùng PSA PBKDF2 primitive, không còn import/destroy HMAC key ở từng iteration. Việc chuyển sang worker async chỉ cần thực hiện nếu benchmark phần cứng vẫn vượt giới hạn UX.

### 4.5 Validate session sau login

```text
GET /
    -> web_auth_require_request()
        -> đọc auth status từ RAM cache
        -> cấp phát buffer và parse Cookie header
        -> web_auth_validate_session()
            -> web_auth_session_validate()
                -> kiểm tra độ dài/alphabet canonical
                -> SHA-256
                -> tìm token hash trong session table RAM
```

Hot path này không còn đọc NVS. Credential chỉ được load khi login hoặc thay đổi cấu hình nhạy cảm.

---

## 5. Các vấn đề được phát hiện

### 5.1 P0 — Session token tạo 32 ký tự nhưng validator yêu cầu 43 — Đã sửa

Session dùng 32 byte random. Base64url không padding của 32 byte phải có 43 ký tự.

Creator cũ có vòng lặp bị giới hạn bởi:

```c
i < SESSION_TOKEN_LEN
```

với `SESSION_TOKEN_LEN = 32`, nên chỉ ghi 32 ký tự vào cookie.

Validator lại có điều kiện:

```c
if (token_len != 43) return WEB_AUTH_INVALID_CREDENTIALS;
```

#### Ảnh hưởng

- backend có thể trả login success và cấp cookie;
- cookie vừa cấp luôn bị validator từ chối;
- browser chuyển tới `/`, sau đó bị redirect lại `/login`;
- người dùng thấy login chậm, reload hoặc không vào được dashboard.

#### Phương hướng giải quyết

- dùng base64url encoder đã được kiểm thử thay vì tự xử lý bit nếu ESP-IDF/mbedTLS đã có primitive phù hợp;
- nếu giữ implementation riêng, vòng lặp output phải sinh đúng 43 ký tự và không đọc quá 32 byte input;
- creator và validator phải dùng cùng một canonical encoding;
- thêm test round-trip: create token → validate token → success;
- thêm test reject token sai độ dài, sai alphabet và token đã logout.

#### Tiêu chí nghiệm thu

- token trả cho browser dài đúng 43 ký tự;
- token vừa tạo validate thành công;
- thay đổi một ký tự bất kỳ làm validation thất bại;
- reboot hoặc logout làm session cũ thất bại.

#### Kết quả triển khai

- Sinh 32 byte random rồi encode base64url không padding bằng primitive mbedTLS, kết quả đúng 43 ký tự.
- Creator và validator dùng cùng canonical representation; hash SHA-256 được tính trên token canonical.
- Validator từ chối sai độ dài và ký tự ngoài alphabet.
- Đã thêm Unity test round-trip, token bị sửa, alphabet sai, logout và hai session độc lập.

---

### 5.2 P0 — PBKDF2 triển khai không tối ưu và có lỗi key size — Đã sửa implementation

PBKDF2 mặc định dùng 60.000 iterations. Implementation cũ tự viết vòng lặp PBKDF2 và ở mỗi lần HMAC đều thực hiện:

```text
psa_import_key()
psa_mac_sign_setup()
psa_mac_update()
psa_mac_sign_finish()
psa_destroy_key()
```

Như vậy một lần login có thể import và destroy PSA key khoảng 60.000 lần.

Ngoài ra code cũ đặt:

```c
psa_set_key_bits(&attrs, 256);
```

nhưng truyền password có độ dài biến đổi làm key material. Với PSA implementation hiện tại, declared key size không khớp actual key size có thể trả `PSA_ERROR_INVALID_ARGUMENT`. Password chỉ tình cờ khớp khi dài đúng 32 byte.

#### Ảnh hưởng hiệu năng

- thời gian login có thể vượt xa target 300 ms;
- HTTP server task bị giữ trong suốt quá trình derive key;
- request của client khác cũng bị xếp hàng;
- watchdog/timeout có thể trở thành rủi ro nếu iteration tăng hoặc CPU đang bận BLE/Wi-Fi.

#### Phương hướng giải quyết

1. Dùng primitive PBKDF2-HMAC-SHA256 do mbedTLS cung cấp, không tự triển khai vòng lặp HMAC.
2. Không hardcode HMAC key thành 256 bit nếu key là password có độ dài biến đổi.
3. Giữ iterations đã persist trong NVS để credential cũ vẫn verify được sau khi đổi Kconfig.
4. Benchmark trên ESP32-S3 ở 240 MHz với ít nhất các mức 10k, 30k và 60k.
5. Chọn iteration lớn nhất vẫn đạt mục tiêu UX và không làm HTTP server mất phản hồi quá lâu.
6. Nếu primitive tối ưu vẫn vượt target, chuyển password verification sang worker và hoàn tất HTTP request theo async handler pattern.

Không nên chỉ giảm iterations trước khi sửa implementation, vì phần overhead import/destroy key hiện là vấn đề kiến trúc.

#### Tiêu chí nghiệm thu

- password dài từ 8 đến 64 byte đều hash/verify đúng;
- hash tạo ra khớp vector PBKDF2-HMAC-SHA256 chuẩn;
- password sai bị từ chối;
- verify password trên ESP32-S3 đạt mục tiêu không quá 300 ms, hoặc có target mới được ghi rõ bằng số liệu đo;
- trong lúc login, các endpoint nhẹ vẫn đáp ứng trong giới hạn đã định nếu dùng worker async.

#### Kết quả triển khai

- Đã thay vòng lặp HMAC tự viết bằng `PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_256)`.
- Không còn khai báo password thành key cố định 256 bit.
- Vẫn dùng iteration persist trong NVS và kiểm tra range 10.000–200.000.
- Đã thêm vector chuẩn 10.000 iterations và test password dài 8/32/64 byte.
- Firmware/test app đã compile; target ≤ 300 ms và quyết định async còn chờ benchmark trên thiết bị thật.

---

### 5.3 P1 — Đọc NVS trong mọi session validation — Đã sửa

`web_auth_validate_session()` cũ tiếp tục gọi `web_auth_store_load()` dù middleware đã biết trạng thái auth.

Một lần `web_auth_store_load()` trong implementation cũ đọc:

1. `ver`;
2. `enabled`;
3. `username`;
4. `pwd_salt`;
5. `pwd_hash`;
6. `pwd_iter`.

Đây là dữ liệu credential, trong khi session validation thực tế chỉ cần:

- biết auth đang bật;
- hash token;
- so sánh với session table trong RAM;
- kiểm tra idle và absolute timeout.

#### Chi phí trong lần tải dashboard

Sau khi sửa lỗi token, một lần tải dashboard auth ON dự kiến đi qua middleware ít nhất ở:

```text
GET /
GET /api/devices
GET /api/settings
```

Tương đương tối thiểu:

```text
3 lần web_auth_store_load()
x 6 blob/lần
= 18 lần nvs_get_blob()
```

Aggregate Settings API vẫn đọc namespace NVS MCP một lần để lấy trạng thái token, nhưng không tạo thêm HTTP round-trip.

Trước thay đổi cache hiện tại, `web_auth_get_status()` cũng đọc NVS nên một request có cookie có thể đọc auth record hai lần.

#### Phương hướng giải quyết

- tạo một runtime auth state trong RAM, được load một lần khi `web_auth_init()`;
- middleware quyết định auth OFF/ON từ runtime state;
- khi auth ON và có cookie, gọi trực tiếp session validator trong RAM;
- chỉ đọc credential NVS khi login hoặc thao tác cần current password;
- cập nhật runtime state ngay sau khi NVS commit thành công;
- bảo vệ state bằng mutex hoặc critical section phù hợp nếu có nhiều task truy cập;
- không coi lỗi đọc NVS là auth OFF một cách im lặng trong các đường nhạy cảm.

Flow mục tiêu:

```text
request
    -> auth state RAM
    -> cookie parse
    -> SHA-256 token
    -> session table RAM
    -> handler
```

Không có NVS I/O trên request hot path.

#### Tiêu chí nghiệm thu

- request có session hợp lệ không gọi `nvs_open()` hoặc `nvs_get_*()`;
- enable/disable/change password cập nhật NVS và runtime state nhất quán;
- NVS commit thất bại không làm runtime state lệch với persistent state;
- sau reboot, runtime state được khôi phục chính xác.

#### Kết quả triển khai

- Runtime auth state được load một lần ở init và bảo vệ bằng mutex.
- Middleware và session validation chỉ dùng RAM; không gọi NVS trên protected-request hot path.
- Login/config mutation vẫn load credential khi cần; RAM chỉ được cập nhật sau khi ghi NVS thành công.
- NVS record được kiểm tra đủ field, terminator, schema và iteration range; lỗi đọc/corrupt fail closed.

---

### 5.4 P1 — Dashboard tạo request waterfall không cần thiết — Đã sửa

Baseline trước khi đơn giản hóa chạy:

```javascript
await Promise.all([devices.load(), settings.load()]);
```

Trong `settings.load()` cũ:

```text
await GET /api/status
await GET /api/settings
start GET /api/mcp-token
```

Hai request cuối được gọi trên mọi lần mở dashboard, dù tab active là Devices hoặc Scanner.

#### Ảnh hưởng

- thêm round-trip khi dashboard khởi tạo;
- `/api/settings` nằm trên critical path của `settings.load()`;
- thêm hai lần auth middleware khi auth ON;
- thêm NVS read ở MCP-token API;
- cạnh tranh với `/api/devices` trên HTTP server task.

#### Phương hướng giải quyết

- dùng một `GET /api/settings` trả `system`, `network`, `auth` và `mcp`;
- bỏ read route `GET /api/mcp-token`; giữ các mutation route generate/update/delete;
- sau generate/revoke, cập nhật browser state trực tiếp thay vì GET lại token status;
- full token chỉ trả một lần sau generate; Settings response chỉ trả preview;
- có thể lazy-load `/api/settings` theo tab ở phase sau nếu sidebar không cần preload system/network state.

#### Tiêu chí nghiệm thu

- mỗi lần load Settings chỉ có một `GET /api/settings`;
- không còn `GET /api/status` rồi `GET /api/settings` theo waterfall;
- không còn `GET /api/mcp-token`;
- generate/revoke không phát sinh GET refresh;
- response không chứa full MCP token.

---

### 5.5 P2 — Dashboard HTML lớn và chưa gzip — Đã sửa

Artifact build tại thời điểm đánh giá:

| Phiên bản | Kích thước raw | Kích thước gzip mức 9 |
|---|---:|---:|
| `HEAD` trước nhóm thay đổi đang đánh giá | 90.466 byte | 17.961 byte |
| Dashboard artifact hiện tại | 98.610 byte | 18.962 byte |
| Login artifact hiện tại | 7.172 byte | 2.456 byte |

Dashboard gzip giảm khoảng 80,8% và nằm dưới target 25 KiB.

Dashboard, login và provisioning setup đều được gzip deterministic tại build time và embed dưới dạng `.gz`.

Dashboard còn dùng `Cache-Control: no-cache`. Server chưa cung cấp validator rõ ràng như ETag, nên browser có thể phải nhận lại toàn bộ response khi revalidate.

#### Phương hướng giải quyết

- gzip `dashboard.html` và `login.html` tại build time giống provisioning page;
- embed file `.gz` và trả `Content-Encoding: gzip`;
- dùng tên asset/version hoặc ETag nếu muốn browser cache chắc chắn;
- giữ HTML auth-sensitive ở chính sách cache phù hợp, nhưng không nhầm `no-cache` với `no-store`;
- CSS/font có thể tiếp tục dùng cache dài vì là asset tĩnh.

#### Tiêu chí nghiệm thu

- response dashboard có `Content-Encoding: gzip`;
- payload dashboard ở cấu hình hiện tại nhỏ hơn 25 KiB;
- CSP và HTML render không thay đổi sau nén;
- login page, dashboard và captive provisioning đều được kiểm tra riêng.

#### Kết quả triển khai

- `GET /` và `GET /login` trả `Content-Encoding: gzip` trong gateway mode.
- Build giữ nguyên CSP và chính sách `Cache-Control: no-cache` của HTML.
- Đã kiểm tra `gzip -t` và giải nén so khớp byte-for-byte với nguồn build.

---

### 5.6 P1 — Unit test chưa bao phủ component auth — Đã bổ sung

Trước sửa đổi, `components/web_auth/test/test_web_auth.c` tồn tại nhưng `web_auth` chưa có trong `TEST_COMPONENTS` của test project.

Bộ test cũ mới kiểm tra:

- init;
- default status;
- username validation;
- password length validation.

Bộ test cũ chưa có test cho:

- PBKDF2 hash/verify;
- session token create/validate;
- login end-to-end;
- cookie flow;
- expiration;
- NVS state transitions;
- rate limit;
- performance target.

#### Kết quả triển khai

- Đã thêm `web_auth` vào `TEST_COMPONENTS` và khôi phục CMake registration còn thiếu của `memory_policy/test`.
- Đã bổ sung PBKDF2 vector/độ dài password, session round-trip, mutation, alphabet, logout chọn lọc và enable-login-validate-logout flow.
- Test app đã build/link thành công; chưa flash/chạy Unity vì phiên làm việc không có bo và cổng serial.
- Expiration/rate-limit và HTTP route matrix vẫn nên được bổ sung thành integration tests trên phần cứng.

#### Phương hướng giải quyết

- thêm `web_auth` vào `TEST_COMPONENTS`;
- thêm test dependency và reset NVS namespace giữa test cases;
- thêm test vector PBKDF2 chuẩn;
- thêm session round-trip và expiry test;
- thêm HTTP integration test cho redirect/login/cookie/protected API;
- thêm benchmark chạy trên hardware, nhưng không biến thời gian tuyệt đối thành unit test dễ flaky.

---

## 6. Các vấn đề correctness/security liên quan

Các mục sau không phải nguyên nhân chính của độ trễ nhưng cần xử lý cùng đợt vì ảnh hưởng flow auth.

### 6.1 Route protection chưa đồng nhất — Đã sửa các route phát hiện

Các route gateway `/api/status`, `/api/restart` và `/api/ble/*` hiện đã đi qua middleware. `/api/status` ở provisioning mode vẫn public theo chủ đích vì auth subsystem đầy đủ chưa khởi tạo trong mode này.

Phần còn lại là thêm integration test kiểm tra bảng route ở cả auth OFF, auth ON/no session và auth ON/valid session.

### 6.2 Logout không yêu cầu session và xóa toàn bộ session — Đã sửa

`POST /api/auth/logout` hiện yêu cầu session hợp lệ khi auth ON và chỉ xóa session khớp token. `invalidate_all_sessions()` vẫn được dùng cho thay đổi credential hoặc auth state.

Policy đã triển khai:

- yêu cầu cookie hợp lệ khi auth ON;
- chỉ hủy session tương ứng với token hiện tại;
- giữ `invalidate_all_sessions()` cho password change, username change, disable auth hoặc admin operation có chủ đích.

### 6.3 Cookie lifetime không khớp session lifetime — Đã sửa

Policy được chọn là cookie sống đến absolute timeout; server tiếp tục enforce cả idle timeout và absolute timeout. Cookie bị clear khi logout hoặc thay đổi cấu hình yêu cầu đăng nhập lại.

Cookie dùng `Max-Age` bằng absolute timeout; idle timeout vẫn do session table phía server quyết định.

### 6.4 Decode table không phân biệt ký tự không hợp lệ — Đã sửa

Decode table tự viết đã được loại bỏ. Creator dùng base64 encoder chuẩn; validator kiểm tra trực tiếp đúng 43 ký tự và alphabet base64url trước khi hash.

### 6.5 NVS read bỏ qua lỗi của phần lớn field — Đã sửa

Store hiện kiểm tra exact size và kết quả đọc của mọi field, schema version, username NUL/format, trạng thái enabled và iteration range. Record thiếu/hỏng không còn được coi là credential hợp lệ; middleware fail closed nếu runtime auth state lỗi.

Các validation đã áp dụng:

- schema version;
- trạng thái trả về của từng field;
- username terminator/length;
- PBKDF2 iteration range;
- tính nhất quán giữa `enabled` và credential.

---

## 7. Kiến trúc sau triển khai

```text
                          +----------------------+
Boot / config mutation -->| Runtime auth state   |
                          | enabled/configured   |
                          +----------+-----------+
                                     |
HTTP request                         v
    -> middleware -> auth OFF ------> handler
                  -> auth ON
                      -> parse cookie
                      -> hash token
                      -> session table RAM
                      -> handler

Login request
    -> load credential NVS
    -> optimized PBKDF2 verify
    -> create canonical token
    -> store token hash in RAM
    -> set cookie

Enable/disable/change credential
    -> verify current password when required
    -> write and commit NVS
    -> update runtime state
    -> invalidate sessions according to policy
```

Nguyên tắc chính:

- NVS không nằm trên hot path của request thông thường;
- PBKDF2 chỉ chạy khi login hoặc thao tác nhạy cảm;
- session validation chỉ dùng dữ liệu RAM;
- UI gộp dữ liệu Settings vào một request;
- static HTML được nén tại build time;
- route policy có test tự động.

---

## 8. Trạng thái triển khai

### Phase 0 — Đo baseline — Chờ phần cứng

Thêm timing log tạm thời hoặc telemetry cho:

- tổng thời gian `POST /api/auth/login`;
- thời gian PBKDF2;
- thời gian `web_auth_require_request()`;
- số lần `web_auth_store_load()`;
- thời gian gửi dashboard HTML;
- thời gian từ `DOMContentLoaded` đến khi device list được render.

Đo tối thiểu ba lần cho mỗi kịch bản và báo cáo median/p95 nếu đủ mẫu.

### Phase 1 — Sửa correctness P0 — Hoàn thành trong source

1. Sửa canonical base64url session token.
2. Sửa PBKDF2 key handling.
3. Dùng PBKDF2 primitive chuẩn.
4. Thêm session round-trip và PBKDF2 vector tests.

Không nên tối ưu UI trước khi login/session hoạt động đúng, vì kết quả đo khi token luôn fail không phản ánh flow thực tế.

### Phase 2 — Loại NVS khỏi request hot path — Hoàn thành

1. Hoàn thiện runtime auth state.
2. Tách `is auth enabled` khỏi `validate session`.
3. Session validation chỉ dùng RAM.
4. Thêm counter/test bảo đảm request thường không đọc NVS.

### Phase 3 — Tối ưu Web UI và asset — Hoàn thành phần chính

1. Đã dùng một aggregate Settings API.
2. Đã loại request waterfall và GET MCP-token riêng.
3. Cân nhắc lazy-load aggregate API theo tab.
4. Đã gzip dashboard/login.
5. Định nghĩa cache policy và validator cho asset.

### Phase 4 — Hoàn thiện policy và regression tests — Hoàn thành một phần

1. Đã bảo vệ các route quản trị bị thiếu trong audit.
2. Đã sửa logout chỉ hủy session hiện tại.
3. Đã đồng bộ cookie với absolute lifetime.
4. Còn thêm integration test auth OFF/ON cho toàn bộ route.
5. Đã build firmware và unit-test project riêng biệt; còn flash/chạy trên bo.

---

## 9. Ma trận kiểm thử tối thiểu

| Kịch bản | Kết quả mong đợi |
|---|---|
| Auth OFF, `GET /` | Trả dashboard, không đọc auth NVS theo request |
| Auth ON, không cookie, `GET /` | `303 /login` |
| Auth ON, login đúng | Trả success, cookie 43 ký tự |
| Cookie vừa cấp, `GET /` | Trả dashboard, không redirect |
| Login sai 5 lần | Kích hoạt rate limit theo policy |
| Session idle hết hạn | Protected API trả 401 hoặc root redirect login |
| Session absolute hết hạn | Session bị từ chối |
| Logout browser A | Chỉ session A bị hủy |
| Change password | Mọi session cũ bị hủy |
| Disable auth | Dashboard/API hoạt động không cookie |
| Enable lại auth | Yêu cầu current password và session mới |
| Password dài 8, 32, 64 byte | Hash/verify thành công |
| Dashboard load Settings | Chỉ một `GET /api/settings`; không GET status/token riêng |
| Dashboard gzip | Render đúng, CSP không lỗi |
| NVS record thiếu field | Fail closed hoặc recovery theo policy rõ ràng |

---

## 10. Chỉ số hiệu năng đề xuất

Các target cần được xác nhận trên ESP32-S3 thật:

| Chỉ số | Target đề xuất |
|---|---:|
| PBKDF2 verify | ≤ 300 ms |
| Middleware với session hợp lệ | ≤ 5 ms, không NVS I/O |
| `GET /` đến byte đầu tiên | ≤ 100 ms khi đã có session |
| Dashboard compressed payload | < 25 KiB |
| Device list ready sau `DOMContentLoaded` | ≤ 500 ms trong LAN ổn định |
| Read request cho toàn bộ Settings state | 1 |
| Auth NVS read trên protected request hợp lệ | 0 |

Nếu target không đạt, log cần tách rõ thời gian:

```text
cookie parse
auth state lookup
session SHA-256/lookup
NVS I/O
handler business logic
response send
```

---

## 11. Trạng thái ưu tiên cuối cùng

| Trạng thái | Công việc | Ghi chú |
|---|---|---|
| Đã làm | Sửa token create/validate | Canonical base64url 43 ký tự và test round-trip |
| Đã làm | Thay implementation PBKDF2 và sửa key size | PSA PBKDF2 primitive; còn benchmark hardware |
| Đã làm | Loại NVS khỏi session validation | Protected request dùng runtime state/session RAM |
| Đã làm | Aggregate Settings API và bỏ GET MCP-token riêng | Một read API cho Settings |
| Một phần | Bổ sung `web_auth` tests và HTTP integration tests | Unity unit flow đã có; route matrix/runtime còn thiếu |
| Đã làm | Hoàn thiện route/logout policy | Các route audit đã bảo vệ; logout chọn lọc |
| Đã làm | Gzip dashboard/login | Dashboard payload 18.962 byte |
| Đã làm | Đồng bộ cookie/session lifetime | Cookie dùng absolute timeout, server enforce idle |
| Chờ bo | Benchmark PBKDF2/middleware/UI và chạy Unity | Cần ESP32-S3, Wi-Fi và serial monitor |

---

## 12. Kết luận

Việc tích hợp auth làm dashboard chậm hơn trước đây có cơ sở từ flow cũ. Đợt sửa đổi này đã loại các nguyên nhân có thể xử lý trực tiếp trong source:

- token creator/validator đã thống nhất, không còn redirect lặp do token mismatch;
- PBKDF2 dùng primitive chuẩn thay vì import/destroy key theo từng iteration;
- protected request không còn đọc auth NVS;
- Settings dùng một aggregate read API và MCP token ngắn gọn `mcp_` + 32 hex;
- dashboard/login được gửi gzip; dashboard còn khoảng 18,5 KiB;
- route, logout, cookie lifetime và NVS validation đã được làm nhất quán.

Firmware và test app đều build/link thành công. Chưa nên giảm 60.000 iterations một cách cảm tính: bước tiếp theo là flash lên ESP32-S3, chạy Unity và đo median/p95 cho login, middleware, TTFB và thời gian render. Chỉ chuyển PBKDF2 sang worker async hoặc điều chỉnh iteration khi số liệu phần cứng cho thấy cần thiết.
