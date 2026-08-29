# Xiaozhi external MCP wire fixtures

- Capture date: 2026-08-29
- Endpoint type: Xiaozhi external MCP WSS endpoint
- MCP protocol version: `2024-11-05`
- Client: `xz-mcp-broker/0.0.1`
- Framing: one raw JSON-RPC message in WebSocket TEXT frames
- WebSocket subprotocol: none observed
- Authentication: signed credential in the endpoint query string
- Initialize request ID: JSON number `0`
- `notifications/initialized`: no `params`
- `tools/list`: JSON number ID and `params: {}`
- Health check: JSON-RPC `ping`, distinct from WebSocket control ping/pong

The signed endpoint and token are deliberately not stored in this directory.
`tools/call`, close/reconnect behavior, cancellation, and progress frames have
not yet been captured and must not be fabricated to make tests pass.
