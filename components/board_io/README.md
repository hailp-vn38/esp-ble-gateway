# board_io

Abstraction cho I/O vật lý của board gateway: button reset/factory-reset,
status LED, display tùy chọn (text frame 4x32). Spec đầy đủ:
`docs/Board_IO_Development_Spec_v2.0.md` (v2.1 prod-hardening).

## Nguyên tắc

- Application nói bằng semantic intent (`board_io_set_status`,
  `board_io_signal`, `board_io_display_update`); component quyết định
  GPIO level, debounce, pattern timing.
- Button chỉ emit semantic event: `SHORT_PRESS`, `RESTART_REQUEST`,
  `FACTORY_RESET_REQUEST`. Không restart/xóa NVS bên trong component.
- Public API thread-safe từ task context, không ISR-safe.
- Không phụ thuộc Wi-Fi/BLE/device_store/web/mcp.

## Cấu hình

Feature mặc định tắt (`Kconfig`). Bật production config trong
`sdkconfig.defaults.esp32s3` chỉ sau khi schematic xác nhận pin
(spec mục 39/76). Pin validation runtime chặn blocklist ESP32-S3:
22–25 (chip), 26–32 (flash), 33–37 (octal PSRAM), cảnh báo strapping
0/3/45/46 và UART0 43/44.

## Unit tests

Component nằm trong `TEST_COMPONENTS` của `test/` project; build và flash
theo hướng dẫn ở repo root `AGENTS.md`. HIL (button thật, LED polarity,
provisioning/reconnect LED) là test thủ công theo spec mục 62–66.
