# Gateway Phase G1 — Settings Core, PSRAM Memory Model & Snapshot Lifetime


**Repo:** `hailp-vn38/esp-ble-gateway`  
**Baseline:** `dev-ws`  
**Target:** ESP32-S3  
**Feature:** Device Settings v2  
**Primary constraint:** bảo vệ internal SRAM; dữ liệu Settings lớn/long-lived phải dùng PSRAM-required.


## Goal

Tạo `device_settings` gateway component và memory ownership model trước BLE discovery.

## Add files

```text
components/device_settings/CMakeLists.txt
components/device_settings/include/device_settings.h
components/device_settings/device_settings.c
components/device_settings/device_settings_snapshot.c
components/device_settings/device_settings_memory.c
components/device_settings/device_settings_protocol.c
components/device_settings/device_settings_operation.c
```

## Data model

Tách schema/value:

```c
typedef struct device_settings_schema device_settings_schema_t;
typedef struct device_settings_values device_settings_values_t;
```

Schema contains:

```text
schema_revision
setting_count
compact descriptors
string pool
enum option pool
```

Values contains:

```text
config_revision
value_count
scalar values
string value pool
secret configured bits
```

Không rebuild schema mỗi value update.

## Compact descriptor

Không dùng fixed 32/48-byte string arrays per setting. Dùng offsets vào pool:

```c
typedef struct {
    uint16_t id_off;
    uint16_t title_off;
    uint16_t group_off;
    uint16_t unit_off;
    int32_t min_value;
    int32_t max_value;
    int32_t step;
    uint16_t flags;
    uint8_t type;
    uint8_t option_count;
    uint16_t option_index;
} device_setting_desc_compact_t;
```

## Allocator classes

Provide explicit wrappers:

```c
void *settings_alloc_external_required(size_t n);
void settings_free(void *p);
```

Long-lived schema/values/pools MUST use external-required. Không dùng `EXTERNAL_PREFERRED` cho các object này.

## Immutable snapshot lifetime

Need:

```c
const device_settings_schema_t *device_settings_schema_acquire(...);
void device_settings_schema_release(...);
const device_settings_values_t *device_settings_values_acquire(...);
void device_settings_values_release(...);
```

Discovery builds staging privately, validates, then atomic swap committed pointer. Old snapshot free only after reader refs drop to zero (refcount/deferred free/RCU-like implementation).

### Race to prevent

```text
HTTP GET acquires old snapshot
BLE discovery commits new snapshot
BLE thread must NOT free old snapshot yet
HTTP finishes -> release -> old snapshot free
```

## Per-device control record

Keep internal-resident record compact:

```text
device identity/index handle
schema pointer
values pointer
state
generation/ref bookkeeping minimal
active operation pointer/id
```

Do not store fixed `settings[MAX]` here.

## Tests

### Allocation
- [ ] external-required success places object in PSRAM-capable memory.
- [ ] forced PSRAM allocation failure returns error.
- [ ] no fallback to internal heap.

### Builder
- [ ] compact strings deduplicated optionally or at least contiguous.
- [ ] offset overflow detected.
- [ ] max schema rejects safely if size representation exceeded.

### Lifetime/concurrency
- [ ] acquire/release normal.
- [ ] swap with one reader.
- [ ] swap with many readers.
- [ ] deferred free only after last release.
- [ ] failed staging leaves committed untouched.

### Footprint
- [ ] persistent internal control metadata measured <= target.
- [ ] `sizeof` queue/pending structs static_assert/CI check where practical.

## Checklist

- [ ] No fixed-size strings per setting in internal record.
- [ ] Schema/value separate.
- [ ] Long-lived allocations external-required.
- [ ] No use-after-free window.
- [ ] Failed PSRAM allocation changes only Settings state/error.
- [ ] Wi-Fi/BLE core remains alive.

## Exit gate

Component passes forced-allocation-failure and concurrent snapshot swap tests before BLE discovery integration.
