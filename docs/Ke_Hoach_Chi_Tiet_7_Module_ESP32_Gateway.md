# Kế hoạch chi tiết hoàn thiện 7 Module — ESP32 BLE Gateway (Giai đoạn 1)

**Đối tượng:** Tài liệu hướng dẫn triển khai chi tiết cho codebase khung đã cung cấp (`esp32-ble-gateway.zip` và `device-module.zip`).
**Mục tiêu:** Đưa mỗi module từ trạng thái "khung chạy được" (skeleton) sang trạng thái "hoàn thiện, ổn định, sẵn sàng test thực tế".

---

## Cách đọc tài liệu này

Mỗi module được trình bày theo 5 phần cố định:
- **Mô tả & vai trò** — module này làm gì, tại sao cần, quan hệ với module khác.
- **Flow hoạt động** — luồng xử lý theo từng bước, có sơ đồ trạng thái nếu cần.
- **Việc cần hoàn thiện** — liệt kê cụ thể phần code hiện là khung/placeholder cần thay bằng logic thật.
- **Hướng dẫn kiểm thử** — cách test độc lập module trước khi tích hợp toàn hệ thống.
- **Rủi ro & lưu ý** — các lỗi thường gặp, giới hạn kỹ thuật cần nhớ.

---

## Module 1 — Device Store (NVS Storage Layer)

### Mô tả & vai trò
Module quản lý danh sách thiết bị BLE đã đăng ký (device_id, name, type), lưu bền qua NVS flash. Đây là "nguồn sự thật" (source of truth) cho toàn bộ hệ thống biết gateway đang quản lý những thiết bị nào — mọi module khác (BLE Central, Dispatcher, Web UI) đều đọc/ghi qua đây, không module nào tự lưu danh sách riêng.

### Flow hoạt động
1. Khi khởi động (`device_store_init()`), đọc namespace `dev_list` từ NVS vào RAM cache (mảng `s_cache`).
2. Mọi thao tác add/delete/edit đều: (a) sửa RAM cache trước, (b) ghi lại toàn bộ xuống NVS, (c) rollback RAM nếu ghi NVS lỗi.
3. Các module khác đọc dữ liệu qua `device_store_list()` hoặc `device_store_find()` — luôn đọc từ RAM cache (nhanh), không đọc trực tiếp NVS mỗi lần.
4. Trường `connected` là runtime-only, không lưu NVS — được `ble_central` cập nhật qua `device_store_set_connected()` mỗi khi connect/disconnect.

```
[Boot] → load_from_nvs() → RAM cache
              │
   ┌──────────┼──────────┐
   ▼          ▼          ▼
add_device  delete_device  edit_device
   │          │          │
   └──────────┴──────────┘
              ▼
      save_to_nvs() (ghi lai toan bo)
```

### Việc cần hoàn thiện
- **Bổ sung trường MAC address BLE**: hiện `device_entry_t` chưa có trường lưu địa chỉ BLE (6 byte) — cần thêm để gateway tự động `ble_central_connect()` lại đúng thiết bị sau reboot, thay vì phải scan lại từ đầu mỗi lần.
- **Cơ chế ghi NVS hiệu quả hơn**: hiện `save_to_nvs()` ghi lại toàn bộ danh sách mỗi lần add/delete — chấp nhận được ở quy mô ~16 thiết bị, nhưng nên chuyển sang ghi từng entry riêng (key theo index) nếu mở rộng lớn hơn, để giảm số byte ghi mỗi lần.
- **Xử lý version cấu hình (migration)**: nếu sau này đổi cấu trúc `device_entry_t`, cần thêm trường `schema_version` trong NVS để phát hiện và xử lý dữ liệu cũ không khớp cấu trúc mới.

### Hướng dẫn kiểm thử
Dùng Unity test framework có sẵn trong ESP-IDF — tạo thư mục `components/device_store/test/test_device_store.c`, khai báo `REQUIRES unity` trong `CMakeLists.txt` của thư mục test[web:97][web:98]:

```c
TEST_CASE("add and find device", "[device_store]") {
    device_store_init();
    TEST_ASSERT_EQUAL(0, device_store_add("dev_A", "Den phong khach", "relay"));
    device_entry_t *e = device_store_find("dev_A");
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_STRING("Den phong khach", e->name);
}
```

Build và chạy test trên target thật bằng `idf.py -T device_store build flash monitor`[web:99]. Kiểm thử thủ công bổ sung: add 5 thiết bị giả, gọi `esp_restart()`, xác nhận danh sách còn nguyên sau khi `device_store_init()` chạy lại.

### Rủi ro & lưu ý
NVS namespace tối đa 15 ký tự — `"dev_list"` (8 ký tự) an toàn, nhưng nếu thêm namespace mới nhớ kiểm tra độ dài. Giới hạn ghi flash ~100,000 lần/sector — vì module này chỉ ghi khi người dùng chủ động add/delete/edit (không phải theo sự kiện dữ liệu), nên không có rủi ro hao mòn trong sử dụng bình thường.

---

## Module 2 — Wi-Fi Provisioning (SoftAP)

### Mô tả & vai trò
Module xử lý việc thiết lập kết nối Wi-Fi lần đầu theo chuẩn IoT quen thuộc: gateway tự phát Access Point riêng, người dùng kết nối vào để nhập SSID/password mạng LAN thật, sau đó gateway chuyển sang chế độ Station (STA) để tham gia mạng đó.

### Flow hoạt động
```
[Boot] → co Wi-Fi da luu trong NVS?
           │                    │
          Co                  Chua
           │                    │
           ▼                    ▼
    start_sta()           start_softap()
    (ket noi LAN)          (phat AP rieng)
           │                    │
     STA_CONNECTED       Nguoi dung ket noi AP
     → co IP LAN          → mo browser 192.168.4.1
     → Web UI hoat dong   → submit form Wi-Fi
                          → wifi_prov_save_and_connect()
                          → luu NVS → can restart de chuyen STA
```

Cơ chế retry: nếu STA disconnect, tự động `esp_wifi_connect()` lại tối đa 5 lần (`STA_CONNECT_RETRY`) trước khi coi là thất bại hẳn.

### Việc cần hoàn thiện
- **Chuyển đổi mode "hot" không cần restart**: hiện `wifi_prov_save_and_connect()` chỉ lưu NVS và ghi log nhắc "restart để áp dụng" — cần bổ sung logic gọi `esp_wifi_stop()` → `esp_wifi_set_mode(WIFI_MODE_STA)` → `esp_wifi_start()` ngay lập tức để UX mượt hơn, không bắt người dùng rút nguồn.
- **Fallback tự động về SoftAP**: khi STA thất bại sau 5 lần retry, hiện chỉ log lỗi — nên bổ sung logic tự chuyển lại SoftAP để người dùng có thể sửa lại thông tin Wi-Fi sai mà không cần factory reset.
- **Captive portal tự động redirect (tùy chọn UX nâng cao)**: hiện người dùng phải tự mở browser và gõ `192.168.4.1` — có thể bổ sung DNS hijack để tự động hiện trang cấu hình khi kết nối AP, giống trải nghiệm router thương mại.
- **Trang HTML cho form Wi-Fi**: code hiện có endpoint `/api/wifi` nhận JSON nhưng chưa có trang HTML riêng cho bước SoftAP — cần thêm route `GET /` trả về form đơn giản khi gateway đang ở SoftAP mode (khác với trang quản lý thiết bị khi đã STA).

### Hướng dẫn kiểm thử
Test thủ công theo 3 tình huống: (1) lần đầu boot chưa có Wi-Fi — xác nhận SoftAP `ESP32-Gateway-Setup` xuất hiện khi scan Wi-Fi từ điện thoại; (2) submit form với SSID/password đúng — xác nhận gateway lấy được IP LAN (theo dõi qua Serial Monitor dòng `Got IP:`); (3) submit sai password — xác nhận retry đúng 5 lần rồi dừng, không crash hoặc treo.

### Rủi ro & lưu ý
Mật khẩu SoftAP mặc định (`gateway123`) là hardcode trong code mẫu — nên đổi hoặc random hóa (dựa theo MAC address) trước khi phân phối rộng, để tránh nhiều gateway dùng chung mật khẩu AP dễ đoán.

---

## Module 3 — BLE Central (NimBLE)

### Mô tả & vai trò
Module lõi kỹ thuật quan trọng nhất — quản lý toàn bộ kết nối BLE tới các thiết bị DIY con, đóng vai trò GATT Client (Central). Chịu trách nhiệm: kết nối, discover characteristic, gửi lệnh (Write Command), nhận phản hồi (Notify), và duy trì connection pool cho tới ~10-16 thiết bị đồng thời.

### Flow hoạt động

**Luồng kết nối một thiết bị:**
```
ble_central_connect(device_id, mac, addr_type)
        │
   alloc_slot() -- het slot? --> return -1
        │
   ble_gap_connect() voi connection params (itvl=15ms, latency=0, timeout=150ms)
        │
   [Async] BLE_GAP_EVENT_CONNECT
        │
   ble_gattc_disc_all_chrs() -- tim COMMAND (0xABF1) va STATUS (0xABF2)
        │
   on_chr_disc() luu val_handle cho tung characteristic
        │
   Neu tim thay STATUS -- ghi CCCD (0x01,0x00) -- subscribe Notify
        │
   Ket noi hoan tat -- device_store_set_connected(id, 1)
```

**Luồng gửi lệnh và nhận phản hồi:**
```
command_dispatcher --> ble_central_send_command(device_id, msg)
        │
   cbor_codec_encode(msg) --> buffer
        │
   ble_gattc_write_no_rsp_flat() --> gui xuong COMMAND characteristic
        │
   [Thiet bi xu ly, tu Notify status nguoc lai]
        │
   BLE_GAP_EVENT_NOTIFY_RX --> cbor_codec_decode() --> notify_cb() --> dispatcher log
```

**Luồng mất kết nối:**
```
BLE_GAP_EVENT_DISCONNECT --> device_store_set_connected(id, 0)
        │
   slot.conn_handle = NONE (giu device_id, in_use=1 de co the reconnect)
```

### Việc cần hoàn thiện
- **Cơ chế reconnect tự động**: hiện khi disconnect, slot chỉ reset `conn_handle` mà chưa tự động gọi lại `ble_central_connect()`. Cần thêm timer FreeRTOS retry (ví dụ mỗi 5-10 giây thử reconnect nếu thiết bị có trong `device_store` nhưng chưa connected).
- **Scan để tìm thiết bị mới**: code hiện giả định đã biết MAC address trước (qua tham số `ble_addr`) — cần bổ sung hàm `ble_central_scan_start()` dùng `ble_gap_disc()` để quét thiết bị advertising theo UUID service `0xABF0`, giúp gateway tự phát hiện thiết bị mới thay vì phải nhập MAC thủ công.
- **Xử lý timeout khi discover characteristic thất bại**: nếu `ble_gattc_disc_all_chrs()` không tìm thấy đúng UUID (thiết bị lỗi/khác version), hiện code không có timeout hoặc fallback — nên thêm cơ chế hủy kết nối và báo lỗi rõ ràng sau X giây không tìm thấy characteristic.
- **Điều chỉnh connection interval động theo số lượng kết nối**: theo khuyến nghị kỹ thuật đã thống nhất, khi số kết nối tăng cần tăng CI tương ứng để tránh nghẽn airtime — hiện code dùng CI cố định 15ms cho mọi kết nối, cần logic tính CI động dựa trên `BLE_CENTRAL_MAX_CONN` đang active.

### Hướng dẫn kiểm thử
Đây là module rủi ro kỹ thuật cao nhất — nên test bằng phần cứng thật sớm nhất có thể, dùng chính `device-module` (firmware Peripheral) đã cung cấp, flash lên 2-3 board ESP32 khác làm thiết bị giả lập. Kịch bản test tăng dần:

1. Kết nối 1 thiết bị, gửi lệnh `toggle`, xác nhận Notify phản hồi đúng qua Serial Monitor.
2. Tăng lên 3, rồi 5 thiết bị đồng thời — đo thời gian round-trip mỗi lệnh (log timestamp lúc gửi và lúc nhận Notify).
3. Test vật cản: đặt 1-2 thiết bị sau tường/hộp, quan sát tần suất `BLE_GAP_EVENT_DISCONNECT` bất thường trong 30 phút.
4. Test giới hạn: tăng dần tới 10 thiết bị, theo dõi RAM còn trống (`heap_caps_get_free_size()`) và log lỗi từ NimBLE stack.

### Rủi ro & lưu ý
`BLE_CENTRAL_MAX_CONN` (16) trong header phải khớp với `CONFIG_BT_NIMBLE_MAX_CONNECTIONS` trong `sdkconfig.defaults` — nếu sửa một bên mà quên bên kia, code sẽ compile được nhưng runtime bị giới hạn sai. CCCD handle được tính bằng `val_handle + 1` — đây là giả định phổ biến nhưng không phải chuẩn tuyệt đối; nếu thiết bị con có descriptor khác chèn giữa, cần discover CCCD riêng bằng `ble_gattc_disc_all_dscs()` thay vì cộng cứng[web:105].

---

## Module 4 — CBOR Codec

### Mô tả & vai trò
Lớp chuyển đổi dữ liệu — encode/decode message nội bộ giữa 2 định dạng: binary gọn nhẹ (dùng qua BLE, giới hạn MTU) và JSON (dùng ở Web UI/MCP, dễ đọc/debug). Đây là "ngôn ngữ chung" giữa gateway và mọi thiết bị con.

### Flow hoạt động
```
Tu BLE (nhan Notify)          Tu Web UI/MCP (nhan HTTP request)
        │                              │
  cbor_codec_decode()          cbor_codec_json_to_msg()
        │                              │
        └──────────┬───────────────────┘
                    ▼
              gw_message_t (struct chuan noi bo)
                    │
        ┌───────────┴───────────┐
        ▼                       ▼
cbor_codec_encode()      cbor_codec_msg_to_json()
   (gui xuong BLE)         (tra ve Web UI/MCP)
```

### Việc cần hoàn thiện
- **Thay placeholder bằng thư viện CBOR thật**: đây là việc quan trọng nhất của module này. Layout binary hiện tại (length-prefixed fields) chỉ để test luồng — cần tích hợp QCBOR (khuyến nghị, nhẹ và có API rõ ràng) hoặc libcbor qua ESP Component Registry (`idf.py add-dependency`).
- **Thay JSON string-matching bằng cJSON**: parser JSON hiện dùng `strstr`/`sscanf` thủ công — hoạt động với schema đơn giản/flat nhưng dễ vỡ với dữ liệu có ký tự đặc biệt hoặc nested object. Nên chuyển sang `cJSON` (có sẵn trong ESP-IDF qua `REQUIRES json`) khi payload phức tạp hơn (ví dụ mảng nhiều tham số cho một lệnh).
- **Mở rộng schema cho payload phức tạp**: `gw_message_t` hiện chỉ có `int_value`/`bool_value` — nếu về sau cần gửi float, string, hoặc mảng giá trị (ví dụ set nhiều thông số cùng lúc), cần thiết kế lại schema dùng CBOR map lồng nhau thay vì struct cố định.

### Hướng dẫn kiểm thử
Test round-trip đơn giản nhất trong Unity test — encode một message mẫu, decode lại, so sánh từng field:

```c
TEST_CASE("encode decode round trip", "[cbor_codec]") {
    gw_message_t msg = {.type="device_command", .device_id="dev_A",
                         .command="toggle", .bool_value=1, .has_device_id=1};
    uint8_t buf[256];
    int len = cbor_codec_encode(&msg, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, len);

    gw_message_t decoded;
    TEST_ASSERT_EQUAL(0, cbor_codec_decode(buf, len, &decoded));
    TEST_ASSERT_EQUAL_STRING("toggle", decoded.command);
}
```

Sau khi thay bằng QCBOR thật, test thêm interoperability: encode từ gateway (C), decode bằng thư viện CBOR Python (`cbor2`) trên máy tính để xác nhận 2 phía hiểu đúng cùng 1 format — quan trọng nếu sau này có thêm client khác (Mac app) cũng cần đọc CBOR.

### Rủi ro & lưu ý
Khi đổi từ placeholder sang CBOR thật, **phải đổi đồng thời cả 2 phía** — `cbor_codec.c` trong gateway VÀ `cbor_msg.c` trong từng `device-module` — nếu chỉ đổi một bên, hai phía sẽ không hiểu nhau. Nên bump một "protocol version" field vào message để dễ debug nếu có thiết bị cũ chưa update chạy chung với gateway mới.

---

## Module 5 — Command Dispatcher & Registry

### Mô tả & vai trò
Bộ định tuyến lệnh trung tâm — nhận mọi message chuẩn hóa (bất kể nguồn: Web UI hay MCP), phân loại theo `type`, rồi dẫn tới đúng nơi xử lý: forward BLE (device_command) hoặc gọi hàm nội bộ đã đăng ký (gateway_command). Đây là điểm hội tụ giữa tất cả module khác.

### Flow hoạt động
```
command_dispatcher_handle(msg, result)
        │
   msg.type == ?
        │
   ┌────┴─────────────────┐
   ▼                       ▼
"device_command"      "gateway_command"
   │                       │
Kiem tra ble_central_    Tra bang s_registry[]
is_connected(device_id)   theo msg.command
   │                       │
Chua connect?             Tim thay?
   → result.success=0       │
   │                    ┌───┴────┐
Da connect              Co       Khong
   │                     │        │
ble_central_          Goi fn()   result.success=0
send_command()        (vd cmd_    "Unknown command"
   │                  add_device)
Ghi log_buffer
   │
result.success=1
```

### Việc cần hoàn thiện
- **Thread-safety khi gọi đồng thời**: Web UI (HTTP task) và MCP endpoint (cũng HTTP task, có thể khác connection) có thể gọi `command_dispatcher_handle()` đồng thời, cùng lúc truy cập `device_store` hoặc `ble_central` conn pool. Cần bổ sung FreeRTOS mutex (`xSemaphoreCreateMutex()`) bọc quanh các đoạn thao tác dùng chung, tránh race condition khi 2 request tới gần như cùng lúc.
- **Mở rộng `cmd_edit_device`**: hiện hàm này gọi `device_store_edit()` với `new_name`/`new_type` = NULL (không đổi gì thực sự) — cần parse thêm các field name/type mới từ `gw_message_t` (hiện struct chưa có chỗ chứa, cần mở rộng schema hoặc dùng convention riêng qua `command` field).
- **Cơ chế timeout cho device_command**: khi `ble_central_send_command()` gửi thành công (Write Command không có ACK ATT layer), dispatcher hiện coi đó là "success" ngay — nhưng thực tế thiết bị có thể không nhận được. Nên thiết kế application-layer ACK: dispatcher chờ Notify phản hồi trong X ms, nếu không có thì trả lỗi timeout cho client gọi (Web UI/MCP), đúng theo thiết kế "ACK tầng ứng dụng" đã thảo luận trước.

### Hướng dẫn kiểm thử
Test độc lập không cần phần cứng BLE — mock `ble_central_is_connected()` và `ble_central_send_command()` để luôn trả về giá trị giả định, tập trung test đúng logic routing:

```c
TEST_CASE("dispatch gateway_command list_devices", "[dispatcher]") {
    command_dispatcher_init();
    gw_message_t msg = {.type="gateway_command", .command="list_devices"};
    dispatch_result_t result;
    command_dispatcher_handle(&msg, &result);
    TEST_ASSERT_EQUAL(1, result.success);
}

TEST_CASE("dispatch unknown type returns error", "[dispatcher]") {
    gw_message_t msg = {.type="invalid_type"};
    dispatch_result_t result;
    command_dispatcher_handle(&msg, &result);
    TEST_ASSERT_EQUAL(0, result.success);
}
```

### Rủi ro & lưu ý
`DISPATCHER_MAX_COMMANDS` (16) là giới hạn tĩnh của registry — nếu số lệnh gateway mở rộng vượt quá, cần tăng hằng số này (đơn giản) nhưng nhớ kiểm tra RAM tổng thể không bị ảnh hưởng.

---

## Module 6 — Web Server & Web UI

### Mô tả & vai trò
Giao diện quản lý dành cho người dùng cuối — chạy trên chính ESP32, phục vụ trang HTML đơn giản để add/delete/edit thiết bị và xem log, thông qua REST API nội bộ gọi vào Command Dispatcher.

### Flow hoạt động
```
Browser --GET /--> index_get_handler() --> tra INDEX_HTML (co JS fetch())
                                                  │
                                          setInterval 3s --> refresh()
                                                  │
                            ┌─────────────────────┼─────────────────────┐
                            ▼                     ▼                     ▼
                    GET /api/devices      GET /api/logs         POST /api/devices
                            │                     │                     │
                    device_store_list()   log_buffer_get_all()   device_store_add()
                            │                     │                     │
                    Tra JSON array        Tra JSON array         Tra {success:bool}
```

### Việc cần hoàn thiện
- **Đồng bộ Web UI với Command Dispatcher**: hiện `devices_post_handler()`/`devices_delete_handler()` gọi trực tiếp `device_store_add()`/`device_store_delete()`, **bỏ qua** Command Dispatcher — vi phạm nguyên tắc "một lớp xử lý chung" đã thống nhất trong tài liệu khung. Cần sửa lại để build `gw_message_t` rồi gọi `command_dispatcher_handle()`, giống cách `mcp_endpoint` đang làm — đảm bảo log, validation, và logic tương lai (mutex, ACK) áp dụng đồng nhất cho cả 2 lối vào.
- **Parser JSON body thủ công cần thay bằng cJSON**: giống Module 4, `sscanf`/`strstr` trong `devices_post_handler()` dễ lỗi với input không chuẩn (thiếu field, ký tự đặc biệt trong tên thiết bị).
- **UI hiển thị trạng thái Wi-Fi/SoftAP**: hiện trang chính giả định đã ở STA mode — cần thêm logic JS kiểm tra `/api/status` để hiển thị đúng trang (form Wi-Fi khi đang SoftAP, trang quản lý khi đã STA).
- **Bổ sung nút gửi lệnh trực tiếp tới thiết bị**: hiện Web UI chỉ add/delete được thiết bị, chưa có UI để gửi `device_command` (ví dụ nút toggle) — cần thêm form/button gọi `POST` tới một endpoint mới (ví dụ `/api/command`) nối vào dispatcher.

### Hướng dẫn kiểm thử
Test qua browser thực tế trên điện thoại/máy tính cùng LAN với gateway. Dùng DevTools (Network tab) để xác nhận mỗi request `/api/*` trả đúng JSON structure. Test edge case: gửi tên thiết bị có ký tự `"` hoặc `\` để kiểm tra parser JSON có bị vỡ không (đây là lý do nên chuyển sang cJSON sớm).

### Rủi ro & lưu ý
`HTTPD_MAX_URI_HANDLERS=16` trong `sdkconfig.defaults` — nếu thêm nhiều route mới (ví dụ `/api/command`), cần tăng giá trị này tương ứng, nếu không `httpd_register_uri_handler()` sẽ thất bại âm thầm ở route vượt giới hạn.

---

## Module 7 — MCP Endpoint (JSON-RPC)

### Mô tả & vai trò
Cổng giao tiếp dành riêng cho AI Agent — nhận request theo chuẩn JSON-RPC 2.0 tối giản qua LAN, dịch thành message nội bộ, forward vào Command Dispatcher (dùng chung với Web UI), trả kết quả theo format JSON-RPC.

### Flow hoạt động
```
AI Agent --POST /mcp--> mcp_post_handler()
                              │
                    extract "method" field
                              │
              ┌───────────────┼───────────────┐
              ▼                               ▼
      method="list_tools"           method="call_tool"
              │                               │
      handle_list_tools()          handle_call_tool()
      (tra danh sach lenh          (build gw_message_t
       tinh, khong query           tu "params", suy luan
       dispatcher)                 type neu thieu)
              │                               │
              └───────────────┬───────────────┘
                              ▼
                    command_dispatcher_handle()
                              │
                    Tra JSON-RPC response
                    {jsonrpc, result/error, id}
```

### Việc cần hoàn thiện
- **`list_tools` nên lấy động từ registry thay vì hardcode**: hiện `handle_list_tools()` trả một chuỗi cố định danh sách lệnh — nên đổi `command_dispatcher` để expose một hàm `command_dispatcher_get_registered_names()`, giúp `list_tools` luôn phản ánh đúng lệnh thực tế đã đăng ký (đặc biệt quan trọng khi mở rộng thêm lệnh mới).
- **Validate JSON-RPC đúng chuẩn hơn**: hiện code không kiểm tra `jsonrpc: "2.0"` có đúng không, cũng không xử lý trường hợp thiếu `id` (notification trong JSON-RPC không cần response) — nên bổ sung validate cơ bản để tương thích tốt hơn với MCP client chuẩn.
- **Bảo mật endpoint (khi cần trong tương lai)**: hiện `/mcp` mở hoàn toàn không xác thực trong mạng LAN (đúng quyết định giai đoạn 1) — khi mở rộng, có thể cần thêm API key đơn giản qua HTTP header để tránh thiết bị lạ trong LAN gọi được lệnh điều khiển.
- **Timeout khi dispatcher xử lý lâu (device_command chờ ACK)**: nếu Module 5 sau này bổ sung cơ chế chờ ACK tầng ứng dụng, `mcp_post_handler()` cần đặt timeout hợp lý để không giữ HTTP connection quá lâu (mặc định `esp_http_server` có timeout riêng, cần kiểm tra không bị đá trước khi dispatcher trả lời).

### Hướng dẫn kiểm thử
Test bằng `curl` từ máy khác cùng LAN với gateway:

```bash
curl -X POST http://<gateway_ip>/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"call_tool","params":{"device_id":"dev_A","command":"toggle","bool_value":true},"id":1}'
```

Xác nhận response đúng format JSON-RPC và thiết bị (nếu đã kết nối BLE) nhận được lệnh. Test thêm `list_tools` để xác nhận danh sách tool trả về đúng. Với Postman/Insomnia, có thể lưu sẵn collection các request mẫu để test lại nhanh sau mỗi lần sửa code.

### Rủi ro & lưu ý
Vì `/mcp` và Web UI dùng chung một `httpd_handle_t` (thiết kế đúng theo yêu cầu ban đầu), số lượng route tổng cộng của cả 2 module phải nằm trong `HTTPD_MAX_URI_HANDLERS` — cần tính tổng khi mở rộng thêm route ở cả hai phía.

---

## Bảng tổng hợp mức độ hoàn thiện hiện tại

| Module | Trạng thái code | Việc quan trọng nhất cần làm |
|---|---|---|
| 1. Device Store | Chạy được, logic đầy đủ | Thêm trường MAC address để reconnect sau reboot |
| 2. Wi-Fi Provisioning | Chạy được, thiếu UX polish | Chuyển đổi STA "hot" không cần restart |
| 3. BLE Central | Chạy được, thiếu tự động hóa | Cơ chế reconnect tự động + scan tìm thiết bị mới |
| 4. CBOR Codec | Placeholder, cần thay thế | Tích hợp QCBOR + cJSON thật |
| 5. Command Dispatcher | Chạy được, thiếu an toàn luồng | Thêm mutex cho truy cập đồng thời |
| 6. Web Server & UI | Chạy được, vi phạm 1 nguyên tắc | Route qua dispatcher thay vì gọi device_store trực tiếp |
| 7. MCP Endpoint | Chạy được, cần chuẩn hóa hơn | Lấy tool list động từ registry |

## Thứ tự ưu tiên hoàn thiện đề xuất

1. **Module 6** (sửa vi phạm nguyên tắc dispatcher) — nhanh, quan trọng về mặt kiến trúc.
2. **Module 3** (reconnect + scan) — rủi ro kỹ thuật cao nhất, cần validate sớm bằng phần cứng thật.
3. **Module 4** (CBOR/cJSON thật) — cần làm trước khi test tích hợp sâu, tránh phải sửa lại nhiều nơi sau.
4. **Module 5** (mutex) — làm sau khi đã chạy ổn định với phần cứng, tránh tối ưu sớm.
5. **Module 1, 2, 7** — hoàn thiện dần theo nhu cầu thực tế phát sinh khi test.
