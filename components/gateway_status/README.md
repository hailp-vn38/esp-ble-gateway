# Gateway Status

## 1. Tổng quan

`gateway_status` là **single source of truth** cho trạng thái gateway (Plan v2 §44–47). Mọi consumer đọc cùng một implementation — không còn bản aggregate riêng từng nơi.

```text
REST GET /api/status ──────────┐
                               ▼
                    gateway_status_get(&status)
                               ▲
dispatcher get_status ─────────┘
      ▲
      └── MCP tools/call get_status (qua dispatcher)
```

Nguyên tắc:

* Không block trên BLE I/O; an toàn gọi từ HTTPD task lẫn dispatcher handler.
* Chỉ aggregate snapshot; không format JSON, không biết gì về transport.
* Hoạt động đúng ở cả provisioning mode (Wi-Fi STA chưa có → rssi = null).

---

## 2. Các file chính

```text
components/gateway_status/
├── gateway_status.c            # Aggregate từ device_store / wifi_prov / ble_central
├── include/gateway_status.h    # Struct + API
└── CMakeLists.txt
```

---

## 3. Public API

```c
esp_err_t gateway_status_get(gateway_status_t *status); // ESP_ERR_INVALID_ARG nếu NULL
```

## 4. `gateway_status_t`

| Trường | Nguồn | Ghi chú |
|---|---|---|
| `device_count`, `connected_count` | `device_store_snapshot()` | |
| `ble_link_count` | `ble_central_active_count()` | |
| `ip`, `wifi_connected`, `provisioning`, `wifi_state[24]` | `wifi_prov_*` | |
| `free_heap`, `uptime_ms` | `esp_get_free_heap_size()`, `esp_timer` | |
| `firmware_version[32]`, `idf_version[32]` | `esp_app_get_description()` | truncate nếu dài hơn |
| `wifi_ssid[33]`, `has_wifi_rssi`, `wifi_rssi` | `esp_wifi_sta_get_ap_info()` | rssi chỉ có khi STA connected |
| `wifi_mac[18]` | `esp_wifi_get_mac(STA)` | `"00:00:00:00:00:00"` khi lỗi |

---

## 5. Consumers

| Consumer | Cách dùng |
|---|---|
| `web_system_api.c` `/api/status` | Fill struct → format cJSON giữ nguyên field name cũ (UI không vỡ) |
| `command_dispatcher/gateway_commands.c` `get_status` | Format payload JSON rút gọn `{"status":"ok","device_count":N,"connected_count":N,"ble_link_count":N}` — wire contract không đổi |

Thêm consumer mới: REQUIRES `gateway_status`, gọi `gateway_status_get()`. Không tự aggregate lại.

---

## 6. Dependency

```text
gateway_status ──► device_store, wifi_provisioning, ble_central,
                   esp_wifi, esp_app_format, esp_timer, esp_system
```

Component lá — không phụ thuộc web_server/dispatcher/MCP (core dependency invariant §4.6).
