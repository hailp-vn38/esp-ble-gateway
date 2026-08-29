# ESP32 BLE Gateway

Firmware ESP-IDF cho ESP32-S3, hoạt động như một BLE Central kết nối tới các
thiết bị DIY và cung cấp Web UI, REST API cùng endpoint JSON-RPC qua Wi-Fi.

## Chức năng

- Device Store lưu tối đa 16 thiết bị, metadata và địa chỉ BLE trong NVS; có
  migration schema và API snapshot an toàn khi nhiều task cùng truy cập.
- Wi-Fi dùng STA khi credentials đã được kiểm tra và tự fallback về SoftAP có
  captive DNS khi kết nối thất bại. Provisioning chạy ở boot mode tối giản và
  restart sau khi lưu credentials thành công.
- NimBLE Central quét theo service UUID, quản lý tối đa 9 link trên ESP32-S3,
  pairing/bonding, GATT discovery, subscribe CCCD, reconnect và discovery
  timeout.
- Message BLE là CBOR chuẩn qua QCBOR 1.6.1; JSON dùng cJSON 1.7.19~2.
- Dispatcher có registry động, định tuyến gateway/device command và chờ ACK
  riêng cho từng thiết bị.
- Capability Discovery tự hỏi peripheral sau khi BLE session READY, lưu snapshot
  command vào NVS và validate kiểu/range argument trước khi gửi.
- Web UI quản lý Wi-Fi, quét BLE, CRUD thiết bị, gửi lệnh và xem log/status.
- Dashboard không chồng request định kỳ: trạng thái/thiết bị cập nhật mỗi 5 giây,
  log mỗi 10 giây; lệnh thiết bị chạy trên worker riêng để không khóa HTTP task.
- `POST /mcp` hỗ trợ subset JSON-RPC 2.0 gồm `tools/list` và `tools/call`
  theo MCP 2026-07-28, bao gồm notification không có `id`.
- **MCP Dynamic Tool Exposure**: mỗi command của thiết bị có thể được expose
  như một MCP tool riêng (ví dụ `fan_01.set_speed`), quản lý qua Admin API
  hoặc dashboard, persist trong NVS, tự reconcile khi boot.

## Giao thức BLE

Thiết bị con cần quảng bá và triển khai các UUID 16-bit sau:

- Service: `0xABF0`
- Command characteristic (write without response): `0xABF1`
- Status characteristic (notify): `0xABF2`
- CCCD chuẩn: `0x2902`

Gateway dùng protocol version `3` và vẫn nhận message v1/v2. Payload CBOR là map
có key số để giảm kích thước; schema nằm trong
`components/cbor_codec/cbor_codec.c`. Peripheral v3 nhận command reserved
`describe_capabilities`, rồi notify `capabilities_begin`, các
`capability_item`, `capabilities_end` và ACK cuối cùng.

## Build và flash

Project được kiểm tra với ESP-IDF v6.1-rc1, target ESP32-S3 và flash 16 MiB.
QCBOR được ghim bằng Git submodule; cJSON được ESP-IDF Component Manager tải
theo manifest.

```sh
git submodule update --init --recursive
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

Nếu đổi target, cần kiểm tra lại giới hạn `CONFIG_BT_NIMBLE_MAX_CONNECTIONS`
và dung lượng partition.

## Wi-Fi lần đầu

Khi chưa có credentials hợp lệ, gateway tạo mạng provisioning:

- SSID: `ESP-GW-<MAC>` (prefix `ESP-GW` + last 4 hex của MAC)
- Password: `gateway123`
- URL: `http://192.168.4.1/`

Ở provisioning boot, firmware chỉ khởi tạo NVS, Wi-Fi APSTA, captive DNS và
HTTP server với 8 route cấu hình. Device Store, Dispatcher, BLE Central,
reconnect supervisor và MCP chưa được khởi tạo.

Captive DNS (`dns_hijack`) chỉ chạy trong provisioning: mọi truy vấn tên miền
được trả về `192.168.4.1` để điện thoại/máy tính mở trang cấu hình. Task này giữ
hoạt động trong lúc kiểm tra Wi-Fi và được hệ thống giải phóng khi gateway
restart sang STA-only.

Gateway chỉ ghi credentials vào NVS sau khi STA thực sự nhận được IP. Scan và
kiểm tra credentials chạy trong worker task để không chặn web server. Lệnh cấu
hình trả về ngay; UI polling trạng thái và gateway restart sau 4 giây khi kết
nối thành công. Ở lần boot kế tiếp, firmware kiểm tra Wi-Fi đã lưu, chạy
STA-only và chỉ sau khi nhận IP mới khởi tạo toàn bộ module gateway.

Ở gateway boot, Wi-Fi power-save được tắt để giảm độ trễ REST/BLE. Cấu hình này
phù hợp thiết bị dùng nguồn liên tục nhưng sẽ tăng mức tiêu thụ điện.

```sh
curl -X POST http://192.168.4.1/api/wifi/scan
curl http://192.168.4.1/api/wifi/scan

curl -X POST http://192.168.4.1/api/wifi \
  -H 'Content-Type: application/json' \
  -d '{"ssid":"TEN_WIFI","password":"MAT_KHAU"}'

curl http://192.168.4.1/api/wifi
```

## REST API

### API thiết bị

| Method | Path | Mục đích |
|---|---|---|
| `GET` | `/` | Web UI theo boot mode |
| `GET` | `/api/status` | Trạng thái provisioning hoặc gateway |
| `GET` | `/api/devices` | Danh sách thiết bị |
| `POST` | `/api/devices` | Thêm thiết bị |
| `PUT` | `/api/devices` | Sửa tên hoặc loại thiết bị |
| `DELETE` | `/api/devices?device_id=...` | Xóa thiết bị |
| `POST` | `/api/command` | Gửi lệnh tới thiết bị và chờ ACK |
| `GET` | `/api/capabilities?device_id=...` | Capability snapshot của thiết bị |
| `POST` | `/api/capabilities/refresh` | Yêu cầu discovery lại capability |

### API Wi-Fi (chỉ provisioning mode)

| Method | Path | Mục đích |
|---|---|---|
| `POST` | `/api/wifi/scan` | Bắt đầu quét Wi-Fi nền |
| `GET` | `/api/wifi/scan` | Trạng thái và kết quả Wi-Fi scan đã cache |
| `POST` | `/api/wifi` | Bắt đầu kiểm tra credentials nền |
| `GET` | `/api/wifi` | Trạng thái job cấu hình Wi-Fi |

### API BLE

| Method | Path | Mục đích |
|---|---|---|
| `GET` | `/api/ble/scan` | Kết quả quét BLE đã cache |
| `POST` | `/api/ble/scan` | Bắt đầu quét BLE |

### Admin API — MCP Tool Exposure

Yêu cầu header `Authorization: Bearer <admin_token>` (xem Kconfig bên dưới).

| Method | Path | Mục đích |
|---|---|---|
| `GET` | `/api/mcp/exposures?device_id=...` | Danh sách command + trạng thái exposure |
| `PUT` | `/api/mcp/exposures` | Bật/tắt một hoặc nhiều command |

```sh
# Xem exposure của thiết bị
curl http://<GATEWAY_IP>/api/mcp/exposures?device_id=fan_01 \
  -H "Authorization: Bearer gw-admin-token-2026"

# Bật tool
curl -X PUT http://<GATEWAY_IP>/api/mcp/exposures \
  -H "Authorization: Bearer gw-admin-token-2026" \
  -H "Content-Type: application/json" \
  -d '{"device_id":"fan_01","command":"set_speed","enabled":true}'

# Tắt tool
curl -X PUT http://<GATEWAY_IP>/api/mcp/exposures \
  -H "Authorization: Bearer gw-admin-token-2026" \
  -H "Content-Type: application/json" \
  -d '{"device_id":"fan_01","command":"set_speed","enabled":false}'
```

### Ví dụ thêm thiết bị

```sh
curl -X POST http://<GATEWAY_IP>/api/devices \
  -H 'Content-Type: application/json' \
  -d '{"device_id":"lamp-1","name":"Đèn bàn","type":"light","ble_addr":"11:22:33:44:55:66","ble_addr_type":0}'
```

## MCP Endpoint (JSON-RPC)

Endpoint `POST /mcp` hỗ trợ MCP 2026-07-28, bao gồm `server/discover`,
`tools/list` và `tools/call`. Chi tiết đầy đủ xem [`docs/MCP_API.md`](docs/MCP_API.md).

### Static tools

Luôn có sẵn: `get_status`, `list_devices`, và tùy chọn `device_command` /
`list_device_capabilities` (tắt mặc định).

### Dynamic device tools

Mỗi command của thiết bị có thể được expose như MCP tool riêng. Ví dụ thiết bị
`fan_01` có command `set_speed` sẽ xuất hiện dưới tên `fan_01.set_speed`:

```sh
# Liệt kê tools (bao gồm static + dynamic)
curl -X POST http://<GATEWAY_IP>/mcp \
  -H 'Content-Type: application/json' \
  -H 'MCP-Protocol-Version: 2026-07-28' \
  -H 'Mcp-Method: tools/list' \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}}}}'

# Gọi dynamic tool trực tiếp
curl -X POST http://<GATEWAY_IP>/mcp \
  -H 'Content-Type: application/json' \
  -H 'MCP-Protocol-Version: 2026-07-28' \
  -H 'Mcp-Method: tools/call' \
  -H 'Mcp-Name: fan_01.set_speed' \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"fan_01.set_speed","arguments":{"value":60},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}}}}'
```

### Legacy `device_command`

```sh
curl -X POST http://<GATEWAY_IP>/mcp \
  -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","method":"tools/call","params":{"name":"device_command","arguments":{"device_id":"lamp-1","command":"toggle","bool_value":true}},"id":2}'
```

Endpoint này là JSON-RPC/MCP subset cho LAN. Không nên expose trực tiếp ra Internet.

## Cấu hình

### Kconfig quan trọng

| Tùy chọn | Mặc định | Mô tả |
|---|---|---|
| `CONFIG_MCP_AUTH_TOKEN` | `""` | Bearer token cho `/mcp` (rỗng = dev mode) |
| `CONFIG_MCP_HOST_ALLOWLIST` | `"gateway.local,192.168.4.1"` | Danh sách Host header hợp lệ |
| `CONFIG_MCP_DEVICE_COMMAND_ALLOWLIST` | `""` | Command thiết bị được phép qua `device_command` (rỗng = deny all) |
| `CONFIG_MCP_COMPAT_2025` | `y` | Hỗ trợ initialize flow MCP 2024/2025 cũ |
| `CONFIG_MCP_RATE_LIMIT_RPS` | `10` | Giới hạn request/giây cho `/mcp` (token bucket, capacity 10) |
| `CONFIG_MCP_TOOLS_CACHE_TTL_MS` | `60000` | TTL cache `tools/list` trả về cho client |
| `CONFIG_MCP_DYNAMIC_TOOLS` | `y` | Bật dynamic tool exposure |
| `CONFIG_MCP_DYNAMIC_TOOL_MAX_ENABLED` | `32` | Số tool dynamic tối đa đồng thời |
| `CONFIG_MCP_EXPOSURE_RECORD_MAX` | `96` | Số record exposure NVS tối đa |
| `CONFIG_MCP_DYNAMIC_ALLOW_DESTRUCTIVE` | `n` | Cho phép expose destructive commands |
| `CONFIG_WEB_ADMIN_AUTH_TOKEN` | `""` | Bearer token cho Admin API exposure |
| `CONFIG_CMD_EXEC_WORKER_COUNT` | `2` | Số persistent command worker |
| `CONFIG_CMD_EXEC_JOB_TIMEOUT_MS` | `3000` | Deadline end-to-end của một command |
| `CONFIG_MCP_WS_BRIDGE` | `y` | Bật WebSocket bridge tới external MCP broker |

### Dashboard Web UI

Dashboard quản lý thiết bị có sẵn tại `http://<GATEWAY_IP>/`. Các tab:

- **My Devices**: danh sách thiết bị, xem chi tiết, gửi lệnh, refresh capability
- **Add Device**: quét BLE, thêm thiết bị mới
- **MCP Tools**: quản lý exposure — chọn device, bật/tắt từng command
- **Gateway Settings**: system info, network, admin token, restart

Admin token cho MCP Tools lưu trong browser (localStorage). Cần nhập token
ở tab Settings trước khi sử dụng MCP Tools.

## Unit test

Test app riêng bao phủ 13 component: Device Store, Device Capabilities, CBOR,
Command Dispatcher, Command Executor, MCP Endpoint, MCP Tool Exposure, MCP WS
Bridge, Wi-Fi Provisioning, BLE Central, Web Server, Board I/O, Memory Policy.
Test được compile tách biệt khỏi firmware production:

```sh
cd test
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

Các test tự chạy một lượt khi boot rồi mở Unity menu. Test end-to-end Wi-Fi,
BLE discovery, reconnect và ACK vẫn cần board ESP32-S3 thật cùng một peripheral
triển khai service `0xABF0`.

## Cấu trúc

```text
main/                           Khởi động và nối các module
components/device_store/         NVS device registry
components/device_capabilities/  Capability cache, discovery và validation
components/wifi_provisioning/    Wi-Fi STA/SoftAP và captive DNS
components/ble_central/          NimBLE Central/GATT Client
components/cbor_codec/           QCBOR và JSON codec
components/command_dispatcher/   Command registry, ACK routing
components/command_executor/     Worker task chạy command offline
components/web_server/           Web UI, REST API và admin auth
components/mcp_endpoint/         JSON-RPC/MCP endpoint
components/mcp_tool_exposure/    Dynamic tool exposure, catalog, naming, digest
components/mcp_ws_bridge/        WebSocket bridge tới external MCP broker
components/memory_policy/        PSRAM/internal allocation policy
components/board_io/             Button FSM và LED status
components/gateway_status/       Gateway status tracking
components/qcbor_lib/            QCBOR 1.6.1 submodule wrapper
test/                            Unity unit-test application
docs/                            Thiết kế, spec và API documentation
```

## Tài liệu

- [`docs/MCP_API.md`](docs/MCP_API.md) — MCP endpoint API reference (static + dynamic tools)
- [`docs/MCP_DYNAMIC_DEVICE_TOOLS_DASHBOARD_EXPOSURE_SPEC_v1.1.md`](docs/MCP_DYNAMIC_DEVICE_TOOLS_DASHBOARD_EXPOSURE_SPEC_v1.1.md) — Spec thiết kế dynamic tool exposure
- [`docs/MCP_ENDPOINT_DUAL_ERA_UPDATE_PLAN_v1.1.md`](docs/MCP_ENDPOINT_DUAL_ERA_UPDATE_PLAN_v1.1.md) — Kế hoạch cập nhật dual-era MCP
- [`docs/MCP_MINIMAL_TOOLS_SERVER_REFACTOR_SPEC_V3_1.md`](docs/MCP_MINIMAL_TOOLS_SERVER_REFACTOR_SPEC_V3_1.md) — Spec refactor minimal tools server
- [`docs/ESP_BLE_GATEWAY_XIAOZHI_DIRECT_MCP_BRIDGE_DEVELOPMENT_SPEC_v1.1.md`](docs/ESP_BLE_GATEWAY_XIAOZHI_DIRECT_MCP_BRIDGE_DEVELOPMENT_SPEC_v1.1.md) — Spec WebSocket bridge tới Xiaozhi MCP broker
- [`docs/ESP32_S3_Gateway_Memory_Resource_Implementation_Guide_IDF_6_1_rc1_v2_1.md`](docs/ESP32_S3_Gateway_Memory_Resource_Implementation_Guide_IDF_6_1_rc1_v2_1.md) — Hướng dẫn quản lý PSRAM/internal memory
- [`docs/Board_IO_Development_Spec_v2.0.md`](docs/Board_IO_Development_Spec_v2.0.md) — Spec button FSM, LED, display
- [`docs/WEB_DASHBOARD_SETTINGS_DEVELOPMENT_SPEC_v1.1.md`](docs/WEB_DASHBOARD_SETTINGS_DEVELOPMENT_SPEC_v1.1.md) — Spec tab Settings trên dashboard
