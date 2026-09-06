# Phase 5 — Integration, Regression, Rollout

## 1. Tổng quan

Phase này xác nhận Gateway hỗ trợ cả:

```text
old reference device
new demo device v2
```

Không thêm semantic feature mới ở phase này.

---

## 2. Cần thêm / sửa gì

- integration tests;
- schema migration tests;
- API contract tests;
- WebSocket tests;
- MCP tests;
- memory/PSRAM measurement;
- reboot/reconnect tests;
- release checklist.

---

## 3. Sửa ở đâu

| Area | Path |
|---|---|
| codec tests | `components/cbor_codec/test/*` |
| schema tests | `components/device_schema/test/*` |
| template tests | `components/device_template/test/*` |
| state tests | device_state test area |
| web tests | `components/web_server/test/*` |
| MCP tests | MCP component tests |
| top-level host tests | `test/*` |
| docs | final protocol/schema docs |

---

## 4. Integration matrix

| Case | Expected |
|---|---|
| old reference device | unchanged |
| new demo discovery | 7 tools / 10 features |
| generic setpoint | supported |
| temp INT read | supported |
| fan PERCENT_SETTING | supported |
| dimmer LEVEL | supported |
| local event | realtime |
| Gateway reboot | schema v2 loads |
| Device reconnect | state reseeded |
| MCP set | correct tool |
| UI set | correct raw scaling |

---

## 5. Persistence tests

### Old Gateway data

Start with schema v1 NVS.

Flash Gateway v2.

Expected:

```text
v1 schema invalidated
device reconnect/discovery
v2 schema persisted
```

No crash.

### New data reboot

Persist v2 -> reboot.

Expected:

- titles retained;
- units retained;
- decimals retained;
- mappings retained.

---

## 6. Memory tests

Track:

```text
internal free
internal min free
largest internal block
PSRAM free
PSRAM min free
```

Scenarios:

- boot;
- 1 device;
- schema discovery;
- Web UI open;
- WebSocket stream;
- MCP describe;
- repeated refresh.

Acceptance:

- no growing leak;
- schema metadata growth bounded;
- no unexpected internal-RAM spike.

---

## 7. Stress tests

### S5.1 — Schema refresh x100

Expected no leak/deadlock.

### S5.2 — Reconnect x100

Expected schema/state recover.

### S5.3 — Sensor stream 60 minutes

Expected WS/UI stable.

### S5.4 — UI + MCP concurrent

Expected final state authoritative.

### S5.5 — Gateway reboot loop

Expected persisted schema stable.

### S5.6 — Old/new device mixed

Both connected.

Expected no semantic collision.

---

## 8. Failure injection

- malformed feature item;
- unknown feature type;
- wrong property;
- duplicate feature ID;
- missing write tool;
- type mismatch;
- unit mismatch;
- invalid decimals;
- corrupt NVS;
- disconnect during discovery;
- state read timeout;
- WS reconnect;
- MCP ambiguous feature.

---

## 9. Checklist

### Protocol

- [ ] old packets.
- [ ] v2 packets.
- [ ] decimals.

### Schema

- [ ] 7/10 demo.
- [ ] max 12/12.
- [ ] migration.
- [ ] template validation.

### State

- [ ] BOOL seed.
- [ ] INT seed.
- [ ] ACK.
- [ ] event.

### Web

- [ ] API.
- [ ] UI.
- [ ] WS.
- [ ] fallback.

### MCP

- [ ] list.
- [ ] describe.
- [ ] read.
- [ ] set.
- [ ] Xiaozhi.

### Regression

- [ ] reference device.
- [ ] existing Web UI behavior.
- [ ] existing MCP auth/policy.
- [ ] existing device storage.

---

## 10. Suggested commit sequence

```text
1 feat(codec): add generic value and decimals extension
2 refactor(schema): store v2 feature metadata
3 fix(template): align fan dimmer and generic value
4 feat(schema-store): migrate persisted schema to v2
5 feat(state): seed int and bool feature state
6 test(schema): cover v2 semantic validation

7 feat(web-api): expose v2 feature metadata
8 feat(web-ui): render title and scaled generic numeric values
9 test(web): cover generic feature detail

10 feat(mcp): expose generic semantic controls
11 feat(mcp): return typed actual state metadata
12 test(mcp): generic describe read set

13 test(e2e): old and new device compatibility
```

---

## 11. Rollout checklist

- [ ] merge Gateway support first.
- [ ] flash Gateway.
- [ ] validate old device.
- [ ] clear/refresh old schema cache if needed.
- [ ] flash demo device v2.
- [ ] verify schema ready.
- [ ] verify initial state.
- [ ] verify UI.
- [ ] verify MCP.
- [ ] monitor heap.
- [ ] run soak.

---

## 12. Exit criteria

- [ ] old device regression pass.
- [ ] new device full support.
- [ ] no NVS migration issue.
- [ ] no UI raw-scaling issue.
- [ ] no MCP semantic ambiguity for exact IDs.
- [ ] memory stable.
