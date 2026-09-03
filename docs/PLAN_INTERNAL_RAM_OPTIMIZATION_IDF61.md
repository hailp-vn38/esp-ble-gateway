# Kế hoạch tối ưu Internal RAM — ESP32-S3 / ESP-IDF 6.1

**Mã kế hoạch:** `RAM`  
**Target:** ESP32-S3, 16 MiB flash, 8 MiB Octal PSRAM, ESP-IDF 6.1.x  
**Nhánh gốc khi lập kế hoạch:** `dev-ws`  
**Ngày lập:** 2026-09-03  
**Trạng thái:** RAM-01 hoàn tất; các stress provisioning và 0/3/6/9 BLE links được loại trừ theo yêu cầu

## 1. Mục tiêu

- Giảm mức dùng và fragmentation của internal SRAM trong profile production.
- Giữ đủ vùng internal/DMA cho Wi-Fi, lwIP, NimBLE 9 links và HTTP server.
- Dùng PSRAM có chủ đích cho dữ liệu lớn, sống lâu hoặc có burst lớn.
- Đo được allocation fallback/failure và stack high-water của mọi application task.
- Ngăn OTA xác nhận một image chưa vượt qua full-init memory gate.
- Có bằng chứng build, flash, runtime và stress riêng biệt; không suy diễn gate này từ gate khác.

## 2. Ngoài phạm vi

- Không bật toàn cục `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`.
- Không chuyển NimBLE/controller task hoặc buffer DMA sang PSRAM.
- Không chuyển HTTPD stack sang PSRAM nếu chưa vượt qua cache-disabled/NVS/OTA stress.
- Không thay đổi giới hạn 9 BLE connections chỉ để làm đẹp số RAM.
- Không thay đổi wire protocol v4, REST contract hoặc MCP behavior ngoài phần telemetry bổ sung.

## 3. Nguyên tắc an toàn

1. Đo bằng `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`; `esp_get_free_heap_size()` chỉ là số tổng hợp tham khảo.
2. Large buffer/snapshot/catalog ưu tiên PSRAM; object đồng bộ, DMA, BLE-sensitive và ISR-visible giữ internal.
3. Không truy cập PSRAM, gọi allocator, log hoặc NVS trong `portENTER_CRITICAL()`.
4. Không chuyển stack của task có thể trực tiếp gọi flash/NVS sang PSRAM nếu chưa tách đường flash sang internal worker.
5. Mọi thay đổi allocator phải fail có kiểm soát, không âm thầm rút cạn internal reserve.
6. Mỗi phase dùng branch riêng từ commit đã xác nhận của phase trước; không merge vào `main` nếu chưa được yêu cầu.
7. Khi một phase hoàn thành, cập nhật heading thành `✅ DONE (YYYY-MM-DD)` và đánh `[x]` toàn bộ checklist của phase trước khi làm phase tiếp theo.

## 4. Baseline tĩnh khi lập kế hoạch

Build hiện tại của commit `b2f4f77` bằng ESP-IDF 6.1 đã thành công:

| Hạng mục | Giá trị |
|---|---:|
| DIRAM dùng tĩnh | 163,619 B / 341,760 B |
| DIRAM còn lại trước runtime allocation | 178,141 B |
| `.dram0.bss` | 46,408 B |
| `.dram0.data` | 23,352 B |
| `device_state.s_entries` | 8,448 B |
| `command_dispatcher.s_requests` | 7,744 B |
| `web_event_ws.s_ws` | 3,424 B |

Các buffer lớn đã dùng `GW_MEM_EXTERNAL_PREFERRED`: schema records, MCP exposure/catalog tables, command worker results, MCP shared dispatch result và MCP WS RX buffer.

Các vấn đề đã xác định để xử lý trong plan:

- cJSON chưa có allocator hook; node/key/string nhỏ vẫn ưu tiên internal heap.
- OTA self-test xác nhận image trước khi Wi-Fi/BLE/web/MCP full-init và chỉ kiểm tra heap khác 0.
- Pending ACK pool có 16 slot dù production command concurrency hiện là 2 workers.
- Chỉ command workers có stack high-water telemetry.
- `gw_mem_get_metrics()` chưa được đưa ra `/api/status`.
- `CONFIG_HTTPD_MAX_OPEN_SOCKETS=7` là symbol không tồn tại; socket budget chưa được gán trực tiếp vào `httpd_config_t`.

Kết nối tới `http://192.168.1.114/api/status` đã trả `200 OK` ngày 2026-09-03. Snapshot tham khảo trước khi flash: internal free `83,343 B`, min free `41,280 B`, largest block `36,864 B`; PSRAM free `7,718,436 B`, min free `7,705,132 B`. Đây chưa phải baseline RAM-01 vì firmware khi đó là `v1.0.0-38-gbde6a6e-dirty`, chưa gắn với ELF/build baseline của source hiện tại. Internal min free của snapshot này thấp hơn release gate 64 KiB và phải được điều tra trong RAM-01; snapshot sau flash được lưu trong report RAM-01.

## 5. Release gates chung

| Gate | Yêu cầu |
|---|---|
| Internal minimum free | `>= 65,536 B` trong mọi scenario bắt buộc |
| Internal largest free block | `>= 32,768 B` trong mọi scenario bắt buộc |
| PSRAM | Khởi tạo thành công, `psram_min_free > 0`, integrity pass |
| Allocation policy | Không external allocation failure; không fallback ngoài allowlist đã ghi rõ |
| Stack | High-water nhỏ nhất của từng application task `>= 1,024 B` sau stress |
| Heap integrity | `heap_caps_check_integrity_all(true)` pass sau mỗi stress group |
| Soak drift | Internal `<= 4 KiB/24h`, PSRAM `<= 16 KiB/24h` hoặc gate chặt hơn có bằng chứng |
| Stability | Không panic, watchdog, stack canary, cache-disabled assertion hoặc task-create failure |
| Functional | BLE READY/reconnect/ACK, REST, dashboard WS và MCP `tools/list`/`tools/call` vẫn đúng |

Nếu baseline chưa đạt 64 KiB/32 KiB, RAM-01 vẫn được phép hoàn thành khi số liệu và nguyên nhân đã được ghi đầy đủ. Các phase tối ưu sau không được coi là release-ready cho đến khi toàn bộ gate chung đạt.

## 6. Phase plan

## RAM-01 ✅ DONE (2026-09-03) — Instrumentation và runtime baseline

**Branch dự kiến:** `ram/01-observability-baseline`

### Công việc

- [x] Xác nhận HEAD, dirty worktree, ESP-IDF path/Python env, target `esp32s3` và config 9 BLE links.
- [x] Tạo build directory riêng `build-ram-baseline`; không tái sử dụng build tree khác IDF/config.
- [x] Build firmware và lưu `idf.py size`, `size-components`, map/ELF SHA vào report.
- [x] Đưa `gw_mem_get_metrics()` vào `gateway_status_t`, `/api/status` và dispatcher `get_status`.
- [x] Bổ sung metric cho preflight fallback rejection; phân biệt external-required fail, external-preferred fail và internal fallback.
- [x] Bổ sung stack high-water cho toàn bộ application task đang chạy; runtime aggregate tách system task.
- [x] Bổ sung queue high-water/drop cho BLE notify và queue device-schema; executor đã có queue metrics.
- [x] Thêm checkpoint memory có nhãn từ boot/NVS/Wi-Fi qua schema/exposure/executor/BLE tới gateway-ready.
- [x] Không log khi đang giữ spinlock/critical section; checkpoint snapshot/log độc lập ngoài lock.
- [x] Xác minh endpoint tại IP `192.168.1.114`; không hard-code IP này vào firmware/test.
- [x] Flash firmware baseline lên board và xác nhận app version/ELF SHA qua runtime.
- [x] Thu baseline STA full-service mode; provisioning stress **bỏ qua theo yêu cầu**.
- [x] Thu baseline 1 BLE link; stress 0/3/6/9 BLE links **bỏ qua theo yêu cầu**.
- [x] Xác minh MCP `tools/list` với bearer token: 2 static tools, 0 dynamic tools; các fixture 8/16/32 dynamic tools chưa có trong runtime profile.
- [x] REST `/api/status` pass; dashboard WS/Xiaozhi WSS stress không chạy (bridge disabled/profile scope).
- [x] Tạo `docs/reports/INTERNAL_RAM_BASELINE_IDF61.md` chứa raw metrics, scenario và giới hạn chưa kiểm chứng.
- [x] Build và flash unit-test app riêng; monitor tự động không chạy được vì môi trường không cấp TTY, sau đó đã reflash firmware production.

### Acceptance

- [x] Có số current/min/largest cho internal và PSRAM ở runtime STA; checkpoint logging đã được tích hợp cho các mốc lifecycle.
- [x] Có high-water của tất cả application task đang chạy; runtime aggregate unknown=0, application minimum=1,272 B.
- [x] Allocation fallback/failure có thể quan sát qua API và report.
- [x] Baseline được gắn với branch, commit, sdkconfig, ELF SHA, IDF version và hardware profile.
- [x] Không thay đổi placement/tuning trong phase đo baseline.

## RAM-02 ✅ DONE (2026-09-03) — Đưa cJSON workload ra PSRAM

**Branch dự kiến:** `ram/02-cjson-external-allocator`

**Trạng thái:** hoàn tất implementation và verification trong profile production hiện tại.

### Công việc

- [x] Liệt kê các call-site `cJSON_Parse`, `cJSON_Create*`, `cJSON_Duplicate` và `cJSON_Print*` trong production components.
- [x] Thêm wrapper allocator riêng cho cJSON và gọi `cJSON_InitHooks()` một lần sau NVS/PSRAM, trước khi khởi tạo service tasks.
- [x] Chọn `GW_MEM_EXTERNAL_PREFERRED`: PSRAM trước, fallback internal có floor/rejection telemetry.
- [x] Bảo đảm allocator/free hook là một cặp; output `cJSON_Print*` dùng `cJSON_free()`.
- [x] Thêm test capability check cho cJSON node trên PSRAM khi PSRAM khả dụng.
- [x] Kiểm tra các failure path parse/build/print và ownership cleanup; OOM injection đầy đủ dành cho test harness riêng.
- [x] Test `tools/list` profile hiện tại (2 static/0 dynamic), device schema REST/status API; ghi nhận fixture 8/16/32 chưa cấu hình.
- [x] Chạy concurrent REST + MCP requests; WS/BLE stress không có fixture hoạt động trong profile hiện tại.
- [x] So sánh internal min/largest với RAM-01: snapshot hiện tại cao hơn baseline RAM-01.

### Acceptance

- [x] cJSON tree/output dùng PSRAM theo policy đã chọn và có test capability check.
- [x] `tools/list` hiện tại trả JSON hợp lệ, không external allocation failure; fixture 32 tools chưa được cấu hình.
- [x] Internal minimum/largest runtime cao hơn baseline RAM-01.
- [x] Không còn `free()` trực tiếp trên pointer do cJSON hooks cấp phát.
- [x] REST/MCP regression profile hiện tại pass; WS/BLE stress fixture chưa có.

## RAM-03 ✅ DONE (2026-09-03) — Right-size static BSS và bounded pools

**Branch dự kiến:** `ram/03-static-pools`

### Công việc

- [x] Đã chứng minh production callers đi qua command executor; tối đa 2 ACK waiter đồng thời theo 2 worker.
- [x] Tách `DEVICE_REQUEST_MAX_PENDING` khỏi `DEVICE_STORE_MAX_DEVICES`, đặt mặc định 2.
- [x] Semaphore chỉ tạo cho 2 slot thực dùng.
- [x] ACK manager tests/build pass; các invariant BUSY, timeout, late ACK và request-id được giữ nguyên.
- [x] `device_state.s_entries` giữ internal vì truy cập dưới critical section.
- [x] Không chuyển device-state sang PSRAM; giữ synchronization hiện tại cache-safe.
- [x] WebSocket event ring tiếp tục giữ internal.
- [x] Schema/exposure/catalog/result workspace dùng policy PSRAM hiện hành khi phù hợp.
- [x] Không bật global external BSS placement.
- [x] `.bss` giảm từ 46,776 B (RAM-01) xuống 39,992 B; DIRAM giảm còn 157,235 B.

### Acceptance

- [x] Thu hồi 6,784 B internal SRAM từ ACK pool (16 → 2 slots).
- [x] Không giảm command concurrency/socket behavior đã công bố.
- [x] Không thay đổi locking/ownership; runtime không ghi nhận lỗi heap hoặc state loss.
- [x] Static map và runtime metric khớp: internal largest runtime 63,488 B, cao hơn RAM-02 baseline.

## RAM-04 — Stack workspace và task sizing

**Branch dự kiến:** `ram/04-stack-workspace-sizing`

### Công việc

- [ ] Sinh stack-frame report từ ELF/disassembly hoặc `-fstack-usage` cho application components.
- [ ] Lập call-chain budget cho các path lớn: device edit/exposure refresh, schema commit/persist, MCP denial/error, `tools/list`, WS drain và Wi-Fi scan.
- [ ] Chuyển large local workspace ra bounded PSRAM allocation khi lifetime/ownership rõ ràng.
- [ ] Loại `dispatch_result_t` 4 KiB khỏi các stack còn sót; tránh shared buffer nếu làm mất concurrency correctness.
- [ ] Giảm local copy của `device_schema_snapshot_t` bằng snapshot workspace/API phù hợp nếu số đo cho thấy cần thiết.
- [ ] Giữ BLE notify/identity/supervisor stack internal.
- [ ] Giữ task trực tiếp gọi NVS/flash trên internal stack, hoặc tách flash operation sang internal worker trước khi thử external stack.
- [ ] Giữ HTTPD stack internal trong cấu hình release mặc định.
- [ ] Chỉ giảm stack size sau stress high-water; mỗi lần thay đổi tối đa một nhóm task để A/B được.
- [ ] Bật compiler stack protection phù hợp cho qualification build; đánh giá chi phí trước khi đưa vào production defaults.
- [ ] Gán `config.max_open_sockets = 7` trực tiếp và loại Kconfig symbol không tồn tại.

### Acceptance

- [ ] Mọi task đạt high-water `>= 1,024 B` trong stress tương ứng.
- [ ] Không cache-disabled assertion khi NVS save/erase, OTA write và Wi-Fi reconnect.
- [ ] Không stack canary/watchdog/panic qua ít nhất 100 chu kỳ của từng path lớn.
- [ ] Socket budget đúng 7 và không có task-create/socket starvation regression.

## RAM-05 — OTA memory gate và degradation policy

**Branch dự kiến:** `ram/05-ota-memory-gate`

### Công việc

- [ ] Tách OTA validation thành early structural checks và late runtime-finalization.
- [ ] Không mark image valid trước khi profile tương ứng đã full-init.
- [ ] Với STA mode: finalize sau Device Store, schema, exposure, executor, NimBLE, reconnect supervisor, HTTPD và optional MCP WS init.
- [ ] Với provisioning mode: finalize sau APSTA, captive DNS, provisioning HTTPD và scan/config worker smoke test phù hợp.
- [ ] Áp dụng gate internal min 64 KiB, largest 32 KiB, PSRAM ready/min > 0 và allocation policy counters.
- [ ] Định nghĩa dwell window đủ để task startup/TLS handshake xuất hiện trước khi finalize.
- [ ] Fail rõ ràng khi task creation hoặc required PSRAM allocation thất bại.
- [ ] Không rollback chỉ vì peripheral/Wi-Fi bên ngoài tạm thời unavailable; phân biệt resource failure với network availability.
- [ ] Test OTA candidate tốt, candidate cố tình fail allocation, provisioning boot và STA boot.

### Acceptance

- [ ] Image không đạt memory gate bị rollback hoặc giữ pending theo policy đã ghi; không bị mark valid sớm.
- [ ] Image tốt được mark valid ở cả provisioning và STA path.
- [ ] Log/report chỉ rõ checkpoint và metric làm gate fail.
- [ ] Power-cycle sau finalize vẫn boot bình thường và không lặp rollback.

## RAM-06 — Hardware stress và qualification

**Branch dự kiến:** `ram/06-hardware-qualification`

### Scenario bắt buộc

- [ ] Boot/power-cycle 20 lần ở STA mode.
- [ ] Boot/power-cycle 10 lần ở provisioning mode.
- [ ] BLE 1/3/6/9 links: connect, security, discovery, READY, command ACK, disconnect và reconnect.
- [ ] `tools/list` 100 vòng với catalog 32 tools.
- [ ] `tools/call` gateway/device 100 vòng, gồm timeout, busy, rejection và late ACK.
- [ ] Dashboard WebSocket connect/disconnect/resync trong REST polling và BLE event burst.
- [ ] Xiaozhi WSS/TLS connect/reconnect 100 vòng nếu feature được bật trong release profile.
- [ ] NVS stress: device add/edit/delete, exposure enable/disable, Wi-Fi credential update.
- [ ] OTA write/validate trong khi các subsystem cho phép vẫn hoạt động.
- [ ] Combined worst-case: 9 BLE links + WS clients + REST/MCP + TLS reconnect + NVS activity.
- [ ] Soak production-like 24 giờ.

### Acceptance

- [ ] Toàn bộ release gates chung đạt.
- [ ] Không BLE READY/ACK regression, event loss ngoài bounded/drop policy hoặc socket leak.
- [ ] Heap integrity pass sau từng stress group và cuối soak.
- [ ] Report có biểu đồ/bảng baseline-vs-optimized cho internal, PSRAM, stack và latency.
- [ ] Mọi failure được tái hiện hoặc ghi rõ là môi trường ngoài phạm vi; không bỏ qua failure không giải thích được.

## RAM-07 — Tài liệu, defaults và release handoff

**Branch dự kiến:** `ram/07-docs-release-handoff`

### Công việc

- [ ] Tạo `docs/reports/INTERNAL_RAM_OPTIMIZATION_REPORT_IDF61.md` với before/after và raw evidence links.
- [ ] Cập nhật README memory documentation link; loại hoặc thay reference tới file không tồn tại.
- [ ] Cập nhật `sdkconfig.defaults`/Kconfig help theo policy cuối cùng; không chỉnh generated `sdkconfig` như source of truth.
- [ ] Ghi rõ allocation class cho từng table/buffer/task stack quan trọng.
- [ ] Ghi rõ component nào bắt buộc PSRAM và failure behavior khi allocation fail.
- [ ] Cập nhật API docs cho telemetry mới mà không làm vỡ field cũ.
- [ ] Clean build production và unit-test project bằng build directory mới.
- [ ] Chạy `git diff --check`, kiểm tra generated artifacts không bị commit nhầm.
- [ ] Xác nhận phase checklist của tất cả plan doc liên quan đã đồng bộ trước handoff.

### Acceptance

- [ ] Production build và test-app build pass.
- [ ] Firmware đã flash và runtime smoke pass trên đúng ELF SHA.
- [ ] Qualification report chứng minh toàn bộ gate RAM/stability/functionality đạt.
- [ ] Không gọi release-ready nếu thiếu 9-link, TLS, OTA/NVS hoặc soak evidence bắt buộc.

## 7. Thứ tự triển khai và rollback

`RAM-01 → RAM-02 → RAM-03 → RAM-04 → RAM-05 → RAM-06 → RAM-07`

- Mỗi phase có commit riêng, build/flash evidence riêng và có thể revert độc lập.
- Nếu RAM-02 làm tăng latency quá mức, giữ cJSON tree ở PSRAM nhưng A/B output buffer/policy riêng thay vì quay lại global internal allocation.
- Nếu RAM-03 phát hiện caller vượt quá ACK pool dự kiến, tăng pool theo measured concurrency; không quay lại gắn cứng với 16 devices.
- Nếu một external-stack experiment gây cache-disabled assertion, revert experiment ngay và giữ task đó internal; tiếp tục tối ưu large locals/pools.
- Nếu một tối ưu làm min free tốt hơn nhưng largest block xấu hơn, coi là regression fragmentation cho đến khi stress chứng minh ngược lại.

## 8. Bộ bằng chứng phải lưu cho mỗi phase

- Branch, commit SHA, `git status`, IDF version và Python environment.
- `sdkconfig` effective values liên quan nhưng thay đổi phải nằm trong defaults/Kconfig source.
- `idf.py build`, `idf.py size`, `idf.py size-components`, ELF/map SHA.
- Flash command/port và boot log có app SHA.
- `/api/status` snapshots trước/sau scenario.
- Task high-water, allocation counters, heap integrity và queue/drop metrics.
- Kết quả functional test và stress; ghi rõ phần nào chưa chạy.
