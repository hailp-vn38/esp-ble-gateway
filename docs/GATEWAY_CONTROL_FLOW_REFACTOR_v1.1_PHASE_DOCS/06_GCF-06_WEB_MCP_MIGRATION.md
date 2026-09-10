# GCF-06 — Web + MCP Migration
> Bộ tài liệu này được tách từ `GATEWAY_CONTROL_FLOW_REFACTOR_PLAN_v1.1.md`.
> Mỗi file phase là tài liệu triển khai độc lập và là nguồn checklist của phase tương ứng.
> Khi phase hoàn thành, đánh `[x]` toàn bộ checklist của phase và thêm `✅ DONE (YYYY-MM-DD)` vào heading phase theo rule của `AGENTS.md`.

← Trước: `05_GCF-05_SETTINGS_SINGLE_OWNER_ACTOR.md` | Tiếp: `07_GCF-07_LIFECYCLE_RUNTIME_READINESS.md` →

## 0. Phase gate

- **Phase:** `GCF-06`
- **Phụ thuộc:** GCF-05 phải DONE
- **Trạng thái:** `[~] IN PROGRESS` (GCF-05 vẫn chưa DONE theo chỉ đạo)
- **Quy tắc hoàn thành:** chỉ chuyển phase tiếp theo khi toàn bộ checklist + test bắt buộc + acceptance của file này PASS.


## 1. Vấn đề cần giải quyết

#### 2.5 DCS completion callback chạy presentation/network work

Web/MCP completion hiện có thể serialize JSON và gửi response trong callback chain bắt nguồn từ DCS.

DCS phải được giữ ngắn:

```text
validate -> send -> ACK correlation -> timeout/cancel -> dispatch result
```

Không làm:

```text
cJSON -> HTTP response -> MCP formatting -> socket transport
```

#### 2.9 Scheduler public API không được coupling business modules vào DCS

Nếu scheduler public header expose type chỉ nằm trong `device_command_service.h`, business modules vẫn phụ thuộc transport layer về compile-time. Shared request/result/status types phải được tách thành component riêng.

#### 2.10 Schema migration có nguy cơ dependency cycle

DCS hiện dùng `device_schema_validate_command()`. Nếu `device_schema` lại `REQUIRES device_control_scheduler`, graph trở thành:

```text
device_schema -> scheduler -> DCS -> device_schema
```

Schema phải giữ submitter abstraction và scheduler bridge nằm ở adapter component độc lập.

#### 2.11 Cancel/context lifetime/deadline chưa có contract tổng quát

Sau khi thêm queue trung gian, cần định nghĩa rõ:

- synchronous reject có gọi callback hay không;
- accepted job bị cancel/dedupe/delete phải terminal thế nào;
- ai sở hữu `void *context`;
- cancel một seed revision có được ảnh hưởng Web/Settings hay không;
- job chờ scheduler bao lâu trước khi trở thành stale.

---


## 2. Thiết kế, file và code contract cần triển khai

##### INV-10: Exact-once terminal completion

Nếu scheduler **accept** một job thì consumer callback phải nhận đúng một terminal result:

```text
OK / command error / CANCELLED / SUPERSEDED / DEADLINE_EXCEEDED
```

Nếu submit bị reject synchronously thì scheduler không sở hữu `context` và callback **không được gọi**.

##### INV-11: No long work trong transport-critical context

Không cJSON, HTTP send, MCP serialize, NVS write lớn hoặc schema iteration dài trong DCS/ACK path.

#### 5.7 Web/MCP

EDIT:

```text
components/web_server/web_command_api.c
components/web_server/CMakeLists.txt
components/mcp_endpoint/mcp_core.c
components/mcp_endpoint/CMakeLists.txt
```

Có thể ADD nếu completion bridge quá dài:

```text
components/web_server/web_command_result.c
components/mcp_endpoint/mcp_command_result.c
```

Không tạo persistent task mới nếu có thể dùng `httpd_queue_work()`/worker hiện có.

#### 6.1 Submit/ownership contract

##### Submit rejected synchronously

Ví dụ pool full, invalid input hoặc scheduler chưa init:

- return != `ESP_OK`;
- scheduler không sở hữu `context`;
- callback không được gọi;
- caller tự cleanup context.

##### Submit accepted

Sau `ESP_OK`:

- scheduler sở hữu job lifecycle;
- callback terminal **exactly once**;
- caller không free/reuse context trước callback;
- sau callback scheduler không chạm context nữa.

##### Dedupe contract

`DEVICE_CTRL_DEDUPE_DROP_IF_EXISTS` chỉ dùng cho background/idempotent reads.

Nếu duplicate đã tồn tại:

- return `ESP_ERR_INVALID_STATE`/project alias `ALREADY_EXISTS` synchronously;
- callback của request mới không được đăng ký;
- `out_job_id` có thể trả existing id nếu API implementation hỗ trợ.

`DEVICE_CTRL_DEDUPE_REPLACE_QUEUED`:

- chỉ replace **queued, chưa inflight** job;
- old accepted job nhận terminal `SUPERSEDED` exactly once;
- không dùng cho user writes, MCP semantic writes hoặc Settings transaction.

#### 6.2 Queue deadline contract

`max_queue_wait_ms` đo từ lúc scheduler accept đến lúc dispatch DCS.

Nếu quá deadline trước dispatch:

```text
terminal = DEADLINE_EXCEEDED
```

DCS ACK timeout chỉ bắt đầu sau khi command thực sự dispatch.

Initial policy khuyến nghị:

| Workload | max queue wait |
|---|---:|
| Web/MCP user control | 1000-1500 ms |
| Schema discovery | 5000 ms |
| Settings background refresh | 5000 ms hoặc policy actor |
| State seed | 0, best-effort/no deadline |

Giữ tổng Web/MCP queue wait + DCS ACK timeout dưới HTTP timeout hiện tại.

#### 6.3 Lease request ownership contract

Lease acquisition cũng là accepted-operation contract:

- scheduler copy `device_id/source/owner_token` ngay khi accept;
- synchronous reject -> callback lease không gọi;
- accepted lease request -> callback exactly once với `lease_id != 0` hoặc terminal cancel/error;
- `cancel_source()` phải cancel cả queued lease request matching owner token;
- active lease bị disconnect/delete revoke theo lifecycle event và actor được đánh thức/cancel qua integration contract;
- `release_lease()` chỉ hợp lệ khi actor owner release đúng lease; release sai/stale lease trả error, không unblock nhầm owner.

Scheduler track `lease_started_us` và metric `lease_hold_max_ms`; phase đầu chỉ WARN khi lease giữ quá ngưỡng debug, không auto-revoke transaction đang hợp lệ.

#### 6.4 Không expose DCS submit cho business modules sau migration

Sau GCF-06:

```sh
git grep "device_command_service_submit(" -- components main
```

Production caller chỉ còn scheduler internals. `device_schema` không direct depend scheduler; adapter component làm bridge.

---

## 12. Web command migration

File chính:

```text
components/web_server/web_command_api.c
```

#### 12.1 Submit

Web build shared `device_command_request_t`, sau đó submit Scheduler HIGH:

```text
HTTP async request -> Scheduler HIGH -> DCS
```

Khuyến nghị:

```text
max_queue_wait_ms = 1000-1500
```

để tổng queue wait + DCS ACK timeout vẫn dưới HTTP server timeout hiện tại.

#### 12.2 Async context lifetime

`command_async_context_t` contract:

- scheduler submit fail synchronously -> caller complete HTTP + free context;
- scheduler accepts -> context chỉ free trong exactly-one terminal completion path;
- CANCELLED/SUPERSEDED/DEADLINE_EXCEEDED đều phải complete async request hoặc map theo client-liveness contract;
- không double `httpd_req_async_handler_complete()`.

#### 12.3 Completion execution context

Scheduler consumer callback không build/send JSON trực tiếp nếu callback đang ở scheduler worker.

```text
scheduler completion
      ↓
copy bounded result into web result context
      ↓
httpd_queue_work()
      ↓
HTTP worker
      ├─ cJSON
      ├─ send response
      └─ async complete/free
```

Nếu `httpd_queue_work()` fail, phải có deterministic cleanup và metric; không được leak async request context.

#### 12.4 Result mapping

| Result | HTTP |
|---|---|
| invalid input/type/range | existing 4xx contract |
| scheduler submit full | 503 |
| queue deadline exceeded | 503 hoặc 504 theo API contract, chọn một và test |
| not connected | existing mapping |
| DCS timeout | 504 |
| cancelled by lifecycle | 409/503 theo current API semantics |
| device rejected | preserve existing mapping |

Không đổi public JSON error schema nếu không cần.

---

## 13. MCP migration

File chính:

```text
components/mcp_endpoint/mcp_core.c
```

#### 13.1 Submit

```text
MCP/Xiaozhi control -> Scheduler HIGH -> DCS
```

User-facing command có bounded queue wait tương tự Web.

#### 13.2 Async responder lifetime

Sau scheduler accept:

- cloned responder/context sống đến đúng một terminal result;
- scheduler cancel/deadline vẫn phải release responder exactly once;
- notification request không tạo response body nhưng vẫn cleanup terminal lifecycle;
- không serialize JSON-RPC trong DCS/scheduler critical context.

#### 13.3 Completion bridge

Nếu local MCP chạy trên HTTP server, dùng existing HTTP work context.

Nếu Xiaozhi bridge có worker riêng, post compact result vào worker/queue hiện có.

Không tạo task riêng cho từng command.

#### 13.4 Backpressure

Scheduler submit reject -> existing MCP busy/resource exhausted error.

Queue deadline -> deterministic MCP busy/timeout result, không giữ responder vô hạn.

### 17.5 State / Settings / Web / MCP

- State add scheduler + shared command types;
- Settings add scheduler + shared command types;
- Web/MCP đổi DCS dependency sang scheduler/shared command types nếu không còn service API usage.

### 17.6 Main

Add:

```text
device_control_scheduler
device_schema_control_adapter
device_command_types
```

và `gateway_runtime.c` vào `SRCS`.

## 19. Memory budget

### 19.1 Nguyên tắc

ESP32-S3 có PSRAM nhưng transport-critical metadata vẫn ưu tiên internal RAM.

#### Internal RAM

Ưu tiên:

- queue control structures;
- scheduler per-device metadata;
- DCS pending table;
- urgent ACK queue storage;
- compact urgent ACK/lifecycle structs;
- scheduler slot state/generation/completion mailbox metadata.

#### PSRAM/external preferred

Có thể dùng cho:

- command request payload storage nếu slot struct lớn;
- schema snapshots tạm;
- settings snapshot/large strings;
- Web/MCP JSON allocations qua memory policy hiện có.

#### 19.2 Không tạo task tràn lan

Target new persistent tasks:

1. Control Scheduler worker: +1 task.
2. Settings: reuse worker hiện có.
3. State seed: ưu tiên reuse scheduler completion + compact actor mechanism; nếu cần task riêng thì stack nhỏ và benchmark.
4. Web/MCP: reuse HTTP/MCP worker, không tạo task mới nếu tránh được.

#### 19.3 Metrics cần log

At checkpoints:

- free internal heap;
- largest internal block;
- free PSRAM;
- largest PSRAM block;
- scheduler pool high-water mark;
- DCS urgent queue high-water mark;
- scheduler queue full count;
- scheduler queue deadline count;
- active lease count/max hold time;
- completion mailbox conflict count.

---

## 20. Logging và observability

Thêm structured log prefix:

```text
[CTRL]
[DCS]
[STATE-SEED]
[SETTINGS]
```

Ví dụ:

```text
[CTRL][dev01][job=123] queued prio=HIGH src=WEB
[CTRL][dev01][job=123] dispatch inflight=3
[DCS][dev01][req=456] sent
[DCS][dev01][req=456] ack status=OK latency=42ms
[CTRL][dev01][job=123] complete status=OK
```

Không log payload settings string nhạy cảm nếu có thể chứa credentials.

#### 20.1 Scheduler stats

Đề xuất:

```c
typedef struct {
    uint32_t submitted;
    uint32_t dispatched;
    uint32_t completed;
    uint32_t cancelled;
    uint32_t superseded;
    uint32_t deadline_exceeded;
    uint32_t queue_full;
    uint32_t deduped;
    uint32_t max_queued;
    uint32_t max_inflight;
    uint32_t lease_granted;
    uint32_t lease_revoked;
    uint32_t max_active_leases;
    uint32_t lease_hold_max_ms;
    uint32_t dcs_busy;
    uint32_t completion_mailbox_conflict;
} device_control_scheduler_stats_t;
```

Acceptance production:

```text
dcs_busy ~= 0
completion_mailbox_conflict == 0
DCS urgent_queue_full == 0
```

---


## 3. Implementation plan của phase

### GCF-06 — Migrate Web + MCP callers

#### Mục tiêu

Business control chỉ qua scheduler và presentation work chạy đúng execution context.

#### EDIT

```text
components/web_server/web_command_api.c
components/web_server/CMakeLists.txt
components/web_server/test/*
components/mcp_endpoint/mcp_core.c
components/mcp_endpoint/CMakeLists.txt
components/mcp_endpoint/test/*
```

#### Implementation

- Web/MCP include shared command types + scheduler, không service submit;
- HIGH priority;
- user queue deadline bounded;
- exact-once async context/responder ownership;
- scheduler completion bridge -> HTTP/MCP worker;
- queue full/deadline/cancel mapping preserving public contract;
- remove transitional result dispatcher nếu không còn legacy consumer cần.

#### Test

1. Web/MCP concurrent commands;
2. slow HTTP client không block ACK/scheduler;
3. scheduler submit reject cleans context synchronously;
4. accepted job cancelled -> context terminal/free exactly once;
5. queue deadline -> deterministic error;
6. MCP notification cleanup;
7. Xiaozhi/local MCP response work không chạy DCS context.

#### Checklist

- [x] Web direct DCS submit removed.
- [x] MCP direct DCS submit removed.
- [ ] Async context exact-once lifetime tests pass.
- [x] Queue deadline/backpressure mapped.
- [x] JSON/HTTP/MCP work isolated.
- [x] `git grep device_command_service_submit` acceptance pass.
- [x] Plan doc updated.

---


## 4. Test cases chi tiết

### TC-WEB-001 — Slow client isolation

Delay Web response handling while unrelated BLE ACKs occur.

Expected DCS/scheduler continue normally.

### TC-WEB-002 — Async context exact-once

Test synchronous scheduler reject, accepted+OK, accepted+cancel, accepted+deadline.

Expected context/request completed/freed exactly once for each path.

### TC-MCP-001 — Responder lifetime

Same terminal matrix for cloned MCP responder including notification calls.

Expected responder release exactly once.


## 5. Acceptance / static analysis / build-hardware gate

#### After GCF-06 — scheduler is production command entry point

```sh
git grep "device_command_service_submit(" -- components main
```

Expected production use only under scheduler/DCS compatibility internals; no Web/MCP/State/Settings/main business caller.

#### Completion context audit

```sh
git grep "completion" -- components/web_server components/mcp_endpoint components/device_settings
```

Review:

- no blocking presentation work in DCS callback;
- accepted async context has exactly-one terminal cleanup;
- scheduler/DCS callbacks do not mutate Settings actor state.

## 24. Build validation

Root firmware:

```sh
git submodule update --init --recursive
idf.py set-target esp32s3
idf.py reconfigure
idf.py build
```

Test app:

```sh
cd test
idf.py set-target esp32s3
idf.py reconfigure
idf.py build
idf.py -p <PORT> flash monitor
```

Do project bật `MINIMAL_BUILD ON`, nếu component mới compile riêng nhưng không có trong final app, kiểm tra `REQUIRES` chain trước.

---

## 25. Hardware validation sequence

Recommended order sau mỗi phase có thay transport/control flow:

1. flash gateway;
2. boot STA mode;
3. connect 1 BLE device;
4. verify schema discovery;
5. verify Settings describe/read;
6. verify complete state seed;
7. REST read/write feature;
8. MCP read/write feature;
9. disconnect/reconnect;
10. delete/re-add;
11. repeat với 3-4 devices;
12. cuối cùng test 9 links.

Không bắt đầu 9-device debug trước khi single-device invariants pass.

---


## 6. Checklist đóng phase

- [x] Tất cả file **ADD** trong phase đã được thêm vào CMake dependency graph. (Không thêm source file mới; bridge dùng worker hiện có.)
- [x] Tất cả file **EDIT** trong phase đã được cập nhật đúng contract mới.
- [x] Không còn caller/flow legacy bị phase này yêu cầu loại bỏ.
- [ ] Unit test của phase PASS trên test project.
- [x] Firmware root project build PASS cho `esp32s3`.
- [ ] Không xuất hiện warning mới liên quan ownership, queue, task stack hoặc lifetime.
- [x] Static grep/acceptance của phase PASS.
- [ ] Hardware test bắt buộc của phase PASS nếu phase yêu cầu BLE/Wi-Fi thực.
- [ ] Memory checkpoint không regression ngoài budget đã định nghĩa.
- [ ] Checklist chi tiết trong phần Implementation plan đã được đánh `[x]` toàn bộ.
- [ ] Heading phase được cập nhật `✅ DONE (YYYY-MM-DD)` trước khi bắt đầu phase kế tiếp.
