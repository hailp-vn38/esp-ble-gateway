# Gateway Status

Single source of truth cho trạng thái gateway. Consumers: `/api/status` (REST), dispatcher `get_status` (và MCP qua dispatcher).

## API

```c
esp_err_t gateway_status_get(gateway_status_t *status); // ESP_ERR_INVALID_ARG nếu NULL
```

Không block BLE I/O; an toàn gọi từ HTTPD task hoặc dispatcher handler; chạy đúng ở provisioning mode.

## Nguồn dữ liệu

| Trường | Nguồn |
|---|---|
| `device_count` | `device_store_snapshot()` |
| `connected_count` | `ble_central_get_device_status()` (merge với snapshot) |
| `ble_link_count` | `ble_central_active_count()` |
| `ip`, `wifi_connected`, `provisioning`, `wifi_state` | `wifi_prov_*` |
| `free_heap`, `uptime_ms`, `firmware_version`, `idf_version` | esp_system / esp_timer / app_desc |
| `wifi_ssid`, `has_wifi_rssi`, `wifi_rssi`, `wifi_mac` | `esp_wifi_sta_get_ap_info()`, `esp_wifi_get_mac(STA)` |

Thêm consumer mới: REQUIRES component này và gọi API — không tự aggregate.
