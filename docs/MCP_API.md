# ESP32 BLE Gateway — MCP API Documentation

**MCP Protocol Version:** `2026-07-28`
**Endpoint:** `POST /mcp`
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
| tools/list                   |
| tools/call                   |
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
- No dynamic tool generation per device
- LAN-only (plaintext HTTP, never expose to Internet)

---

## 2. Authentication & Security

### 2.1 Bearer Token

```text
Authorization: Bearer <token>
```

- Token configured via `CONFIG_MCP_AUTH_TOKEN` or NVS key `mcp.token`
- Empty token = dev mode (no auth, warning logged)
- Constant-time comparison, never logged
- Missing/invalid → HTTP 401 Unauthorized

### 2.2 Host/Origin Validation

```text
Host: gateway.local
```

- Must match `CONFIG_MCP_HOST_ALLOWLIST` (comma-separated)
- Entries: `gateway.local,192.168.4.1` (default)
- Case-insensitive, port stripped, IPv6 brackets preserved
- Mismatch → HTTP 403 Forbidden (DNS rebinding protection)

### 2.3 Rate Limiting

```text
Token bucket: 10 req/s, burst capacity 10
```

- Exceeds → HTTP 429 Too Many Requests
- Configurable via `CONFIG_MCP_RATE_LIMIT_RPS`

### 2.4 Content-Type

```text
Content-Type: application/json (required)
```

- Wrong type → HTTP 415 Unsupported Media Type

### 2.5 Request Size

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

Returns the list of available tools with schemas and annotations.

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

**Response:**
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
        "inputSchema": {
          "type": "object",
          "properties": {},
          "additionalProperties": false
        },
        "annotations": {
          "readOnlyHint": true,
          "destructiveHint": false,
          "idempotentHint": true
        }
      },
      {
        "name": "list_devices",
        "description": "List devices known by the gateway",
        "inputSchema": {
          "type": "object",
          "properties": {},
          "additionalProperties": false
        },
        "annotations": {
          "readOnlyHint": true,
          "destructiveHint": false,
          "idempotentHint": true
        }
      },
      {
        "name": "list_device_capabilities",
        "description": "List commands advertised by a BLE device",
        "inputSchema": {
          "type": "object",
          "properties": {
            "device_id": {
              "type": "string",
              "maxLength": 31,
              "description": "Target device identifier"
            }
          },
          "additionalProperties": false,
          "required": ["device_id"]
        },
        "annotations": {
          "readOnlyHint": true,
          "destructiveHint": false,
          "idempotentHint": true
        }
      },
      {
        "name": "device_command",
        "description": "Send an allowlisted command to a device",
        "inputSchema": {
          "type": "object",
          "properties": {
            "device_id": {
              "type": "string",
              "maxLength": 31,
              "description": "Target device identifier",
              "minLength": 1
            },
            "command": {
              "type": "string",
              "maxLength": 31,
              "description": "Command to execute",
              "minLength": 1
            },
            "int_value": {
              "type": "integer",
              "description": "Integer command argument"
            },
            "bool_value": {
              "type": "boolean",
              "description": "Boolean command argument"
            }
          },
          "additionalProperties": false,
          "required": ["device_id", "command"]
        },
        "annotations": {
          "readOnlyHint": false,
          "destructiveHint": false
        }
      }
    ],
    "tool_names": [
      "get_status",
      "list_devices",
      "list_device_capabilities",
      "device_command"
    ],
    "_meta": {
      "io.modelcontextprotocol/serverInfo": {
        "name": "esp32-ble-gateway",
        "version": "1.0.0"
      }
    }
  },
  "_meta": {
    "io.modelcontextprotocol/serverInfo": {
      "name": "esp32-ble-gateway",
      "version": "1.0.0"
    }
  }
}
```

**Tool order** is deterministic (stable for caching): `get_status`, `list_devices`, `list_device_capabilities`, `device_command`.

---

### 5.3 `tools/call`

Execute a tool with the given arguments.

**Request:**
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

**Headers:**
```text
MCP-Protocol-Version: 2026-07-28
Mcp-Method: tools/call
Mcp-Name: device_command
```

**Response (success):**
```json
{
  "jsonrpc": "2.0",
  "id": 3,
  "result": {
    "resultType": "complete",
    "content": [
      {
        "type": "text",
        "text": "Command executed successfully"
      }
    ],
    "structuredContent": {
      "success": true,
      "device_id": "fan_01",
      "command": "set_speed"
    },
    "isError": false,
    "_meta": {
      "io.modelcontextprotocol/serverInfo": {
        "name": "esp32-ble-gateway",
        "version": "1.0.0"
      }
    }
  },
  "_meta": {
    "io.modelcontextprotocol/serverInfo": {
      "name": "esp32-ble-gateway",
      "version": "1.0.0"
    }
  }
}
```

**Response (tool error):**
```json
{
  "jsonrpc": "2.0",
  "id": 3,
  "result": {
    "resultType": "complete",
    "content": [
      {
        "type": "text",
        "text": "command 'toggle' is not allowed by policy"
      }
    ],
    "isError": true,
    "_meta": {
      "io.modelcontextprotocol/serverInfo": {
        "name": "esp32-ble-gateway",
        "version": "1.0.0"
      }
    }
  },
  "_meta": {
    "io.modelcontextprotocol/serverInfo": {
      "name": "esp32-ble-gateway",
      "version": "1.0.0"
    }
  }
}
```

---

## 6. Tool Details

### 6.1 `get_status`

**Description:** Get gateway and BLE status
**Input:** None
**Annotations:** readOnly, non-destructive, idempotent

**Example:**
```sh
curl -s -X POST http://<IP>/mcp \
  -H "Content-Type: application/json" \
  -H "Host: gateway.local" \
  -H "MCP-Protocol-Version: 2026-07-28" \
  -H "Mcp-Method: tools/call" \
  -H "Mcp-Name: get_status" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"get_status","_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}}}}'
```

**structuredContent:**
```json
{
  "status": "ok",
  "device_count": 1,
  "connected_count": 1,
  "ble_link_count": 1,
  "internal": {
    "free": 56975,
    "min_free": 39828,
    "largest_free_block": 31744
  },
  "psram": {
    "ready": true,
    "free": 7853840,
    "min_free": 7851340,
    "largest_free_block": 7733248
  }
}
```

---

### 6.2 `list_devices`

**Description:** List devices known by the gateway
**Input:** None
**Annotations:** readOnly, non-destructive, idempotent

**structuredContent:**
```json
[
  {
    "device_id": "AC:27:6E:CC:F2:26",
    "name": "TEST",
    "type": "generic",
    "connected": true,
    "has_ble_addr": true,
    "ble_addr": "AC:27:6E:CC:F2:26",
    "ble_addr_type": 0
  }
]
```

---

### 6.3 `list_device_capabilities`

**Description:** List commands advertised by a BLE device
**Input:** `device_id` (string, required)
**Annotations:** readOnly, non-destructive, idempotent

**Example:**
```sh
curl -s -X POST http://<IP>/mcp \
  -H "Content-Type: application/json" \
  -H "Host: gateway.local" \
  -H "MCP-Protocol-Version: 2026-07-28" \
  -H "Mcp-Method: tools/call" \
  -H "Mcp-Name: list_device_capabilities" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"list_device_capabilities","arguments":{"device_id":"AC:27:6E:CC:F2:26"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}}}}'
```

**structuredContent:**
```json
{
  "device_id": "AC:27:6E:CC:F2:26",
  "commands": [
    {
      "name": "toggle",
      "value_type": "none",
      "flags": ["idempotent"]
    },
    {
      "name": "set_speed",
      "value_type": "int",
      "min": 0,
      "max": 100,
      "unit": "%",
      "flags": []
    }
  ]
}
```

---

### 6.4 `device_command`

**Description:** Send an allowlisted command to a device
**Input:** `device_id` (string, required), `command` (string, required), `int_value` (integer, optional), `bool_value` (boolean, optional)
**Annotations:** non-read-only, non-destructive (destructive commands blocked by policy)

**Policy checks (in order):**
1. Device exists in store
2. Capabilities ready (state = READY)
3. Command advertised by device
4. Command in `CONFIG_MCP_DEVICE_COMMAND_ALLOWLIST`
5. Command not destructive (control profile)

**Example:**
```sh
curl -s -X POST http://<IP>/mcp \
  -H "Content-Type: application/json" \
  -H "Host: gateway.local" \
  -H "MCP-Protocol-Version: 2026-07-28" \
  -H "Mcp-Method: tools/call" \
  -H "Mcp-Name: device_command" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"device_command","arguments":{"device_id":"AC:27:6E:CC:F2:26","command":"set_speed","int_value":60},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}}}}'
```

**Async execution:**
- `device_command` is executed asynchronously via `command_executor`
- Queue capacity: 2 pending + 1 running
- Queue full → HTTP 503 + JSON-RPC `-32000 Gateway busy`
- HTTPD task never blocks waiting for BLE ACK

---

## 7. Error Handling

### 7.1 JSON-RPC Errors (Protocol Layer)

Returned in the JSON-RPC `error` object when the request cannot be processed at the protocol level.

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

### 7.2 Tool Execution Errors (Application Layer)

When a `tools/call` request is valid but the tool execution fails, the error is returned in `CallToolResult.isError = true`, NOT as a JSON-RPC error. This allows AI clients to read and retry.

```json
{
  "result": {
    "resultType": "complete",
    "content": [{"type": "text", "text": "Device is offline"}],
    "isError": true
  }
}
```

### 7.3 HTTP Transport Errors

Returned as plain HTTP errors (no JSON-RPC envelope) for security/transport failures.

| HTTP Status | Trigger |
|---|---|
| 401 Unauthorized | Missing/invalid Bearer token |
| 403 Forbidden | Host/Origin not in allowlist |
| 413 Content Too Large | Body exceeds 4096 bytes |
| 415 Unsupported Media Type | Wrong Content-Type |
| 429 Too Many Requests | Rate limit exceeded |

---

## 8. Configuration

### 8.1 Kconfig Options

| Option | Default | Description |
|---|---|---|
| `CONFIG_MCP_AUTH_TOKEN` | `""` | Bearer token (empty = dev mode) |
| `CONFIG_MCP_HOST_ALLOWLIST` | `"gateway.local,192.168.4.1"` | Allowed Host headers |
| `CONFIG_MCP_DEVICE_COMMAND_ALLOWLIST` | `""` | Allowed device commands (empty = deny all) |
| `CONFIG_MCP_LEGACY_MODE` | `n` | Accept requests without MCP-Protocol-Version header |
| `CONFIG_MCP_RATE_LIMIT_RPS` | `10` | Rate limit (requests/second) |

### 8.2 NVS Runtime Overrides

| Namespace | Key | Type | Description |
|---|---|---|---|
| `mcp` | `token` | string | Bearer token override |
| `mcp` | `legacy` | u8 | Legacy mode override (1=on, 0=off) |

Override via `mcp_codec_set_legacy_override()` or direct NVS write.

---

## 9. Implementation Details

### 9.1 Component Architecture

| File | Responsibility |
|---|---|
| `mcp_endpoint.c` | HTTP route, body receive, JSON-RPC dispatch |
| `mcp_codec.c` | Protocol validation, `_meta` validation, Base64 decode, `server/discover` |
| `mcp_rpc.c` | JSON-RPC envelope (result/error), `serverInfo` in `result._meta` |
| `mcp_registry.c` | Tool table, schemas, annotations |
| `mcp_tools.c` | Argument parsing, `gw_message_t` normalization, result formatting |
| `mcp_policy.c` | Device capability + allowlist + destructive guard |
| `mcp_auth.c` | Bearer token, Host/Origin, Content-Type, rate limit |

### 9.2 MCP 2026-07-28 Response Metadata

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

### 9.3 No Synchronous Fallback

If async submission fails (OOM, executor unavailable, queue full), the gateway
returns an error immediately. It NEVER falls back to synchronous BLE execution
from the HTTP handler.

---

## 10. Example: AI Voice Flow

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
Gateway:
  1. Validate headers (_meta, Mcp-Method, Mcp-Name)
  2. Registry lookup → device_command
  3. Policy check → device exists, capabilities ready, command allowed
  4. Async submit → command_executor
  5. BLE Central sends command to fan_01
  6. BLE ACK received
  7. Completion callback → CallToolResult
        |
        v
AI Host reads structuredContent and responds to user
```

---

## 11. Testing

### 11.1 Manual Test

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

# tools/call get_status
curl -s -X POST http://<IP>/mcp \
  -H "Content-Type: application/json" \
  -H "Host: gateway.local" \
  -H "MCP-Protocol-Version: 2026-07-28" \
  -H "Mcp-Method: tools/call" \
  -H "Mcp-Name: get_status" \
  -d '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"get_status","_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}}}}'
```

### 11.2 Unity Tests

```sh
cd test
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

Test tags: `[mcp_endpoint]`, `[mcp_conformance]`, `[mcp_stress]`

---

## 12. SDK Interoperability

Tested with:
- ESP32 firmware (this project)
- Official MCP SDK clients (TypeScript, Python)

Success criteria:
- No custom transport patches needed
- No custom JSON-RPC wrapper needed
- No legacy aliases required
- No metadata validation bypass

---

## 13. Security Model

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
  - No session state, no memory leak paths

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
