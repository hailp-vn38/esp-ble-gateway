# RAM-06 qualification evidence — ESP32-S3 / IDF 6.1

Date: 2026-09-03  
Runner: `tools/ram06_qualify.py`  
Target: `192.168.1.114`

## Completed LAN workload

100 iterations of REST `/api/status`, authenticated MCP `tools/list`, and
authenticated MCP `tools/call get_status` completed with 100/100 HTTP 200 for
each operation. The catalog remained stable at 2 static tools and 0 dynamic
tools.

| Metric | First | Last |
|---|---:|---:|
| Internal free | 108,367 B | 108,387 B |
| Internal minimum | 40,488 B | 40,488 B |
| Internal largest block | 63,488 B | 63,488 B |
| PSRAM free | 7,751,976 B | 7,751,976 B |
| PSRAM largest block | 7,733,248 B | 7,733,248 B |
| cJSON/MCP allocation failures | 0 | 0 |

No JSON-RPC error, socket error, or observable heap drift occurred in this
bounded run.

## Not executed

Power-cycle loops, provisioning transitions, BLE 3/6/9-link scenarios, device
ACK timeout/busy/late-ACK matrix, OTA candidate validation, TLS reconnect, and
24-hour soak require hardware fixtures or a long-running test window. They
remain open qualification gates and are not inferred from the LAN run.
