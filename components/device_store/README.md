# Device Store

Registry cấu hình persistent của các thiết bị BLE (NVS + RAM cache). Đây là **nền tảng lưu trữ duy nhất** của gateway: mọi consumer đọc/ghi qua copy-out API có lock, không ai được giữ pointer vào cache nội bộ.

> Spec chi tiết: `device_store_refactor_issues_and_solutions_v2.md` trong thư mục này. README tóm tắt contract hiện hành sau refactor v2.

## Trách nhiệm

```text
Device Store QUẢN LÝ                    Device Store KHÔNG quản lý
------------------------------------    ------------------------------------
device_id (logical identifier)          connected state
name, device type                       connection handle / GATT handles
canonical BLE identity (addr + type)    MTU, retry/backoff
has_ble_identity                        BLE scheduler / connection slot
schema version metadata
```

Runtime connection state thuộc về `ble_central`. Web/MCP cần trạng thái online thì merge hai snapshot ở tầng presentation:

```text
device_store_snapshot()  +  ble_central_get_device_status()  ->  API response
```

## Public API (`include/device_store.h`)

| API | Contract chính |
|---|---|
| `device_store_init()` | **Single-shot**. Gọi lần hai → `DEVICE_STORE_ERR_INVALID_STATE` |
| `device_store_add(id, name, type)` | Validate độ dài; reject trùng id; `ERR_FULL` khi đủ 16 thiết bị. Không suy luận BLE identity từ `device_id` |
| `device_store_delete(id)` | Xóa + compact NVS; `ERR_NOT_FOUND` nếu không tồn tại |
| `device_store_edit(id, name, type)` | NULL = giữ nguyên; cả hai NULL → `ERR_INVALID_ARG` |
| `device_store_get(id, out)` | Copy-out một entry |
| `device_store_snapshot(out, cap, out_count)` | Copy-out toàn bộ. **Không silent truncate** (bên dưới) |
| `device_store_set_ble_identity(id, addr, type)` | Setter transport-identity duy nhất; enforce uniqueness canonical `(type, addr)` |

### `snapshot()` contract

```text
capacity >= count            -> OK, copy tất cả, *out_count = count
capacity <  count            -> ERR_BUFFER_TOO_SMALL, *out_count = count cần
                                (không trả partial list)
out_entries=NULL, capacity=0 -> query mode: *out_count = count
```

### BLE identity

- `device_id` là **logical identifier** — kể cả khi nhìn giống MAC (`AA:BB:CC:...`) cũng **không** tự sinh BLE address.
- Identity chỉ được set qua `set_ble_identity()` với `addr_type` explicit (hợp lệ `0..3`, xem `DEVICE_STORE_BLE_ADDR_TYPE_MAX`).
- Ghi trùng canonical identity của device khác → `ERR_DUPLICATE_BLE_IDENTITY`. Set cùng giá trị cũ → OK (idempotent).
- Canonical identity là địa chỉ resolved từ kết nối (`peer_id_addr`), không phải advertising RPA.

## Typed error contract

`OK` là giá trị thành công duy nhất:

| Mã lỗi | Ý nghĩa | Dispatcher | HTTP |
|---|---|---|---|
| `INVALID_ARG` | Tham số sai/thiếu | `INVALID_ARGUMENT` | 400 |
| `NOT_FOUND` | Không có device này | `NOT_FOUND` | 404 |
| `DUPLICATE_ID` | Trùng logical id | `CONFLICT` | 409 |
| `DUPLICATE_BLE_IDENTITY` | Addr đã thuộc device khác | `CONFLICT` | 409 |
| `FULL` | Đủ `DEVICE_STORE_MAX_DEVICES` | `RESOURCE_EXHAUSTED` | 507 |
| `BUSY` | Không lấy được store mutex | `BUSY` | 409 |
| `PERSISTENCE` | NVS I/O fail | `INTERNAL_ERROR` | 500 |
| `CORRUPT` | Record hỏng/sai kiểu/sai độ dài | `INTERNAL_ERROR` | 500 |
| `SCHEMA_TOO_NEW` | Dữ liệu của firmware mới hơn | `INTERNAL_ERROR` | 500 |
| `BUFFER_TOO_SMALL` | Snapshot buffer thiếu | internal contract error | 500 |
| `CAPACITY_EXCEEDED` | NVS chứa nhiều hơn capacity firmware | `RESOURCE_EXHAUSTED` | 507 |
| `INVALID_STATE` | Chưa init / init lặp | `INTERNAL_ERROR` | 500 |

## NVS layout & schema

Namespace `"dev_list"`, mỗi device là nhiều key theo index:

```text
count        u8   số record hợp lệ (authoritative cho multi-key layout)
schema_ver   u8   hiện tại = 2
id_%d        str  (required)
name_%d      str  (required ở v2)
type_%d      str  (required ở v2)
addr_%d      blob 6 byte, NimBLE byte order (LSB first)
atype_%d     u8
```

Luật load (`device_store_migration.c`):

1. `stored_schema > DEVICE_STORE_SCHEMA_VERSION` → `SCHEMA_TOO_NEW`, **không ghi gì lên NVS** (an toàn OTA rollback).
2. Loader chọn theo schema: `load_entry_v1` (legacy: name/type fallback, MAC-shaped id → BLE identity) hoặc `load_entry_v2` (strict, không fallback).
3. Record corrupt bị skip và store được compact rewrite; lỗi NVS thật (I/O) thì abort load — không bao giờ collapse thành "store rỗng".
4. `raw_count > capacity` firmware → `CAPACITY_EXCEEDED`, không destructive repair.
5. Multi-key mutation luôn kết thúc bằng **một** `nvs_commit()` — crash giữa chừng không để lại record nửa vời.

## Concurrency

- Toàn bộ API đi qua một FreeRTOS mutex (timeout 2s → `ERR_BUSY`).
- Không API nào trả pointer vào cache; mọi read là copy dưới lock.
- NimBLE host callback **không bao giờ** gọi vào store trực tiếp. BLE identity được persist bất đồng bộ bởi worker `ble_central_identity.c`: callback chỉ submit vào pending-slot trong critical section (bounded, non-blocking), worker gọi `set_ble_identity()` với dirty/retry/backoff và coalescing *latest-wins*. Failure được đếm ở metric `identity_persist_failures`.

## Cấu trúc module

```text
include/device_store.h        Public API + typed result
device_store.c                Cache, locking, CRUD, snapshot
device_store_entry.c          Validation, parse addr (cho migration)
device_store_nvs.c            NVS backend, error classification
device_store_migration.c      Per-schema loaders (v1 legacy / v2 strict)
```

## Test

Unity test chạy trên hardware thật:

```sh
cd test
idf.py set-target esp32s3   # lần đầu
idf.py build
idf.py -p <PORT> flash monitor
```

Component nằm trong `TEST_COMPONENTS` của `test/CMakeLists.txt`. Các case chính: validation, capacity full, snapshot contract (`BUFFER_TOO_SMALL` + query mode), identity persistence qua re-init, duplicate canonical identity, compaction sau delete, MAC-shaped id **không** sinh identity, single-shot lifecycle.

Persistence-across-reload dùng hook nội bộ `device_store_reset_for_test()` (chỉ có trong `device_store_internal.h`, không part của public contract).

## Lịch sử refactor v2 (so với API cũ)

Đã xóa:

- `device_store_list()` / `device_store_find()` — trả pointer thẳng vào cache (race/stale pointer).
- `device_store_set_connected()` + trường `connected` — runtime state bị duplicate nguồn sự thật; thay bằng `ble_central_get_device_status()`.
- MAC inference trong create path và trong generic load path (chỉ còn ở loader v1).
- int return `0/-1` → thay bằng `device_store_result_t`.
