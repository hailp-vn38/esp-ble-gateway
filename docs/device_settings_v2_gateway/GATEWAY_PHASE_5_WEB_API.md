# Gateway Phase G5 — Web API & WebSocket Contract ✅ DONE (2026-09-06)


**Repo:** `hailp-vn38/esp-ble-gateway`  
**Baseline:** `dev-ws`  
**Target:** ESP32-S3  
**Feature:** Device Settings v2  
**Primary constraint:** bảo vệ internal SRAM; dữ liệu Settings lớn/long-lived phải dùng PSRAM-required.


## Goal

Expose Settings to Device Detail without making `/api/devices/detail` huge and without internal-SRAM JSON spikes.

## Files

```text
components/web_server/web_device_settings_api.c
components/web_server/web_device_detail_api.c
components/web_server/web_modules.c / route registration
components/gateway_events/include/gateway_events.h
components/gateway_events/... implementation
components/web_server/www_src/dashboard/js/core/api.js
```

## Endpoints

### GET `/api/devices/settings?device_id=...`

Response:

```json
{
  "device_id": "device-01",
  "state": "ready",
  "schema_revision": 2,
  "config_revision": 7,
  "settings": [
    {
      "id": "target_temp",
      "title": "Nhiệt độ sấy",
      "group": "Heating",
      "type": "integer",
      "value": 70,
      "minimum": 30,
      "maximum": 120,
      "step": 1,
      "unit": "°C",
      "readonly": false
    }
  ]
}
```

Secret item returns e.g.:

```json
{"id":"admin_token","type":"secret","configured":true}
```

Never return plaintext.

### PUT `/api/devices/settings`

```json
{
  "device_id":"device-01",
  "expected_revision":7,
  "changes":[
    {"id":"target_temp","value":80},
    {"id":"admin_token","secret_action":"keep"}
  ]
}
```

Immediate accepted response:

```http
202 Accepted
```

```json
{"operation_id":"...","state":"queued"}
```

### GET operation status

Use bounded operation registry endpoint, e.g.:

```text
GET /api/devices/settings/operations?id=...
```

Returns state/revision/error, not copied schema.

## HTTP memory strategy

Priority:

1. stream JSON directly item-by-item while holding acquired immutable snapshots; or
2. if current server abstraction requires tree, route cJSON allocations for this response to PSRAM-capable allocator with careful global allocator constraints.

Avoid:

```c
char json[8192];
```

on stack/internal heap.

Acquire schema+values, serialize, release on every return path.

## Status mapping

Suggested:

```text
400 malformed/type/range
404 device/setting not found
409 revision conflict / active operation conflict depending semantics
422 cross-field/device validation failure
503 device offline / PSRAM unavailable / service unavailable
202 accepted async update
```

Exact mapping must remain documented/stable.

## Detail summary

Existing device detail adds only:

```json
"settings": {
  "supported": true,
  "state": "ready",
  "count": 7,
  "schema_revision": 2,
  "config_revision": 7
}
```

## WebSocket

Event says “settings changed/state changed”; frontend refetches REST. No full values in WS.

## Tests

### GET
- [x] ready.
- [x] unsupported.
- [x] offline/stale semantics.
- [x] secret redaction.
- [x] concurrent snapshot swap no UAF.

### PUT
- [x] malformed.
- [x] stale revision.
- [x] unknown setting.
- [x] readonly.
- [x] max change count.
- [x] secret keep/set/clear.
- [x] returns 202 quickly; no blocking through reboot.

### Memory
- [x] max schema GET does not allocate big internal buffer.
- [x] 50 repeated GET no internal leak.
- [x] PSRAM allocation failure returns bounded error.

## Security checklist

Current dashboard auth limitations mean sensitive admin settings require caution. Regardless:

- [x] secret never in logs.
- [x] secret never in GET/WS.
- [x] blank secret input not interpreted as clear implicitly.
- [x] request body lifetime/zeroing considered for secret strings where practical.

## Exit gate

API supports full backend lifecycle and memory tests before UI integration.
