# Plan: Tách `device_command_service.c` thành các module

**Ngày:** 2026-09-04
**Component:** `components/device_command_service/`
**Hiện trạng:** `device_command_service.c` — 672 dòng, trộn public API, queue worker, pending table, validation và transport
**Mục tiêu:** module hóa theo trách nhiệm, giữ nguyên public API và hành vi wire protocol v4

## Phase DCS-01: Module split ✅ DONE (2026-09-04)

- [x] Tạo internal header chứa type/config và interface nội bộ
- [x] Giữ lifecycle, public API, stats và transport hooks trong `device_command_service.c`
- [x] Tách validation và wire-message mapping sang `device_command_request.c`
- [x] Tách pending-request table và request-id allocation sang `device_command_pending.c`
- [x] Tách event handlers, timeout và service task sang `device_command_worker.c`
- [x] Đăng ký đầy đủ source mới trong `CMakeLists.txt`
- [x] Không đổi `include/device_command_service.h` và public API

## Phase DCS-02: Verification ✅ DONE (2026-09-04)

- [x] `git diff --check` pass
- [x] Firmware `idf.py build` pass bằng ESP-IDF 6.1
- [x] Unit-test app `test/` build pass bằng ESP-IDF 6.1
- [x] Xác nhận component mới vẫn được link dưới `MINIMAL_BUILD`
