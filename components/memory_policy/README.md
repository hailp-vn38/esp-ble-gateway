# Memory allocation policy

Production uses explicit classes through `gw_mem_alloc()`:

| Data | Class | Failure behavior |
|---|---|---|
| cJSON trees and printed JSON | `GW_MEM_EXTERNAL_PREFERRED` | PSRAM first; bounded internal fallback or NULL |
| MCP/REST dispatch result workspace | `GW_MEM_EXTERNAL_PREFERRED` | request returns controlled 503/error |
| BLE wire/event buffers and DMA-visible data | `GW_MEM_INTERNAL_REQUIRED` | operation fails without consuming PSRAM |
| Device state table and lock-protected entries | static internal BSS | bounded drop/error when full |
| Device schema/exposure long-lived snapshots | external-preferred component storage | init/update returns `ESP_ERR_NO_MEM` |

Fallback is limited by `GW_MEM_FALLBACK_MAX_BYTES`, internal free floor and
largest-block floor. Counters are exposed in `/api/status` and dispatcher
`get_status`. PSRAM-required components fail explicitly when PSRAM is absent.
