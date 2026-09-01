# AGENTS.md

ESP-IDF firmware for ESP32-S3 (BLE Central gateway with Web UI / REST / JSON-RPC). Verified against ESP-IDF 6.1.0, 16 MiB flash. Design docs and README are in Vietnamese.

## Phase tracking rule
Mọi migration/feature lớn được chia theo phase phải có một plan doc riêng trong docs/ chứa checklist chi tiết cho từng phase (mã phase dạng <PREFIX>-01, <PREFIX>-02, …). Nếu có nhiều bản plan doc (tóm tắt + chi tiết) cho cùng một việc, tất cả đều phải được cập nhật đồng bộ.

Ngay khi một phase hoàn thành, trước khi làm bất kỳ việc gì khác: mở plan doc tương ứng, đánh [x] toàn bộ checklist của phase đó, và thêm "✅ DONE (YYYY-MM-DD)" vào heading của phase đó — trong TẤT CẢ plan doc liên quan đến việc này.

Không cần liệt kê tiến độ trong agent.md — plan doc là nguồn sự thật duy nhất về trạng thái từng phase. Trước khi bắt đầu hoặc tiếp tục bất kỳ phase nào, đọc plan doc để biết phase nào đã DONE, đang làm, hay chưa bắt đầu.

## Build

Requires ESP-IDF environment (`idf.py` on PATH). QCBOR is a git submodule — run first after clone or it will be missing:

path idf : "/Users/lamphuchai/.espressif/v6.1-rc1/esp-idf"

```sh
git submodule update --init --recursive
idf.py set-target esp32s3   # required once before first build
idf.py build
```

- Two **separate** ESP-IDF projects live here: the firmware (repo root) and the unit-test app (`test/`). Each needs its own `idf.py set-target esp32s3` and build; building one does not build the other.
- `MINIMAL_BUILD ON` is set in both projects: components are only compiled if reachable from `main`'s dependency graph (`REQUIRES` in each component's CMakeLists). A new component that builds but isn't linked is usually missing from `REQUIRES`.
- Target-specific config goes in `sdkconfig.defaults.esp32s3`; shared config in `sdkconfig.defaults`. Changing target/chip requires rechecking `CONFIG_BT_NIMBLE_MAX_CONNECTIONS` (currently 9) and partition size.
- Do not edit generated `sdkconfig` directly; change defaults files and delete/regenerate.

## Tests

Unity tests flash to real hardware (no host runner) and self-run once at boot, then drop into the Unity menu:

```sh
cd test
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

- The component list under test is hardcoded in `TEST_COMPONENTS` in `test/CMakeLists.txt`. Adding a component's `test/` dir requires updating that string.
- End-to-end Wi-Fi/BLE/reconnect/ACK behavior needs a physical ESP32-S3 plus a peripheral advertising service UUID `0xABF0`.

## Web assets are embedded

Dashboard source lives in `components/web_server/www_src/` — assembled into a single `dashboard.html` by `tools/build_webui.py`, then gzipped. `setup.html` (provisioning UI) is gzipped separately. Both are compiled into firmware via `EMBED_FILES` in `components/web_server/CMakeLists.txt`. Editing any web asset requires a full rebuild+reflash; they are not served from a filesystem.

## Architecture notes

- Boot has two modes: provisioning mode (no valid Wi-Fi creds) initializes only NVS/Wi-Fi APSTA/captive DNS/HTTP config routes (SSID: `ESP-GW-<MAC>`, password `gateway123`); Device Store, Dispatcher, BLE Central, reconnect supervisor, and MCP endpoint initialize only after STA mode gets an IP. Anything touching those modules must handle being called in either mode.
- BLE wire format is CBOR with numeric map keys; schema lives in `components/cbor_codec/cbor_codec.c`. Gateway protocol version is 4 (defined as `GW_PROTOCOL_VERSION` in `cbor_codec.h`), rejects v1/v2/v3 messages. Peripheral devices implement service `0xABF0`, char `0xABF1` (write), `0xABF2` (notify).
- Components: `device_store` (NVS registry), `device_schema` (tools+features discovery/validation/persistence), `wifi_provisioning`, `ble_central` (NimBLE), `cbor_codec`, `command_dispatcher` (registry + per-device ACK routing + JSON ACK payload propagation), `command_executor` (worker task for commands), `web_server`, `mcp_endpoint` (JSON-RPC subset at `POST /mcp`, no auth — LAN only), `mcp_tool_exposure` (dynamic tool exposure/catalog), `mcp_ws_bridge` (WebSocket bridge for MCP), `memory_policy`, `board_io`, `gateway_status`, `qcbor_lib` (submodule wrapper).
- HTTP server receive timeout (`CONFIG_HTTPD_RECV_TIMEOUT_SEC=5`) is coupled to the command ACK timeout — don't lower it below ACK wait + buffer.
