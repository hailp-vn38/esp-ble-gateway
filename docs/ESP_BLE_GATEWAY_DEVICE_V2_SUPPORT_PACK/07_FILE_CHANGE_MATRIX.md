# Gateway v2 — File Change Matrix

## 1. P0 — Must change before new Device rollout

| File | Priority | Required change |
|---|---:|---|
| `components/cbor_codec/include/cbor_codec.h` | P0 | GENERIC_VALUE, VALUE, decimals |
| `components/cbor_codec/cbor_codec.c` | P0 | key31 encode/decode |
| `components/device_schema/include/device_schema.h` | P0 | feature metadata v2 |
| `components/device_schema/device_schema_protocol.c` | P0 | stage title/unit/value_type/decimals |
| `components/device_schema/device_schema_validate.c` | P0 | binding + semantic validation |
| `components/device_template/device_template.c` | P0 | fix FAN/DIMMER, add GENERIC_VALUE/VALUE |
| `components/device_schema/device_schema_store.c` | P0 | NVS schema version 2 |
| `components/device_state/device_state.c` | P0 | seed INT + BOOL |

---

## 2. P1 — Needed for Web UI support

| File | Priority | Required change |
|---|---:|---|
| `components/web_server/web_device_schema_api.c` | P1 | expose metadata |
| `components/web_server/web_device_detail_api.c` | P1 | metadata + control |
| `components/web_server/www_src/dashboard/js/features/devices.js` | P1 | title + decimals + generic numeric renderer |
| Web i18n resources | P1 | labels if needed |

---

## 3. P1 — Needed for MCP/Xiaozhi

| File | Priority | Required change |
|---|---:|---|
| `components/mcp_endpoint/mcp_semantic_control.c` | P1 | serialize v2 metadata |
| `components/mcp_endpoint/mcp_device_control.c` | P1 | generic read/set/result |
| `components/mcp_endpoint/mcp_registry.c` | P2 | optional number_value schema |
| `components/mcp_endpoint/mcp_endpoint_internal.h` | P2 | only if structures change |

---

## 4. Test files

| Area | Tests |
|---|---|
| CBOR | old/new roundtrip |
| Schema | 7/10 + 12/12 |
| Template | all semantic pairs |
| NVS | v1 invalidation, v2 reboot |
| State | BOOL + INT seed |
| Web API | metadata contract |
| UI | raw/display conversion |
| MCP | describe/read/set |
| E2E | old/new mixed devices |

---

## 5. Files not expected to need semantic redesign

Normally unchanged unless tests expose issue:

```text
BLE Central GATT transport
BLE UUIDs
device_store identity model
WebSocket event envelope
MCP auth
MCP policy
Wi-Fi
OTA
```

WebSocket already carries typed feature state; v2 metadata remains in schema/API cache.

---

## 6. Important invariants

```text
feature_id = identity
title      = display only

tool       = min/max/step authority
feature    = semantic metadata
state      = runtime value

BLE numeric = raw int
UI numeric  = human scale

Gateway rollout before Device v2
```
