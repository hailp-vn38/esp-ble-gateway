# Device Detail Page Baseline — 2026-09-04

## Scope

- HEAD: `2260d5f814e11cc4f234a9034ad845266d95446e`
- Branch: `dev-ws`
- Baseline captured before production changes for Device Detail plan v2.0.

## Evidence

The current frontend requests the device inventory, schema, exposure policy, and
event WebSocket independently. `mcp_exposure.js` reads `data.commands` and
sends `command`, while the current exposure backend serializes `features[]` and
accepts `feature_id`. This contract mismatch makes the MCP section empty even
when feature policy data exists.

The current source locations are:

- `www_src/dashboard/js/features/mcp_exposure.js`: legacy `commands[]`,
  `tool_name`, capacity, and `command` mutation handling.
- `www_src/dashboard/views/device_detail.html`: `MCP Tools` heading and
  dynamic capacity element.
- `web_exposure_api.c`: feature-oriented exposure response and mutation.

## Runtime limitation

No browser session or gateway URL/device was available in this run, so a live
Network request count, exposure JSON capture, console log capture, and visual
reproduction could not be performed. These remain explicit unverified runtime
items; no production fix was made before this baseline file.

## Expected current API contract

`GET /api/mcp/exposures?device_id=X` is expected to return HTTP 200 with
`features[]` and `policy_revision`, without `commands[]`. The feature-based PUT
contract is:

```json
{"device_id":"X","feature_id":"relay_1","enabled":false}
```

