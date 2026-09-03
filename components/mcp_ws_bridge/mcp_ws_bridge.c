#include "mcp_ws_bridge.h"

#include <string.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "memory_policy.h"

#include "wifi_prov.h"

#ifndef CONFIG_MCP_WS_BRIDGE

bool mcp_ws_bridge_is_supported(void) { return false; }
esp_err_t mcp_ws_bridge_init(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t mcp_ws_bridge_start(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t mcp_ws_bridge_stop(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t mcp_ws_bridge_reload(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t mcp_ws_bridge_get_status(mcp_ws_status_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->state = MCP_WS_DISABLED;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t mcp_ws_bridge_config_set(const mcp_ws_config_t *config)
{
    (void)config;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t mcp_ws_bridge_config_update(bool has_enabled, bool enabled,
                                      bool has_endpoint, const char *endpoint)
{
    (void)has_enabled;
    (void)enabled;
    (void)has_endpoint;
    (void)endpoint;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t mcp_ws_bridge_config_get_public(mcp_ws_public_config_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t mcp_ws_bridge_config_clear(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t mcp_ws_bridge_config_load(mcp_ws_config_t *out)
{
    (void)out;
    return ESP_ERR_NOT_SUPPORTED;
}
const char *mcp_ws_bridge_state_name(mcp_ws_state_t state)
{
    (void)state;
    return "disabled";
}

#else

#include "mcp_ws_bridge_internal.h"

#define MCP_WS_BACKOFF_STEPS 7

#ifdef CONFIG_MCP_WS_MEMORY_DIAGNOSTICS
static const char *TAG = "mcp_ws_bridge";
#endif

/* ── Shared bridge state ────────────────────────────────────────────── */

bridge_state_t s_bridge;

/* ── Memory diagnostics ─────────────────────────────────────────────── */

void bridge_log_memory_snapshot(const char *label)
{
#ifdef CONFIG_MCP_WS_MEMORY_DIAGNOSTICS
    ESP_LOGI(TAG,
             "[MEM:%s] INT free=%u largest=%u | DMA free=%u largest=%u | "
             "PSRAM free=%u largest=%u",
             label,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL |
                                                MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                         MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL |
                                                MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                         MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM |
                                                MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM |
                                                         MALLOC_CAP_8BIT));
#else
    (void)label;
#endif
}

/* ── Queue helper ───────────────────────────────────────────────────── */

bool bridge_queue_event(const bridge_event_t *event)
{
    return s_bridge.queue != NULL &&
           xQueueSend(s_bridge.queue, event, 0) == pdTRUE;
}

/* ── State helpers ──────────────────────────────────────────────────── */

void bridge_set_state_locked(mcp_ws_state_t state)
{
    s_bridge.status.state = state;
}

void bridge_invalidate_connection_locked(void)
{
    s_bridge.status.generation++;
    s_bridge.initialize_response_sent = false;
    s_bridge.status.negotiated_protocol_version[0] = '\0';
    s_bridge.rx_length = 0;
    s_bridge.rx_active = false;
    s_bridge.rx_discard = false;
}

/* ── Timer helpers ──────────────────────────────────────────────────── */

static void timer_queue_callback(void *arg)
{
    bridge_event_t event = {.type = (bridge_event_type_t)(intptr_t)arg};
    bridge_queue_event(&event);
}

void bridge_stop_timer(esp_timer_handle_t timer)
{
    if (timer != NULL && esp_timer_is_active(timer)) esp_timer_stop(timer);
}

/* ── Reconnect scheduling ───────────────────────────────────────────── */

void bridge_schedule_reconnect(void)
{
    static const uint32_t delays[] = {1000, 2000, 4000, 8000,
                                      16000, 30000, 60000};
    xSemaphoreTake(s_bridge.lock, portMAX_DELAY);
    if (!s_bridge.started || !s_bridge.config.enabled ||
        s_bridge.config.endpoint[0] == '\0' || !s_bridge.network_up) {
        bridge_set_state_locked(s_bridge.config.enabled ? MCP_WS_WAIT_NETWORK
                                                        : MCP_WS_DISABLED);
        xSemaphoreGive(s_bridge.lock);
        return;
    }
    uint32_t retry = s_bridge.status.retry_count++;
    uint32_t base = delays[retry < MCP_WS_BACKOFF_STEPS
                               ? retry
                               : MCP_WS_BACKOFF_STEPS - 1];
    if (base > CONFIG_MCP_WS_MAX_BACKOFF_MS) base = CONFIG_MCP_WS_MAX_BACKOFF_MS;
    uint32_t spread = base / 10;
    int32_t jitter = spread == 0
                         ? 0
                         : (int32_t)(esp_random() % (spread * 2 + 1)) -
                               (int32_t)spread;
    uint32_t delay = (uint32_t)((int32_t)base + jitter);
    bridge_set_state_locked(MCP_WS_BACKOFF);
    xSemaphoreGive(s_bridge.lock);
    bridge_stop_timer(s_bridge.reconnect_timer);
    esp_timer_start_once(s_bridge.reconnect_timer, (uint64_t)delay * 1000);
}

/* ── Client destroy ─────────────────────────────────────────────────── */

void bridge_destroy_client(void)
{
    esp_websocket_client_handle_t client = NULL;
    bridge_log_memory_snapshot("before_destroy");
    xSemaphoreTake(s_bridge.lock, portMAX_DELAY);
    client = s_bridge.client;
    s_bridge.client = NULL;
    bridge_invalidate_connection_locked();
    xSemaphoreGive(s_bridge.lock);
    bridge_stop_timer(s_bridge.handshake_timer);
    if (client != NULL) {
        esp_websocket_client_stop(client);
        esp_websocket_client_destroy(client);
    }
    bridge_log_memory_snapshot("after_destroy");
}

/* ── WiFi event handler ─────────────────────────────────────────────── */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;
    if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xSemaphoreTake(s_bridge.lock, portMAX_DELAY);
        s_bridge.network_up = true;
        bool connect = s_bridge.started && s_bridge.config.enabled &&
                       s_bridge.config.endpoint[0] != '\0';
        xSemaphoreGive(s_bridge.lock);
        if (connect) {
            bridge_event_t event = {.type = BRIDGE_EVENT_CONNECT};
            bridge_queue_event(&event);
        }
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xSemaphoreTake(s_bridge.lock, portMAX_DELAY);
        s_bridge.network_up = false;
        bridge_invalidate_connection_locked();
        bridge_set_state_locked(s_bridge.config.enabled ? MCP_WS_WAIT_NETWORK
                                                        : MCP_WS_DISABLED);
        xSemaphoreGive(s_bridge.lock);
        bridge_event_t event = {.type = BRIDGE_EVENT_NETWORK_DOWN};
        bridge_queue_event(&event);
    }
}

/* ── Bridge task ────────────────────────────────────────────────────── */

static void bridge_task(void *arg)
{
    (void)arg;
    bridge_event_t event;
    for (;;) {
        if (xQueueReceive(s_bridge.queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (event.type) {
        case BRIDGE_EVENT_CONNECT: {
            esp_err_t result = mcp_ws_connect_client();
            if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
                bridge_schedule_reconnect();
            }
            break;
        }
        case BRIDGE_EVENT_NETWORK_DOWN:
            bridge_stop_timer(s_bridge.reconnect_timer);
            bridge_destroy_client();
            xSemaphoreTake(s_bridge.lock, portMAX_DELAY);
            bridge_set_state_locked(s_bridge.config.enabled ? MCP_WS_WAIT_NETWORK
                                                            : MCP_WS_DISABLED);
            xSemaphoreGive(s_bridge.lock);
            break;
        case BRIDGE_EVENT_WS_CONNECTED:
            mcp_ws_handle_connected(&event);
            break;
        case BRIDGE_EVENT_WS_DISCONNECTED:
            mcp_ws_handle_disconnected(&event);
            break;
        case BRIDGE_EVENT_RX_MESSAGE:
            mcp_ws_handle_rx_message(&event);
            break;
        case BRIDGE_EVENT_TX_MESSAGE:
            mcp_ws_handle_tx_message(&event);
            break;
        case BRIDGE_EVENT_RELOAD:
            bridge_stop_timer(s_bridge.reconnect_timer);
            bridge_destroy_client();
            xSemaphoreTake(s_bridge.lock, portMAX_DELAY);
            if (!s_bridge.started || !s_bridge.config.enabled) {
                bridge_set_state_locked(MCP_WS_DISABLED);
            } else if (!s_bridge.network_up ||
                       s_bridge.config.endpoint[0] == '\0') {
                bridge_set_state_locked(MCP_WS_WAIT_NETWORK);
            }
            bool reconnect = s_bridge.started && s_bridge.network_up &&
                             s_bridge.config.enabled &&
                             s_bridge.config.endpoint[0] != '\0';
            s_bridge.status.retry_count = 0;
            xSemaphoreGive(s_bridge.lock);
            if (reconnect) {
                if (CONFIG_MCP_WS_RELOAD_COOLDOWN_MS == 0) {
                    bridge_event_t connect = {.type = BRIDGE_EVENT_CONNECT};
                    bridge_queue_event(&connect);
                } else {
                    esp_timer_start_once(
                        s_bridge.reconnect_timer,
                        CONFIG_MCP_WS_RELOAD_COOLDOWN_MS * 1000ULL);
                }
            }
            break;
        case BRIDGE_EVENT_STOP:
            bridge_stop_timer(s_bridge.reconnect_timer);
            bridge_destroy_client();
            xSemaphoreTake(s_bridge.lock, portMAX_DELAY);
            bridge_set_state_locked(MCP_WS_DISABLED);
            xSemaphoreGive(s_bridge.lock);
            break;
        case BRIDGE_EVENT_HANDSHAKE_TIMEOUT:
            xSemaphoreTake(s_bridge.lock, portMAX_DELAY);
            bool timed_out = s_bridge.status.state == MCP_WS_HANDSHAKING;
            if (timed_out) s_bridge.status.last_error = ESP_ERR_TIMEOUT;
            xSemaphoreGive(s_bridge.lock);
            if (timed_out) mcp_ws_handle_disconnected(&event);
            break;
        }
        gw_mem_free(event.payload);
    }
}

/* ── Public API ─────────────────────────────────────────────────────── */

bool mcp_ws_bridge_is_supported(void) { return true; }

esp_err_t mcp_ws_bridge_init(void)
{
    if (s_bridge.initialized) return ESP_ERR_INVALID_STATE;
    memset(&s_bridge, 0, sizeof(s_bridge));

    s_bridge.lock = xSemaphoreCreateMutex();
    if (s_bridge.lock == NULL) goto fail;

    s_bridge.queue = xQueueCreate(CONFIG_MCP_WS_EVENT_QUEUE_DEPTH,
                                  sizeof(bridge_event_t));
    if (s_bridge.queue == NULL) goto fail;

    s_bridge.rx_buffer = gw_mem_alloc(CONFIG_MCP_WS_MAX_RX_MESSAGE + 1,
                                      GW_MEM_EXTERNAL_PREFERRED);
    if (s_bridge.rx_buffer == NULL) goto fail;

    esp_err_t result = mcp_ws_config_load(&s_bridge.config);
    if (result != ESP_OK) goto fail;

    s_bridge.status.enabled = s_bridge.config.enabled;
    s_bridge.status.runtime_enabled = true;
    s_bridge.status.restart_required = false;
    s_bridge.status.endpoint_configured = s_bridge.config.endpoint[0] != '\0';
    s_bridge.status.state = MCP_WS_DISABLED;
    s_bridge.network_up = wifi_prov_is_connected();

    esp_log_level_set("websocket_client", ESP_LOG_NONE);

    const esp_timer_create_args_t reconnect_args = {
        .callback = timer_queue_callback,
        .arg = (void *)(intptr_t)BRIDGE_EVENT_CONNECT,
        .name = "mcp_ws_retry",
    };
    const esp_timer_create_args_t handshake_args = {
        .callback = timer_queue_callback,
        .arg = (void *)(intptr_t)BRIDGE_EVENT_HANDSHAKE_TIMEOUT,
        .name = "mcp_ws_handshake",
    };
    if (esp_timer_create(&reconnect_args, &s_bridge.reconnect_timer) != ESP_OK ||
        esp_timer_create(&handshake_args, &s_bridge.handshake_timer) != ESP_OK) {
        goto fail;
    }

    result = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        wifi_event_handler, NULL);
    if (result != ESP_OK) goto fail;
    result = esp_event_handler_register(WIFI_EVENT,
                                        WIFI_EVENT_STA_DISCONNECTED,
                                        wifi_event_handler, NULL);
    if (result != ESP_OK) {
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                     wifi_event_handler);
        goto fail;
    }

    if (xTaskCreate(bridge_task, "mcp_ws_bridge", CONFIG_MCP_WS_TASK_STACK,
                    NULL, tskIDLE_PRIORITY + 4, &s_bridge.task) != pdPASS) {
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                     wifi_event_handler);
        esp_event_handler_unregister(WIFI_EVENT,
                                     WIFI_EVENT_STA_DISCONNECTED,
                                     wifi_event_handler);
        goto fail;
    }

    s_bridge.initialized = true;
    bridge_log_memory_snapshot("bridge_init");
    return ESP_OK;

fail:
    if (s_bridge.rx_buffer != NULL) { gw_mem_free(s_bridge.rx_buffer); s_bridge.rx_buffer = NULL; }
    if (s_bridge.queue != NULL) { vQueueDelete(s_bridge.queue); s_bridge.queue = NULL; }
    if (s_bridge.lock != NULL) { vSemaphoreDelete(s_bridge.lock); s_bridge.lock = NULL; }
    if (s_bridge.reconnect_timer != NULL) { esp_timer_delete(s_bridge.reconnect_timer); s_bridge.reconnect_timer = NULL; }
    if (s_bridge.handshake_timer != NULL) { esp_timer_delete(s_bridge.handshake_timer); s_bridge.handshake_timer = NULL; }
    return ESP_ERR_NO_MEM;
}

esp_err_t mcp_ws_bridge_start(void)
{
    if (!s_bridge.initialized) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_bridge.lock, portMAX_DELAY);
    s_bridge.started = true;
    bool connect = s_bridge.config.enabled && s_bridge.network_up &&
                   s_bridge.config.endpoint[0] != '\0';
    bridge_set_state_locked(!s_bridge.config.enabled
                                 ? MCP_WS_DISABLED
                                 : (connect ? MCP_WS_CONNECTING : MCP_WS_WAIT_NETWORK));
    xSemaphoreGive(s_bridge.lock);
    if (connect) {
        bridge_event_t event = {.type = BRIDGE_EVENT_CONNECT};
        if (!bridge_queue_event(&event)) return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t mcp_ws_bridge_stop(void)
{
    if (!s_bridge.initialized) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_bridge.lock, portMAX_DELAY);
    s_bridge.started = false;
    bridge_invalidate_connection_locked();
    bridge_set_state_locked(MCP_WS_DISABLED);
    xSemaphoreGive(s_bridge.lock);
    bridge_event_t event = {.type = BRIDGE_EVENT_STOP};
    return bridge_queue_event(&event) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t mcp_ws_bridge_reload(void)
{
    if (!s_bridge.initialized) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_bridge.lock, portMAX_DELAY);
    bridge_invalidate_connection_locked();
    bool reconnect = s_bridge.started && s_bridge.network_up &&
                     s_bridge.config.enabled &&
                     s_bridge.config.endpoint[0] != '\0';
    bridge_set_state_locked(!s_bridge.config.enabled
                                 ? MCP_WS_DISABLED
                                 : (reconnect ? MCP_WS_CONNECTING
                                              : MCP_WS_WAIT_NETWORK));
    xSemaphoreGive(s_bridge.lock);
    bridge_event_t event = {.type = BRIDGE_EVENT_RELOAD};
    return bridge_queue_event(&event) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t mcp_ws_bridge_get_status(mcp_ws_status_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (!s_bridge.initialized) {
        mcp_ws_config_t config = {0};
        esp_err_t result = mcp_ws_config_load(&config);
        if (result != ESP_OK) return result;
        out->enabled = config.enabled;
        out->runtime_enabled = false;
        out->restart_required = config.enabled;
        out->endpoint_configured = config.endpoint[0] != '\0';
        out->state = MCP_WS_DISABLED;
        memset(&config, 0, sizeof(config));
        return ESP_OK;
    }
    xSemaphoreTake(s_bridge.lock, portMAX_DELAY);
    *out = s_bridge.status;
    out->enabled = s_bridge.config.enabled;
    out->runtime_enabled = s_bridge.status.runtime_enabled;
    out->restart_required = out->enabled != out->runtime_enabled;
    out->endpoint_configured = s_bridge.config.endpoint[0] != '\0';
    xSemaphoreGive(s_bridge.lock);
    return ESP_OK;
}

const char *mcp_ws_bridge_state_name(mcp_ws_state_t state)
{
    switch (state) {
    case MCP_WS_DISABLED: return "disabled";
    case MCP_WS_WAIT_NETWORK: return "wait_network";
    case MCP_WS_CONNECTING: return "connecting";
    case MCP_WS_HANDSHAKING: return "handshaking";
    case MCP_WS_READY: return "connected";
    case MCP_WS_BACKOFF: return "backoff";
    case MCP_WS_ERROR: return "error";
    }
    return "error";
}

#endif // CONFIG_MCP_WS_BRIDGE
