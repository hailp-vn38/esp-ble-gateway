#ifndef MCP_WS_BRIDGE_INTERNAL_H
#define MCP_WS_BRIDGE_INTERNAL_H

#include "sdkconfig.h"

#ifdef CONFIG_MCP_WS_BRIDGE

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "mcp_ws_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Event types (shared between modules) ───────────────────────────── */

typedef enum {
    BRIDGE_EVENT_CONNECT,
    BRIDGE_EVENT_NETWORK_DOWN,
    BRIDGE_EVENT_WS_CONNECTED,
    BRIDGE_EVENT_WS_DISCONNECTED,
    BRIDGE_EVENT_RX_MESSAGE,
    BRIDGE_EVENT_TX_MESSAGE,
    BRIDGE_EVENT_RELOAD,
    BRIDGE_EVENT_STOP,
    BRIDGE_EVENT_HANDSHAKE_TIMEOUT,
} bridge_event_type_t;

typedef struct {
    bridge_event_type_t type;
    esp_websocket_client_handle_t client;
    uint32_t generation;
    char *payload;
    size_t payload_len;
    int error;
    int http_status;
    int close_code;
} bridge_event_t;

/* ── Shared bridge state ────────────────────────────────────────────── */

typedef struct {
    SemaphoreHandle_t lock;
    QueueHandle_t queue;
    TaskHandle_t task;
    esp_timer_handle_t reconnect_timer;
    esp_timer_handle_t handshake_timer;
    esp_websocket_client_handle_t client;
    mcp_ws_config_t config;
    mcp_ws_status_t status;
    bool initialized;
    bool started;
    bool network_up;
    bool initialize_response_sent;
    int64_t connected_at_us;
    char *rx_buffer;
    size_t rx_length;
    bool rx_active;
    bool rx_discard;
} bridge_state_t;

extern bridge_state_t s_bridge;

/* ── Core helpers (mcp_ws_bridge.c) ─────────────────────────────────── */

bool bridge_queue_event(const bridge_event_t *event);
void bridge_set_state_locked(mcp_ws_state_t state);
void bridge_invalidate_connection_locked(void);
void bridge_stop_timer(esp_timer_handle_t timer);
void bridge_schedule_reconnect(void);
void bridge_destroy_client(void);
void bridge_log_memory_snapshot(const char *label);

/* ── WebSocket transport (mcp_ws_bridge_ws.c) ───────────────────────── */

bool mcp_ws_endpoint_valid(const char *endpoint);
void mcp_ws_endpoint_display(const char *endpoint, char *out, size_t out_size);
esp_err_t mcp_ws_connect_client(void);
void mcp_ws_websocket_event_handler(void *handler_args, esp_event_base_t base,
                                    int32_t event_id, void *event_data);
void mcp_ws_handle_rx_message(bridge_event_t *event);
void mcp_ws_handle_tx_message(bridge_event_t *event);
void mcp_ws_handle_connected(const bridge_event_t *event);
void mcp_ws_handle_disconnected(const bridge_event_t *event);

/* ── Config (mcp_ws_bridge_config.c) ────────────────────────────────── */

esp_err_t mcp_ws_config_load(mcp_ws_config_t *config);
esp_err_t mcp_ws_config_store(const mcp_ws_config_t *config);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_MCP_WS_BRIDGE */
#endif /* MCP_WS_BRIDGE_INTERNAL_H */
