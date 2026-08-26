# Web Server

Lớp HTTP của gateway: Web UI nhúng + REST + captive portal. Chạy `esp_http_server` ở hai chế độ **độc excluaive**: provisioning (chưa có Wi-Fi) hoặc gateway (đầy đủ, `/mcp` do `mcp_endpoint` đăng ký thêm trên cùng server).

## Files

```text
web_server.c        # start server 2 chế độ; route capacity constants
web_http.c/.h       # helper chung: JSON/error/body/route registration
web_assets.c        # assets nhúng (EMBED_FILES), CSP/security headers
web_gateway_api.c   # /api/devices, /api/command
web_system_api.c    # /api/status, /api/logs, /api/restart
web_ble_api.c       # /api/ble/scan (esp_timer + deadline guard)
web_wifi_api.c      # /api/wifi*, chỉ đăng ký ở provisioning mode
www/                # dashboard.html, setup.html (gzip build-time), css, font
```

Server config: Gateway = 21 URI slots / stack 12288 · Provisioning = 14 / 8192. Keep-alive on; recv/send timeout 5s.

## Execution model

- Mọi mutation (`POST /api/command`, POST/PUT/DELETE `/api/devices`) và MCP device_command đi qua `command_executor` — HTTPD task không chờ BLE ACK. GET đọc giữ sync.
- Body: deadline tuyệt đối 3s; limits per endpoint (devices 512 / command 1024 / wifi 256). Oversize → **413 + Connection: close**; slow body → **408 + close**; body đã consume hết thì keep-alive được giữ.
- BLE scan: auto-stop ~6s bằng esp_timer one-shot + deadline guard (stale callback không kill scan mới); POST khi đang scan = idempotent.

## Routes — Gateway mode

| Route | Mô tả |
|---|---|
| `GET /` , `/dashboard.css`, `/icons.css`, `/assets/Phosphor.woff2`, `/favicon.ico` | Dashboard UI nhúng (CSP + nosniff + Referrer-Policy) |
| `GET /api/devices` | List device (sync qua dispatcher `list_devices`) |
| `POST /api/devices` / `PUT` | add/edit device — async executor |
| `DELETE /api/devices?device_id=` | Xóa device — async executor |
| `POST /api/command` | Gửi device_command tới BLE peripheral — async executor |
| `GET /api/status` | Snapshot từ `gateway_status` + block `"executor"` metrics |
| `GET /api/logs` | Log gần đây từ `log_buffer` |
| `POST /api/restart` | Schedule restart sau 1s |
| `GET/POST/DELETE /api/ble/scan` | Quét BLE: kết quả cache 20 thiết bị, auto-stop 6s |

## Routes — Provisioning mode

| Route | Mô tả |
|---|---|
| `GET /` | `setup.html.gz` (có chặn request inject AdGuard) |
| `/generate_204`, `/hotspot-detect.html`, `/connecttest.txt`, `/ncsi.txt` | Captive portal redirect → `/` |
| `GET /api/status`, `GET /api/logs` | Bản rút gọn cho setup UI |
| `POST /api/wifi/scan` · `GET` | Bắt đầu quét Wi-Fi (202) · lấy kết quả |
| `GET /api/wifi` · `POST` | Trạng thái job connect · test+save credential (202, restart 4s sau khi thành công) |

## Response envelope

```json
{"success": true, "status": 0, "message": "...", "data": {...},
 "error": {"code": "device_not_connected"}}
```

`error.code` chỉ xuất hiện khi thất bại: `invalid_request`, `payload_too_large`, `request_timeout`, `device_not_found`, `device_busy`, `device_not_connected`, `command_timeout`, `transport_error`, `device_error`, `internal_error`.

HTTP mapping: OK→200 · invalid→400 · not_found→404 · busy→409 · timeout→504 · not_connected/transport/device_error→502 · queue-full→503.

## Lưu ý

- Assets compile vào firmware (`EMBED_FILES`); sửa `www/*` phải rebuild + reflash mới có hiệu lực.
- Không chứa MCP protocol logic — xem `mcp_endpoint`.
- Tests HTTP thật: xem `docs/refactor-execution-plan.md`; unit suite ở project `test/` (web_server chưa có test component riêng).
