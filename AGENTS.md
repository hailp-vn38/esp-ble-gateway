# AGENTS.md

ESP-IDF firmware for ESP32-S3 (BLE Central gateway with Web UI / REST / JSON-RPC). Verified against ESP-IDF 5.4.4, 16 MiB flash. Design docs and README are in Vietnamese.

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

`components/web_server/www/*` (dashboard.html, setup.html, CSS, font) are compiled into the firmware via `EMBED_FILES` in `components/web_server/CMakeLists.txt`. `setup.html` is gzipped at build time by `gzip_asset.py`. Editing assets requires a full rebuild+reflash to take effect; they are not served from the filesystem.

## Architecture notes

- Boot has two modes: provisioning mode (no valid Wi-Fi creds) initializes only NVS/Wi-Fi APSTA/captive DNS/HTTP config routes; Device Store, Dispatcher, BLE Central, reconnect supervisor, and MCP endpoint initialize only after STA mode gets an IP. Anything touching those modules must handle being called in either mode.
- BLE wire format is CBOR with numeric map keys; schema lives in `components/cbor_codec/cbor_codec.c`. Gateway protocol version is 3 (defined as `GW_PROTOCOL_VERSION` in `cbor_codec.h`), still accepts v1/v2 messages. Peripheral devices implement service `0xABF0`, char `0xABF1` (write), `0xABF2` (notify).
- Components: `device_store` (NVS registry), `device_capabilities` (capability cache/discovery/validation with operation token serializer), `wifi_provisioning`, `ble_central` (NimBLE), `cbor_codec`, `command_dispatcher` (registry + per-device ACK routing + JSON ACK payload propagation), `command_executor` (worker task for commands), `web_server`, `mcp_endpoint` (JSON-RPC subset at `POST /mcp`, no auth — LAN only), `board_io`, `gateway_status`, `qcbor_lib` (submodule wrapper).
- HTTP server receive timeout (`CONFIG_HTTPD_RECV_TIMEOUT_SEC=5`) is coupled to the command ACK timeout — don't lower it below ACK wait + buffer.
