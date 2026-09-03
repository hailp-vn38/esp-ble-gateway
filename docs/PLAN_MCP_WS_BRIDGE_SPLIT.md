# Plan: Split mcp_ws_bridge.c into 3 modules

**Date:** 2026-09-03
**Component:** `components/mcp_ws_bridge/`
**Current:** `mcp_ws_bridge.c` — 1210 lines, monolithic
**Target:** 3 focused modules + 1 internal header

---

## Phase MWS-01: Module split ✅ DONE (2026-09-03)

- [x] Tạo internal header cho state và cross-module interface
- [x] Tách WebSocket transport sang `mcp_ws_bridge_ws.c`
- [x] Tách NVS/config API sang `mcp_ws_bridge_config.c`
- [x] Giữ orchestration và public lifecycle API trong `mcp_ws_bridge.c`
- [x] Đăng ký đầy đủ source mới trong `CMakeLists.txt`

### 1. `mcp_ws_bridge.c` — Core orchestration (~400 lines)

Khi `CONFIG_MCP_WS_BRIDGE` disabled: stub implementations (giữ nguyên).

Khi enabled:
- Shared state instance: `s_bridge`
- Memory diagnostics: `bridge_log_memory_snapshot()`
- Queue: `bridge_queue_event()`
- State helper: `bridge_set_state_locked()`, `bridge_invalidate_connection_locked()`
- Timer helpers: `timer_queue_callback()`, `bridge_stop_timer()`
- Connection lifecycle: `bridge_schedule_reconnect()`, `bridge_destroy_client()`
- WiFi event handler: `wifi_event_handler()`
- **Bridge task**: `bridge_task()` — main event loop
- Public API: `init`, `start`, `stop`, `reload`, `get_status`, `state_name`

### 2. `mcp_ws_bridge_ws.c` — WebSocket transport (~400 lines)

Wrapped in `#ifdef CONFIG_MCP_WS_BRIDGE`.
- Endpoint helpers: `mcp_ws_endpoint_valid()`, `mcp_ws_endpoint_display()`
- WS event handler: `mcp_ws_websocket_event_handler()`
- WS client connect: `mcp_ws_connect_client()`
- WS responder: `ws_responder_*` + `make_ws_responder()` + `send_not_ready()`
- RX handling: `mcp_ws_handle_rx_message()`
- TX handling: `mcp_ws_handle_tx_message()`
- Connection state: `mcp_ws_handle_connected()`, `mcp_ws_handle_disconnected()`

### 3. `mcp_ws_bridge_config.c` — NVS config + config API (~300 lines)

Wrapped in `#ifdef CONFIG_MCP_WS_BRIDGE`.
- Defaults: `default_enabled()`
- NVS: `mcp_ws_config_load()`, `mcp_ws_config_store()`
- Public API: `config_set()`, `config_get_public()`, `config_update()`, `config_clear()`, `config_load()`

### 4. `mcp_ws_bridge_internal.h` — Internal declarations

Cung cấp cross-module interface:
- `bridge_state_t` extern
- `bridge_queue_event()`, `bridge_set_state_locked()`, `bridge_invalidate_connection_locked()`
- `bridge_schedule_reconnect()`, `bridge_destroy_client()`, `bridge_stop_timer()`
- `mcp_ws_config_load()`, `mcp_ws_config_store()`
- `mcp_ws_endpoint_valid()`, `mcp_ws_endpoint_display()`
- Timer handles, WS client lifecycle

---

### Files

| File | Action |
|------|--------|
| `mcp_ws_bridge_internal.h` | Tạo mới |
| `mcp_ws_bridge_ws.c` | Tạo mới |
| `mcp_ws_bridge_config.c` | Tạo mới |
| `mcp_ws_bridge.c` | Giữ core + xóa code đã tách |
| `CMakeLists.txt` | Thêm SRCS |

---

## Phase MWS-02: Build and compatibility verification ✅ DONE (2026-09-03)

- [x] Internal header nạp `sdkconfig.h` trước khi kiểm tra
      `CONFIG_MCP_WS_BRIDGE`, tránh compile hai module mới thành object rỗng
- [x] Internal header khai báo đầy đủ dependency cho `esp_timer_handle_t`
- [x] Đặt `MCP_WS_STABLE_MS` trong module WebSocket đang sử dụng nó
- [x] `idf.py build` pass với `CONFIG_MCP_WS_BRIDGE=y`
- [x] Không đổi public API (`mcp_ws_bridge.h` giữ nguyên)
- [x] Build riêng pass với `CONFIG_MCP_WS_BRIDGE` disabled; stub
      implementations vẫn link được
- [x] Xác nhận các symbol config/WebSocket được định nghĩa bởi object mới
- [x] `git diff --check` pass
