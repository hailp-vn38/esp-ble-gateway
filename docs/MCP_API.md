# ESP32 BLE Gateway — MCP API Documentation

**MCP Protocol Version:** `2026-07-28`
**Endpoint:** `POST /mcp`
**Admin API:** `GET/PUT /api/mcp/exposures`
**Transport:** Streamable HTTP (JSON responses only, no SSE)

---

## 1. Overview

The ESP32 BLE Gateway exposes a minimal MCP Tools Server endpoint that allows
AI agents, voice hosts, and MCP clients to discover and control BLE devices
over a local network.

```text
AI / Voice / MCP Client
        |
        | POST /mcp (JSON-RPC 2.0)
        v
+------------------------------+
|        mcp_endpoint          |
|------------------------------|
| HTTP validation              |
| MCP routing headers          |
| required request _meta       |
| JSON-RPC validation          |
| server/discover              |
| tools/list (static + dynamic)|
| tools/call  (static + dynamic)|
| MCP policy                   |
+--------------+---------------+
               |
               v
       command_executor
               |
               v
      command_dispatcher
               |
               v
          BLE Central
               |
               v
          BLE Device
```

**Key constraints:**
- No session/handshake (`initialize` not required)
- No SSE streaming (JSON responses only)
- No resources, prompts, sampling, or roots
- LAN-only (plaintext HTTP, never expose to Internet)

### 1.1 Dynamic Tool Exposure

Each registered BLE device can expose its commands as **individual MCP tools**,
allowing LLM clients to call `set_speed` directly instead of
`device_command(device_id="fan_01", command="set_speed")`.

```text
Admin Dashboard (GET/PUT /api/mcp/exposures)
        |
        | enable/disable per command
        v
mcp_tool_exposure (NVS + catalog)
        |
        | tools/list merges static + dynamic
        v
tools/call resolves dynamic tool → builds gw_message_t directly
```

Tool names are deterministic (device_id + command), NVS-persisted, and
reconciled on boot. Capabilities are protected by a 128-bit semantic digest
(SHA-256 truncated) — if a device's capabilities change, exposed tools enter
`needs_review` state.

---

## 2. Authentication & Security

### 2.1 Bearer Token (MCP)

```text
Authorization: Bearer <token>
```

- Token configured via `CONFIG_MCP_AUTH_TOKEN` or NVS key `mcp.token`
- Empty token = dev mode (no auth, warning logged)
- Constant-time comparison, never logged
- Missing/invalid → HTTP 401 Unauthorized

### 2.2 Bearer Token (Admin API)

```text
Authorization: Bearer <admin_token>
```

- Token configured via `CONFIG_WEB_ADMIN_AUTH_TOKEN` or NVS key `web_admin.token`
- Separate from MCP token — fail closed if not configured
- Required for all `/api/mcp/exposures` requests

### 2.3 Host/Origin Validation

```text
Host: gateway.local
```

- Must match `CONFIG_MCP_HOST_ALLOWLIST` (comma-separated)
- Entries: `gateway.local,192.168.4.1` (default)
- Case-insensitive, port stripped, IPv6 brackets preserved
- Mismatch → HTTP 403 Forbidden (DNS rebinding protection)

### 2.4 Rate Limiting

```text
Token bucket: 10 req/s, burst capacity 10
```

- Exceeds → HTTP 429 Too Many Requests
- Configurable via `CONFIG_MCP_RATE_LIMIT_RPS`

### 2.5 Content-Type

```text
Content-Type: application/json (required)
```

- Wrong type → HTTP 415 Unsupported Media Type

### 2.6 Request Size

```text
Maximum: 4096 bytes
```

- Exceeds → HTTP 413 Content Too Large + Connection: close

---

## 3. MCP Headers

Every MCP 2026-07-28 request MUST include:

```text
MCP-Protocol-Version: 2026-07-28
Mcp-Method: <JSON-RPC method>
```

For `tools/call`, `Mcp-Name` is also required:

```text
Mcp-Name: <tool name from params.name>
```

### 3.1 Header Validation Rules

| Rule | Error Code | HTTP Status |
|---|---|---|
| Missing `MCP-Protocol-Version` (legacy off) | -32022 | 400 |
| Unsupported version | -32022 | 400 |
| Missing `Mcp-Method` | -32020 | 400 |
| `Mcp-Method` != body `method` | -32020 | 400 |
| Missing `Mcp-Name` for `tools/call` | -32020 | 400 |
| `Mcp-Name` != body `params.name` | -32020 | 400 |
| Base64 `Mcp-Name` decode failure | -32020 | 400 |

### 3.2 Base64 Sentinel Encoding

If the tool name contains characters not safe for HTTP headers, the client may
Base64-encode it with the sentinel prefix `\x00b64:`. The gateway decodes
and normalizes before comparison.

---

## 4. Required Request `_meta`

Every MCP 2026-07-28 request body MUST include:

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "tools/list",
  "params": {
    "_meta": {
      "io.modelcontextprotocol/protocolVersion": "2026-07-28",
      "io.modelcontextprotocol/clientCapabilities": {}
    }
  }
}
```

| Field | Type | Required | Notes |
|---|---|---|---|
| `protocolVersion` | string | Yes | Must equal `"2026-07-28"` |
| `clientCapabilities` | object | Yes | Can be empty `{}` |
| `clientInfo` | object | No | Optional, not used for authorization |

**Validation errors:** `-32602 Invalid params`

---

## 5. Methods

### 5.1 `server/discover`

Returns server identity, capabilities, and supported protocol versions.

**Request:**
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "server/discover",
  "params": {
    "_meta": {
      "io.modelcontextprotocol/protocolVersion": "2026-07-28",
      "io.modelcontextprotocol/clientCapabilities": {}
    }
  }
}
```

**Headers:**
```text
MCP-Protocol-Version: 2026-07-28
Mcp-Method: server/discover
```

**Response:**
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "resultType": "complete",
    "supportedVersions": ["2026-07-28"],
    "capabilities": {
      "tools": {
        "listChanged": false
      }
    },
    "_meta": {
      "io.modelcontextprotocol/serverInfo": {
        "name": "esp32-ble-gateway",
        "version": "1.0.0"
      }
    },
    "instructions": "Controls BLE devices managed by this ESP32 gateway.",
    "ttlMs": 60000,
    "cacheScope": "private"
  },
  "_meta": {
    "io.modelcontextprotocol/serverInfo": {
      "name": "esp32-ble-gateway",
      "version": "1.0.0"
    }
  }
}
```

**Notes:**
- `resultType` is always `"complete"`
- `supportedVersions` contains `"2026-07-28"`
- No resources, prompts, or sampling capabilities advertised
- `cacheScope` is `"private"` (per-client caching)
- `instructions` is static and short

---

### 5.2 `tools/list`

Returns the list of available tools: **static gateway tools + dynamic device tools**.

**Request:**
```json
{
  "jsonrpc": "2.0",
  "id": 2,
  "method": "tools/list",
  "params": {
    "_meta": {
      "io.modelcontextprotocol/protocolVersion": "2026-07-28",
      "io.modelcontextprotocol/clientCapabilities": {}
    }
  }
}
```

**Headers:**
```text
MCP-Protocol-Version: 2026-07-28
Mcp-Method: tools/list
```

**Response (with dynamic tools enabled):**
```json
{
  "jsonrpc": "2.0",
  "id": 2,
  "result": {
    "resultType": "complete",
    "ttlMs": 60000,
    "cacheScope": "private",
    "tools": [
      {
        "name": "get_status",
        "description": "Get gateway and BLE status",
        "inputSchema": { "type": "object", "properties": {} },
        "annotations": { "readOnlyHint": true, "destructiveHint": false, "idempotentHint": true }
      },
      {
        "name": "list_devices",
        "description": "List devices known by the gateway",
        "inputSchema": { "type": "object", "properties": {} },
        "annotations": { "readOnlyHint": true, "destructiveHint": false, "idempotentHint": true }
      },
      {
        "name": "list_device_capabilities",
        "description": "List commands advertised by a BLE device",
        "inputSchema": { "type": "object", "properties": { "device_id": { "type": "string" } } },
        "annotations": { "readOnlyHint": true, "destructiveHint": false, "idempotentHint": true }
      },
      {
        "name": "device_command",
        "description": "Send an allowlisted command to a device",
        "inputSchema": { "type": "object", "properties": { "device_id": { "type": "string" }, "command": { "type": "string" } } },
        "annotations": { "readOnlyHint": false, "destructiveHint": false }
      },
      {
        "name": "fan_01.set_speed",
        "description": "Set speed for TEST fan (0–100)",
        "inputSchema": {
          "type": "object",
          "properties": {
            "value": { "type": "integer", "minimum": 0, "maximum": 100 }
          },
          "required": ["value"]
        },
        "annotations": { "readOnlyHint": false, "destructiveHint": false, "idempotentHint": true }
      },
      {
        "name": "fan_01.toggle",
        "description": "Toggle TEST fan on/off",
        "inputSchema": { "type": "object", "properties": {} },
        "annotations": { "readOnlyHint": false, "destructiveHint": false, "idempotentHint": true }
      }
    ],
    "_meta": {
      "io.modelcontextprotocol/serverInfo": {
        "name": "esp32-ble-gateway",
        "version": "1.0.0"
      }
    }
  }
}
```

**Tool order:** Static tools first (deterministic), then dynamic enabled tools
(alphabetical by tool_name). Tool names are NVS-persisted — the same tool
always has the same name across reboots.

**Dynamic tool naming:**
- Fast path: `{device_id}.{command}` (e.g. `fan_01.set_speed`)
- Sanitized (non-ASCII device_id): `{slug}_{hash16}.{command}`
- Max 128 characters, registered at enable time, never changes
- Dynamic tools also expose the command-first display title
  `{command} on {device_name}` in both `title` and `annotations.title` for
  compatibility with new and legacy MCP clients. It falls back to
  `{device_id}` if the stored device name is unavailable. Calls continue to
  use the stable `name` field.

---

### 5.3 `tools/call`

Execute a tool with the given arguments. Works for both static and dynamic tools.

**Static tool (device_command):**
```json
{
  "jsonrpc": "2.0",
  "id": 3,
  "method": "tools/call",
  "params": {
    "name": "device_command",
    "arguments": {
      "device_id": "fan_01",
      "command": "set_speed",
      "int_value": 60
    },
    "_meta": {
      "io.modelcontextprotocol/protocolVersion": "2026-07-28",
      "io.modelcontextprotocol/clientCapabilities": {}
    }
  }
}
```

**Dynamic tool (direct name):**
```json
{
  "jsonrpc": "2.0",
  "id": 4,
  "method": "tools/call",
  "params": {
    "name": "fan_01.set_speed",
    "arguments": {
      "value": 60
    },
    "_meta": {
      "io.modelcontextprotocol/protocolVersion": "2026-07-28",
      "io.modelcontextprotocol/clientCapabilities": {}
    }
  }
}
```

**Headers:**
```text
MCP-Protocol-Version: 2026-07-28
Mcp-Method: tools/call
Mcp-Name: fan_01.set_speed
```

**Response (success):**
```json
{
  "jsonrpc": "2.0",
  "id": 4,
  "result": {
    "resultType": "complete",
    "content": [
      { "type": "text", "text": "Command executed successfully" }
    ],
    "structuredContent": {
      "success": true,
      "device_id": "fan_01",
      "command": "set_speed"
    },
    "isError": false
  }
}
```

**Dynamic tool resolution (§32):**

1. Lookup tool in catalog by name → get device_id + command
2. Verify exposure state is `ENABLED` (defense in depth)
3. Verify device still exists in store
4. Verify capability still matches (semantic digest check)
5. Map `value` → `int_value`/`bool_value` (NO `normalize_arguments`)
6. Validate arguments via `device_capabilities_validate_command`
7. Submit to command_executor

If any check fails → JSON-RPC error `-32602` with descriptive message.

---

## 6. Tool Details

### 6.1 Static Tools

#### `get_status`
**Description:** Get gateway and BLE status
**Input:** None
**Annotations:** readOnly, non-destructive, idempotent

**structuredContent:**
```json
{
  "status": "ok",
  "device_count": 1,
  "connected_count": 1,
  "ble_link_count": 1,
  "internal": { "free": 56975, "min_free": 39828, "largest_free_block": 31744 },
  "psram": { "ready": true, "free": 7853840, "min_free": 7851340, "largest_free_block": 7733248 }
}
```

#### `list_devices`
**Description:** List devices known by the gateway
**Input:** None
**Annotations:** readOnly, non-destructive, idempotent

#### `list_device_capabilities`
**Description:** List commands advertised by a BLE device
**Input:** `device_id` (string, required)
**Annotations:** readOnly, non-destructive, idempotent

#### `device_command`
**Description:** Send an allowlisted command to a device
**Input:** `device_id` (string), `command` (string), `int_value` (integer, optional), `bool_value` (boolean, optional)
**Annotations:** non-read-only

**Async execution:**
- Queue capacity: 2 pending + 1 running
- Queue full → HTTP 503 + JSON-RPC `-32000 Gateway busy`
- HTTPD task never blocks waiting for BLE ACK

### 6.2 Dynamic Device Tools

Each enabled command becomes a first-class MCP tool:

| Tool Name | Description | Value Schema |
|---|---|---|
| `{device_id}.{command}` | Device command label | Based on capability |

**Example dynamic tools for device `fan_01`:**
| Tool Name | Description | Value |
|---|---|---|
| `fan_01.set_speed` | Set fan speed (0–100) | `value: integer` |
| `fan_01.toggle` | Toggle fan on/off | none |
| `fan_01.set_mode` | Set operating mode | `value: integer` |

**Value mapping (§33):**
- `value_type = INT`: `arguments.value` → `msg.int_value`
- `value_type = BOOL`: `arguments.value` → `msg.bool_value`
- `value_type = NONE`: no value argument

---

## 7. Admin API — MCP Tool Exposure

Admin-protected REST endpoints for managing which device commands are
exposed as MCP tools. Requires `CONFIG_WEB_ADMIN_AUTH_TOKEN`.

### 7.1 `GET /api/mcp/exposures`

Returns exposure state, capacity, and catalog info for a device.

**Headers:**
```text
Authorization: Bearer <admin_token>
```

**Query:** `?device_id=<id>`

**Response:**
```json
{
  "device_id": "fan_01",
  "catalog_revision": 5,
  "capacity": {
    "enabled": 2,
    "max_enabled": 32,
    "records": 2,
    "max_records": 96
  },
  "commands": [
    {
      "command": "set_speed",
      "label": "Set fan speed",
      "value_type": "integer",
      "destructive": false,
      "idempotent": true,
      "minimum": 0,
      "maximum": 100,
      "step": 1,
      "unit": "%",
      "enabled": true,
      "state": "enabled",
      "tool_name": "fan_01.set_speed"
    },
    {
      "command": "toggle",
      "label": "Toggle fan on/off",
      "value_type": "none",
      "destructive": false,
      "idempotent": true,
      "enabled": false,
      "state": "disabled",
      "tool_name": "fan_01.toggle"
    }
  ]
}
```

**States:**
| State | Meaning |
|---|---|
| `enabled` | Tool exposed in `tools/list`, callable via `tools/call` |
| `disabled` | Not exposed, can be enabled |
| `needs_review` | Capability changed since enable, tool blocked until re-enabled |
| `orphaned` | Device deleted but exposure record retained |

### 7.2 `PUT /api/mcp/exposures`

Enable or disable a tool. Supports single command or bulk mode.

**Headers:**
```text
Authorization: Bearer <admin_token>
Content-Type: application/json
```

**Single command:**
```json
{
  "device_id": "fan_01",
  "command": "set_speed",
  "enabled": true,
  "confirm_destructive": true
}
```

**Bulk mode:**
```json
{
  "device_id": "fan_01",
  "commands": [
    { "command": "set_speed", "enabled": true },
    { "command": "toggle", "enabled": false, "confirm_destructive": true }
  ]
}
```

**Response (single):**
```json
{ "success": true }
```

**Response (bulk):**
```json
{
  "success": true,
  "results": [
    { "command": "set_speed", "success": true },
    { "command": "toggle", "success": true }
  ]
}
```

**Error responses:**
| HTTP | Error | Meaning |
|---|---|---|
| 401 | `unauthorized` | Invalid admin token |
| 403 | `admin_auth_not_configured` | No token set |
| 404 | `not_found` | Device or command not found |
| 409 | `mcp_tool_capacity_exceeded` | Max enabled tools reached |
| 409 | `capabilities_not_ready` | No committed capabilities |
| 409 | `destructive_blocked` | Destructive command not confirmed |

**Confirming destructive commands:**
- Destructive commands (set by peripheral firmware via `DEVICE_CAP_FLAG_DESTRUCTIVE`)
- Require `"confirm_destructive": true` in the request
- Controlled by `CONFIG_MCP_DYNAMIC_ALLOW_DESTRUCTIVE` (default: disabled)

---

## 8. Error Handling

### 8.1 JSON-RPC Errors (Protocol Layer)

| Code | Name | HTTP Status | Description |
|---|---|---|---|
| `-32700` | Parse error | 200 | Invalid JSON body |
| `-32600` | Invalid Request | 200/413 | Malformed JSON-RPC, body exceeds limit |
| `-32601` | Method not found | **404** | Unknown MCP method |
| `-32602` | Invalid params | 200 | Missing `_meta`, invalid tool name, wrong arguments |
| `-32603` | Internal error | 200/500 | OOM, executor unavailable |
| `-32000` | Gateway busy | **503** | Command executor queue full |
| `-32001` | Device unavailable | 200 | Device not in store |
| `-32002` | Command denied | 200 | Command not in allowlist |
| `-32003` | Capability unknown | 200 | Device capabilities not ready |
| `-32020` | Header mismatch | **400** | Mcp-Method/Mcp-Name mismatch |
| `-32022` | Unsupported version | **400** | Protocol version not supported |

### 8.2 Tool Execution Errors (Application Layer)

When a `tools/call` request is valid but the tool execution fails, the error
is returned in `CallToolResult.isError = true`, NOT as a JSON-RPC error.

```json
{
  "result": {
    "resultType": "complete",
    "content": [{"type": "text", "text": "Device is offline"}],
    "isError": true
  }
}
```

### 8.3 HTTP Transport Errors

| HTTP Status | Trigger |
|---|---|
| 401 Unauthorized | Missing/invalid Bearer token |
| 403 Forbidden | Host/Origin not in allowlist |
| 413 Content Too Large | Body exceeds 4096 bytes |
| 415 Unsupported Media Type | Wrong Content-Type |
| 429 Too Many Requests | Rate limit exceeded |

---

## 9. Configuration

### 9.1 Kconfig Options — MCP Endpoint

| Option | Default | Description |
|---|---|---|
| `CONFIG_MCP_AUTH_TOKEN` | `""` | Bearer token (empty = dev mode) |
| `CONFIG_MCP_HOST_ALLOWLIST` | `"gateway.local,192.168.4.1"` | Allowed Host headers |
| `CONFIG_MCP_DEVICE_COMMAND_ALLOWLIST` | `""` | Allowed device commands (empty = deny all) |
| `CONFIG_MCP_LEGACY_MODE` | `n` | Accept requests without MCP-Protocol-Version header |
| `CONFIG_MCP_RATE_LIMIT_RPS` | `10` | Rate limit (requests/second) |
| `CONFIG_MCP_TOOLS_CACHE_TTL_MS` | `60000` | tools/list cache TTL (ms) |

### 9.2 Kconfig Options — Dynamic Tool Exposure

| Option | Default | Description |
|---|---|---|
| `CONFIG_MCP_DYNAMIC_TOOLS` | `y` | Enable dynamic device tool exposure |
| `CONFIG_MCP_DYNAMIC_TOOL_MAX_ENABLED` | `32` | Max simultaneously enabled dynamic tools |
| `CONFIG_MCP_EXPOSURE_RECORD_MAX` | `96` | Max NVS exposure records |
| `CONFIG_MCP_DYNAMIC_ALLOW_DESTRUCTIVE` | `n` | Allow exposing destructive commands |
| `CONFIG_MCP_KEEP_GENERIC_DEVICE_COMMAND` | `n` | Keep generic `device_command` in tools/list |
| `CONFIG_MCP_EXPOSE_FULL_CAPABILITY_TOOL` | `n` | Keep `device_command(device_id,command)` fallback |

### 9.3 Kconfig Options — Web Admin Auth

| Option | Default | Description |
|---|---|---|
| `CONFIG_WEB_ADMIN_AUTH_TOKEN` | `""` | Bearer token for admin API (empty = disabled) |

### 9.4 NVS Runtime Overrides

| Namespace | Key | Type | Description |
|---|---|---|---|
| `mcp` | `token` | string | MCP Bearer token override |
| `mcp` | `legacy` | u8 | Legacy mode override (1=on, 0=off) |
| `web_admin` | `token` | string | Admin API Bearer token |
| `mcp_exp` | `exposures` | blob | Exposure records (schema v2, auto-managed) |

---

## 10. Implementation Details

### 10.1 Component Architecture

| File | Responsibility |
|---|---|
| `mcp_endpoint.c` | HTTP route, body receive, JSON-RPC dispatch |
| `mcp_codec.c` | Protocol validation, `_meta` validation, Base64 decode, `server/discover` |
| `mcp_rpc.c` | JSON-RPC envelope (result/error), `serverInfo` in `result._meta` |
| `mcp_registry.c` | Static tool table + dynamic schema builder |
| `mcp_tools.c` | Argument parsing, `gw_message_t` normalization, dynamic resolver |
| `mcp_policy.c` | Device capability + allowlist + destructive guard |
| `mcp_auth.c` | Bearer token, Host/Origin, Content-Type, rate limit |
| `mcp_tool_exposure.c` | Exposure enable/disable/reconcile/forget, worker task |
| `mcp_tool_catalog.c` | RAM catalog with mutex, revision tracking |
| `mcp_tool_name.c` | Deterministic tool name generation (PSA SHA-256) |
| `mcp_tool_digest.c` | Capability semantic digest (128-bit truncated SHA-256) |
| `mcp_tool_exposure_store.c` | NVS blob persistence (schema v2) |
| `web_admin_auth.c` | Admin bearer token, constant-time comparison |

### 10.2 Dynamic Tool Lifecycle

```text
Device discovered → capabilities committed → capabilities discovered
        |
        v
GET /api/mcp/exposures?device_id=X  →  see commands
        |
        v
PUT /api/mcp/exposures  →  enable command
        |
        v
mcp_tool_name_generate() → "fan_01.set_speed"
mcp_tool_digest_compute() → 128-bit hash
NVS write (schema v2) → persistence
catalog_add() → RAM catalog
        |
        v
tools/list → merged static + dynamic
tools/call "fan_01.set_speed" → resolve → validate → execute
```

### 10.3 Boot Reconciliation

On boot, `mcp_tool_exposure_init()` reads all NVS records and:
1. Checks each device still exists in device_store
2. Re-checks capability digest matches
3. Marks mismatched records as `needs_review`
4. Re-enables matching records

### 10.4 MCP 2026-07-28 Response Metadata

Every successful response includes `serverInfo` in `result._meta`:

```json
{
  "_meta": {
    "io.modelcontextprotocol/serverInfo": {
      "name": "esp32-ble-gateway",
      "version": "1.0.0"
    }
  }
}
```

The JSON-RPC-level `_meta` is NOT used for server identity (per spec §35).

---

## 11. Example: AI Voice Flow

### Static Tool
```text
User: "Bật quạt phòng khách lên 60%"
        |
        v
AI Host resolves device + command
        |
        v
tools/call device_command(device_id="fan_01", command="set_speed", int_value=60)
        |
        v
Gateway: validate → policy check → async submit → BLE ACK → result
        |
        v
AI Host reads structuredContent and responds to user
```

### Dynamic Tool
```text
User: "Bật quạt phòng khách lên 60%"
        |
        v
AI Host calls tools/list → sees "fan_01.set_speed" with value schema
        |
        v
tools/call fan_01.set_speed({ "value": 60 })
        |
        v
Gateway: resolve dynamic → build gw_message_t → validate → async → BLE ACK
        |
        v
AI Host reads structuredContent and responds to user
```

---

## 12. Testing

### 12.1 Manual Test — Static Tools

```sh
# server/discover
curl -s -X POST http://<IP>/mcp \
  -H "Content-Type: application/json" \
  -H "Host: gateway.local" \
  -H "MCP-Protocol-Version: 2026-07-28" \
  -H "Mcp-Method: server/discover" \
  -d '{"jsonrpc":"2.0","id":1,"method":"server/discover","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}}}}'

# tools/list
curl -s -X POST http://<IP>/mcp \
  -H "Content-Type: application/json" \
  -H "Host: gateway.local" \
  -H "MCP-Protocol-Version: 2026-07-28" \
  -H "Mcp-Method: tools/list" \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}}}}'
```

### 12.2 Manual Test — Admin API

```sh
# Get exposures for a device
curl -s http://<IP>/api/mcp/exposures?device_id=fan_01 \
  -H "Authorization: Bearer <admin_token>"

# Enable a tool
curl -s -X PUT http://<IP>/api/mcp/exposures \
  -H "Authorization: Bearer <admin_token>" \
  -H "Content-Type: application/json" \
  -d '{"device_id":"fan_01","command":"set_speed","enabled":true}'

# Disable a tool
curl -s -X PUT http://<IP>/api/mcp/exposures \
  -H "Authorization: Bearer <admin_token>" \
  -H "Content-Type: application/json" \
  -d '{"device_id":"fan_01","command":"set_speed","enabled":false}'

# Call dynamic tool
curl -s -X POST http://<IP>/mcp \
  -H "Content-Type: application/json" \
  -H "Host: gateway.local" \
  -H "MCP-Protocol-Version: 2026-07-28" \
  -H "Mcp-Method: tools/call" \
  -H "Mcp-Name: fan_01.set_speed" \
  -d '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"fan_01.set_speed","arguments":{"value":60},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}}}}'
```

### 12.3 Unity Tests

```sh
cd test
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

Test tags: `[mcp_endpoint]`, `[mcp_conformance]`, `[mcp_stress]`

---

## 13. SDK Interoperability

Tested with:
- ESP32 firmware (this project)
- Official MCP SDK clients (TypeScript, Python)

Success criteria:
- No custom transport patches needed
- No custom JSON-RPC wrapper needed
- No legacy aliases required
- No metadata validation bypass

---

## 14. Security Model

```text
Threat: LAN-only, plaintext HTTP
  - Bearer token can be sniffed/replayed on same LAN segment
  - Token is convenience gate, NOT TLS replacement
  - Never expose /mcp to Internet

Threat: DNS rebinding from browsers
  - Host/Origin validation with allowlist
  - Host header is attacker-controlled on direct requests
  - Never treat Host as authentication

Threat: Resource exhaustion
  - Rate limiting (10 req/s token bucket)
  - Request size limit (4096 bytes)
  - Async queue bounded (2 pending + 1 running)
  - Dynamic tool cap (CONFIG_MCP_DYNAMIC_TOOL_MAX_ENABLED)
  - Exposure record cap (CONFIG_MCP_EXPOSURE_RECORD_MAX)
  - No session state, no memory leak paths

Threat: Tool exposure integrity
  - 128-bit semantic digest on capabilities
  - Capability change → needs_review (tool blocked)
  - Defense in depth: re-checks ENABLED state on every tools/call
  - Deterministic tool names (no collision, no injection)

Cloud AI voice architecture:
  Cloud AI
     |
    HTTPS/Auth
     |
  Local MCP Bridge (Raspberry Pi / NAS / Mac)
     |
    LAN
     |
  ESP32 Gateway
```
