# Wi-Fi Provisioning

Vòng đời Wi-Fi của gateway: boot-connect bằng credential đã lưu, hoặc fallback vào **provisioning mode** — SoftAP + captive portal (DNS hijack + DHCP option 114 + HTTP funnel do `web_server` phục vụ). Không có hot-switch runtime: provisioning chỉ được bật lúc boot.

## Files

```text
wifi_prov.c          # state machine, NVS credential, SoftAP/STA lifecycle, event handler
dns_hijack.c         # DNS server UDP/53 trên IP SoftAP; lifecycle RUNNING/STOPPING
dns_packet.c         # parse/build DNS packet (pure, unit-testable)
captive_portal.c     # DHCP option 114: URI builder + stop/set/start DHCPS
include/             # wifi_prov.h, dns_*.h, captive_portal.h
test/                # unity: parser, lifecycle, uri builder
```

## Boot flow

```text
wifi_prov_init()
 ├─ có credential hợp lệ → STA connect (retry ≤ CONFIG_WIFI_PROV_STA_BOOT_RETRY_COUNT)
 │    ├─ GOT_IP trong timeout → CONNECTED (runtime disconnect → bounded reconnect, không mở portal)
 │    └─ fail → enter_provisioning()
 └─ không có credential → APSTA + SoftAP → enter_provisioning()
```

`enter_provisioning()`: đợi AP ready (5s) → đọc AP IPv4 → `captive_portal_configure_dhcp()` → `dns_hijack_start()` → state = PROVISIONING. Mỗi lớp captive độc lập: DHCP/DNS fail chỉ WARN, portal vẫn truy cập trực tiếp `http://<AP-IP>/`.

## States

| State | Ý nghĩa |
|---|---|
| `boot_connecting` | đang thử credential đã lưu |
| `provisioning` | SoftAP + DNS + DHCP-114 + HTTP active |
| `testing` | đang verify credential mới, AP/captive giữ nguyên |
| `restart_pending` | đã lưu NVS, chờ delayed reboot (~4s), portal vẫn sống tới reboot |
| `connected` / `reconnecting` / `failed` | STA runtime |

Invariants: workflow state tách biệt `s_sta_has_ip`; TESTING/RESTART_PENDING không teardown AP/DNS/DHCP/HTTP; event handler không bao giờ start/stop HTTP server; thành công luôn qua reboot, không hot-switch sang gateway mode.

## Captive portal stack

Ba lớp bổ trợ (defense-in-depth, đều chỉ tồn tại ở provisioning mode):

1. **DHCP option 114** (`ESP_NETIF_CAPTIVEPORTAL_URI`, toggle `CONFIG_WIFI_PROV_CAPTIVE_DHCP_OPTION_114`, default y): quảng bá `http://<AP-IP>/`. URI nằm trong buffer static của component vì IDF giữ con trỏ nguyên lifetime DHCPS. Helper stop→set→start DHCPS và **chỉ được gọi khi chưa có station nào giữ lease** (điểm gọi hiện tại: boot-time entry). Stop-fail vẫn thử restart trước khi báo lỗi.
2. **DNS hijack**: A query bất kỳ → IP SoftAP; AAAA/qtype khác IN-class → NOERROR-0-answers / NOTIMP; opcode ≠ QUERY → NOTIMP; malformed → drop; `local.adguard.org|.com` → NXDOMAIN (tránh xung đột extension AdGuard).
3. **HTTP funnel** (thuộc `web_server`): probe routes + mọi URI lạ redirect 303 `/` — xem README web_server.

## Credential test-and-save

`wifi_prov_test_and_save()`: validate arg → state TESTING → connect STA dưới APSTA → GOT_IP mới save NVS → RESTART_PENDING. Fail ở bước nào thì quay lại PROVISIONING, captive services không bị động đến. Legacy result codes: `0/-1..-6` (contract với `web_wifi_api.c`).

## Logs

Lifecycle INFO: SSID/AP-IP/URI sinh ra, state transitions, `DHCP option 114 advertised`, `Provisioning portal ready`. Degradation WARN khi thiếu DHCP-114 hoặc DNS. Không log password AP.

## Tests

Project `test/` (flash lên hardware, tự chạy lúc boot): `test_dns_parser`, `test_dns_lifecycle`, `test_captive_portal` (uri builder pure + arg validation). Verify option 114 thật sự xuất hiện trong DHCP Offer/ACK là device-level check (packet capture) — xem spec v3 §18–19.
