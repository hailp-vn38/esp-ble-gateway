#include "mcp_ws_bridge_internal.h"

#ifdef CONFIG_MCP_WS_BRIDGE

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"

#include "memory_policy.h"
#include "mcp_core.h"

static const char *TAG = "mcp_ws_bridge";

#define MCP_WS_TX_TIMEOUT_MS 5000
#define MCP_WS_STABLE_MS     30000

/* ── Endpoint helpers ───────────────────────────────────────────────── */

bool mcp_ws_endpoint_valid(const char *endpoint)
{
    if (endpoint == NULL) return false;
    size_t len = strnlen(endpoint, MCP_WS_ENDPOINT_MAX_LEN);
    if (len == 0 || len >= MCP_WS_ENDPOINT_MAX_LEN) return false;
    bool secure = strncmp(endpoint, "wss://", 6) == 0;
    bool insecure = strncmp(endpoint, "ws://", 5) == 0;
#ifdef CONFIG_MCP_WS_ALLOW_INSECURE
    bool allow_insecure = true;
#else
    bool allow_insecure = false;
#endif
    if (!secure && !(allow_insecure && insecure)) return false;
    const char *host = endpoint + (secure ? 6 : 5);
    if (*host == '\0' || *host == '/' || *host == '?' || *host == '#') {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)endpoint; *p; p++) {
        if (*p < 0x20 || *p == 0x7f) return false;
        if (*p == '%' && (!isxdigit(p[1]) || !isxdigit(p[2]))) return false;
    }
    return true;
}

void mcp_ws_endpoint_display(const char *endpoint, char *out, size_t out_size)
{
    if (out_size == 0) return;
    out[0] = '\0';
    if (endpoint == NULL || endpoint[0] == '\0') return;
    const char *query = strchr(endpoint, '?');
    size_t prefix = query != NULL ? (size_t)(query - endpoint) : strlen(endpoint);
    const char suffix[] = "?...****";
    size_t suffix_len = query != NULL ? sizeof(suffix) - 1 : 0;
    if (prefix + suffix_len >= out_size) {
        prefix = out_size > suffix_len + 1 ? out_size - suffix_len - 1 : 0;
    }
    if (prefix > 0) memcpy(out, endpoint, prefix);
    if (suffix_len > 0 && prefix + suffix_len < out_size) {
        memcpy(out + prefix, suffix, suffix_len);
        prefix += suffix_len;
    }
    out[prefix] = '\0';
}

/* ── WebSocket event handler ────────────────────────────────────────── */

void mcp_ws_websocket_event_handler(void *handler_args, esp_event_base_t base,
                                    int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_websocket_event_data_t *data = event_data;
    bridge_event_t event = {
        .client = data != NULL ? data->client : NULL,
    };
    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        event.type = BRIDGE_EVENT_WS_CONNECTED;
        bridge_queue_event(&event);
        return;
    }
    if (event_id == WEBSOCKET_EVENT_ERROR) {
        event.type = BRIDGE_EVENT_WS_DISCONNECTED;
        if (data != NULL) {
            event.error = data->error_handle.esp_tls_last_esp_err != ESP_OK
                              ? data->error_handle.esp_tls_last_esp_err
                              : data->error_handle.esp_transport_sock_errno;
            event.http_status = data->error_handle.esp_ws_handshake_status_code;
        }
        bridge_queue_event(&event);
        return;
    }
    if (event_id == WEBSOCKET_EVENT_DISCONNECTED ||
        event_id == WEBSOCKET_EVENT_CLOSED) {
        event.type = BRIDGE_EVENT_WS_DISCONNECTED;
        event.close_code = data != NULL ? data->close_status_code : 0;
        bridge_queue_event(&event);
        return;
    }
    if (event_id != WEBSOCKET_EVENT_DATA || data == NULL) return;
    if (data->op_code == WS_TRANSPORT_OPCODES_CLOSE ||
        data->op_code == WS_TRANSPORT_OPCODES_PING ||
        data->op_code == WS_TRANSPORT_OPCODES_PONG) {
        return;
    }

    xSemaphoreTake(s_bridge.lock, portMAX_DELAY);
    if (data->client != s_bridge.client) {
        xSemaphoreGive(s_bridge.lock);
        return;
    }
    bool start = data->op_code == WS_TRANSPORT_OPCODES_TEXT &&
                 data->payload_offset == 0;
    bool continuation = data->op_code == WS_TRANSPORT_OPCODES_CONT;
    if (start) {
        if (s_bridge.rx_active) {
            s_bridge.rx_length = 0;
            s_bridge.rx_active = false;
            s_bridge.rx_discard = false;
        }
        s_bridge.rx_active = true;
    } else if (!continuation && data->op_code == WS_TRANSPORT_OPCODES_BINARY) {
        s_bridge.rx_length = 0;
        s_bridge.rx_active = false;
        s_bridge.rx_discard = false;
        xSemaphoreGive(s_bridge.lock);
        ESP_LOGW(TAG, "Ignoring binary WebSocket message");
        return;
    } else if (!s_bridge.rx_active) {
        xSemaphoreGive(s_bridge.lock);
        return;
    }

    if (data->payload_offset == 0 && data->payload_len > 0 &&
        s_bridge.rx_length + (size_t)data->payload_len >
            CONFIG_MCP_WS_MAX_RX_MESSAGE) {
        s_bridge.rx_discard = true;
    }
    if (!s_bridge.rx_discard && data->data_len > 0) {
        if (s_bridge.rx_length + (size_t)data->data_len >
            CONFIG_MCP_WS_MAX_RX_MESSAGE) {
            s_bridge.rx_discard = true;
        } else {
            memcpy(s_bridge.rx_buffer + s_bridge.rx_length, data->data_ptr,
                   (size_t)data->data_len);
            s_bridge.rx_length += (size_t)data->data_len;
        }
    }
    bool frame_complete = data->payload_offset + data->data_len >= data->payload_len;
    bool message_complete = frame_complete && data->fin;
    if (message_complete) {
        if (!s_bridge.rx_discard) {
            char *message = gw_mem_alloc(s_bridge.rx_length + 1,
                                        GW_MEM_EXTERNAL_PREFERRED);
            if (message != NULL) {
                memcpy(message, s_bridge.rx_buffer, s_bridge.rx_length);
                message[s_bridge.rx_length] = '\0';
                bridge_event_t rx = {
                    .type = BRIDGE_EVENT_RX_MESSAGE,
                    .client = data->client,
                    .generation = s_bridge.status.generation,
                    .payload = message,
                    .payload_len = s_bridge.rx_length,
                };
                if (!bridge_queue_event(&rx)) gw_mem_free(message);
            }
        } else {
            ESP_LOGW(TAG, "Dropped oversized WebSocket message");
        }
        s_bridge.rx_length = 0;
        s_bridge.rx_active = false;
        s_bridge.rx_discard = false;
    }
    xSemaphoreGive(s_bridge.lock);
}

/* ── WebSocket client connect ───────────────────────────────────────── */

esp_err_t mcp_ws_connect_client(void)
{
    char endpoint[MCP_WS_ENDPOINT_MAX_LEN];
    xSemaphoreTake(s_bridge.lock, portMAX_DELAY);
    if (!s_bridge.started || !s_bridge.network_up || !s_bridge.config.enabled ||
        s_bridge.config.endpoint[0] == '\0' || s_bridge.client != NULL) {
        xSemaphoreGive(s_bridge.lock);
        return ESP_ERR_INVALID_STATE;
    }
    strlcpy(endpoint, s_bridge.config.endpoint, sizeof(endpoint));
    bridge_set_state_locked(MCP_WS_CONNECTING);
    xSemaphoreGive(s_bridge.lock);

    char display[MCP_WS_ENDPOINT_DISPLAY_MAX_LEN];
    mcp_ws_endpoint_display(endpoint, display, sizeof(display));
    ESP_LOGI(TAG, "Connecting external MCP endpoint %s", display);
    bridge_log_memory_snapshot("before_connect");
    const esp_websocket_client_config_t config = {
        .uri = endpoint,
        .disable_auto_reconnect = true,
        .enable_close_reconnect = false,
        .user_context = &s_bridge,
        .task_name = "mcp_ws_client",
        .task_stack = 6144,
        .buffer_size = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .skip_cert_common_name_check = false,
        .network_timeout_ms = 10000,
        .ping_interval_sec = 20,
        .pingpong_timeout_sec = 60,
        .keep_alive_enable = true,
        .keep_alive_idle = 15,
        .keep_alive_interval = 5,
        .keep_alive_count = 3,
    };
    esp_websocket_client_handle_t client = esp_websocket_client_init(&config);
    memset(endpoint, 0, sizeof(endpoint));
    if (client == NULL) return ESP_ERR_NO_MEM;
    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY,
                                  mcp_ws_websocket_event_handler, &s_bridge);
    xSemaphoreTake(s_bridge.lock, portMAX_DELAY);
    s_bridge.client = client;
    xSemaphoreGive(s_bridge.lock);
    esp_err_t result = esp_websocket_client_start(client);
    if (result != ESP_OK) {
        bridge_destroy_client();
    }
    return result;
}

/* ── WebSocket responder ────────────────────────────────────────────── */

typedef struct {
    uint32_t generation;
    bool owned;
} ws_responder_context_t;

static bool ws_responder_is_alive(void *context)
{
    ws_responder_context_t *responder = context;
    bool alive;
    xSemaphoreTake(s_bridge.lock, portMAX_DELAY);
    alive = responder != NULL && responder->generation == s_bridge.status.generation &&
            s_bridge.client != NULL &&
            (s_bridge.status.state == MCP_WS_HANDSHAKING ||
             s_bridge.status.state == MCP_WS_READY);
    xSemaphoreGive(s_bridge.lock);
    return alive;
}

static esp_err_t ws_responder_send_json(void *context, const char *json,
                                        size_t len,
                                        const mcp_response_meta_t *meta)
{
    (void)meta;
    ws_responder_context_t *responder = context;
    if (responder == NULL || json == NULL || len == 0 ||
        len > CONFIG_MCP_WS_MAX_TX_MESSAGE ||
        !ws_responder_is_alive(context)) {
        return ESP_ERR_INVALID_STATE;
    }
    gw_mem_class_t mem_class = len > CONFIG_MCP_WS_PSRAM_TX_THRESHOLD
                                   ? GW_MEM_EXTERNAL_PREFERRED
                                   : GW_MEM_DEFAULT;
    char *copy = gw_mem_alloc(len + 1, mem_class);
    if (copy == NULL) return ESP_ERR_NO_MEM;
    memcpy(copy, json, len);
    copy[len] = '\0';
    bridge_event_t event = {
        .type = BRIDGE_EVENT_TX_MESSAGE,
        .generation = responder->generation,
        .payload = copy,
        .payload_len = len,
    };
    if (!bridge_queue_event(&event)) {
        gw_mem_free(copy);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t ws_responder_send_none(void *context,
                                        const mcp_response_meta_t *meta)
{
    (void)context;
    (void)meta;
    return ESP_OK;
}

static esp_err_t ws_responder_clone(const mcp_responder_t *source,
                                    mcp_responder_t *out)
{
    ws_responder_context_t *source_context = source->context;
    ws_responder_context_t *copy = malloc(sizeof(*copy));
    if (copy == NULL) return ESP_ERR_NO_MEM;
    *copy = *source_context;
    copy->owned = true;
    *out = *source;
    out->context = copy;
    return ESP_OK;
}

static void ws_responder_release(void *context)
{
    ws_responder_context_t *responder = context;
    if (responder != NULL && responder->owned) free(responder);
}

static mcp_responder_t make_ws_responder(ws_responder_context_t *context)
{
    const mcp_responder_t responder = {
        .context = context,
        .send_json = ws_responder_send_json,
        .send_none = ws_responder_send_none,
        .is_alive = ws_responder_is_alive,
        .clone = ws_responder_clone,
        .release = ws_responder_release,
    };
    return responder;
}

static void send_not_ready(const cJSON *id, ws_responder_context_t *context)
{
    cJSON *response = cJSON_CreateObject();
    cJSON *error = cJSON_CreateObject();
    if (response == NULL || error == NULL) {
        cJSON_Delete(response);
        cJSON_Delete(error);
        return;
    }
    cJSON_AddStringToObject(response, "jsonrpc", "2.0");
    cJSON_AddItemToObject(response, "id",
                          id != NULL ? cJSON_Duplicate(id, true)
                                     : cJSON_CreateNull());
    cJSON_AddNumberToObject(error, "code", -32002);
    cJSON_AddStringToObject(error, "message", "Server not initialized");
    cJSON_AddItemToObject(response, "error", error);
    char *text = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    if (text != NULL) {
        ws_responder_send_json(context, text, strlen(text), NULL);
        cJSON_free(text);
    }
}

/* ── RX message handling ────────────────────────────────────────────── */

void mcp_ws_handle_rx_message(bridge_event_t *event)
{
    xSemaphoreTake(s_bridge.lock, portMAX_DELAY);
    bool current = event->client == s_bridge.client &&
                   event->generation == s_bridge.status.generation;
    mcp_ws_state_t state = s_bridge.status.state;
    xSemaphoreGive(s_bridge.lock);
    if (!current) return;

    cJSON *root = cJSON_ParseWithLength(event->payload, event->payload_len);
    const char *method = root != NULL
                             ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(
                                   root, "method"))
                             : NULL;
    const cJSON *id = root != NULL
                          ? cJSON_GetObjectItemCaseSensitive(root, "id")
                          : NULL;
    bool initialize = method != NULL && strcmp(method, "initialize") == 0;
    bool initialized = method != NULL &&
                       strcmp(method, "notifications/initialized") == 0;
    bool ping = method != NULL && strcmp(method, "ping") == 0;

    ws_responder_context_t responder_context = {
        .generation = event->generation,
        .owned = false,
    };
    mcp_responder_t responder = make_ws_responder(&responder_context);
    if (root != NULL && state != MCP_WS_READY && !initialize && !initialized &&
        !ping) {
        if (id != NULL) send_not_ready(id, &responder_context);
        cJSON_Delete(root);
        return;
    }

    mcp_wire_context_t wire = {
        .transport = MCP_TRANSPORT_WS,
        .authenticated = true,
        .trusted_transport = true,
    };
    if (!initialize) {
        wire.has_protocol_version = true;
        strlcpy(wire.protocol_version, "2024-11-05",
                sizeof(wire.protocol_version));
    }
    esp_err_t result = mcp_core_handle_json(event->payload, event->payload_len,
                                            &wire, &responder);
    if (result == ESP_OK && initialize) {
        xSemaphoreTake(s_bridge.lock, portMAX_DELAY);
        if (event->generation == s_bridge.status.generation) {
            s_bridge.initialize_response_sent = true;
            strlcpy(s_bridge.status.negotiated_protocol_version, "2024-11-05",
                    sizeof(s_bridge.status.negotiated_protocol_version));
        }
        xSemaphoreGive(s_bridge.lock);
    } else if (result == ESP_OK && initialized) {
        bool became_ready = false;
        xSemaphoreTake(s_bridge.lock, portMAX_DELAY);
        if (event->generation == s_bridge.status.generation &&
            s_bridge.initialize_response_sent) {
            bridge_set_state_locked(MCP_WS_READY);
            s_bridge.status.retry_count = 0;
            bridge_stop_timer(s_bridge.handshake_timer);
            ESP_LOGI(TAG, "External MCP session ready (2024-11-05)");
            became_ready = true;
        }
        xSemaphoreGive(s_bridge.lock);
        if (became_ready) bridge_log_memory_snapshot("mcp_ready");
    }
    cJSON_Delete(root);
}

/* ── TX message handling ────────────────────────────────────────────── */

void mcp_ws_handle_tx_message(bridge_event_t *event)
{
    esp_websocket_client_handle_t client;
    mcp_ws_state_t state;
    xSemaphoreTake(s_bridge.lock, portMAX_DELAY);
    bool current = event->generation == s_bridge.status.generation &&
                   s_bridge.client != NULL &&
                   (s_bridge.status.state == MCP_WS_HANDSHAKING ||
                    s_bridge.status.state == MCP_WS_READY);
    client = s_bridge.client;
    state = s_bridge.status.state;
    xSemaphoreGive(s_bridge.lock);
    if (!current) return;

    ESP_LOGD(TAG, "WS TX len=%u generation=%u state=%s",
             (unsigned)event->payload_len, (unsigned)event->generation,
             mcp_ws_bridge_state_name(state));
    if (event->payload_len > 8192) {
        ESP_LOGW(TAG, "Large MCP WebSocket response: %u bytes",
                 (unsigned)event->payload_len);
    }
    bridge_log_memory_snapshot("before_tx");

    const TickType_t timeout = pdMS_TO_TICKS(MCP_WS_TX_TIMEOUT_MS);
    int sent = -1;
    if (!esp_websocket_client_is_connected(client)) {
        ESP_LOGW(TAG, "WebSocket disconnected before TX");
    } else if (event->payload_len <= CONFIG_MCP_WS_TX_FRAGMENT_SIZE) {
        sent = esp_websocket_client_send_text(
            client, event->payload, (int)event->payload_len, timeout);
    } else {
        size_t offset = 0;
        size_t chunk = CONFIG_MCP_WS_TX_FRAGMENT_SIZE;
        int frame_sent = esp_websocket_client_send_text_partial(
            client, event->payload, (int)chunk, timeout);
        if (frame_sent == (int)chunk) {
            offset = chunk;
            sent = frame_sent;
        } else {
            sent = frame_sent;
        }

        while (offset > 0 && offset < event->payload_len) {
            size_t remaining = event->payload_len - offset;
            chunk = remaining < CONFIG_MCP_WS_TX_FRAGMENT_SIZE
                        ? remaining
                        : CONFIG_MCP_WS_TX_FRAGMENT_SIZE;
            frame_sent = esp_websocket_client_send_cont_msg(
                client, event->payload + offset, (int)chunk, timeout);
            if (frame_sent != (int)chunk) {
                sent = frame_sent < 0 ? frame_sent : sent + frame_sent;
                offset = 0;
                break;
            }
            offset += chunk;
            sent += frame_sent;
        }
        if (offset == event->payload_len &&
            esp_websocket_client_send_fin(client, timeout) < 0) {
            sent = -1;
        }
    }

    if (sent != (int)event->payload_len) {
        ESP_LOGW(TAG, "WebSocket TX failed: sent=%d expected=%u", sent,
                 (unsigned)event->payload_len);
        bridge_log_memory_snapshot("tx_fail");

        xSemaphoreTake(s_bridge.lock, portMAX_DELAY);
        bool still_current = event->generation == s_bridge.status.generation &&
                             client == s_bridge.client;
        if (still_current) {
            s_bridge.status.last_error = ESP_FAIL;
            bridge_invalidate_connection_locked();
        }
        xSemaphoreGive(s_bridge.lock);
        if (still_current) {
            bridge_destroy_client();
            bridge_schedule_reconnect();
        }
        return;
    }
    bridge_log_memory_snapshot("after_tx");
}

/* ── Connection state handlers ──────────────────────────────────────── */

void mcp_ws_handle_connected(const bridge_event_t *event)
{
    xSemaphoreTake(s_bridge.lock, portMAX_DELAY);
    if (event->client != s_bridge.client) {
        xSemaphoreGive(s_bridge.lock);
        return;
    }
    s_bridge.status.generation++;
    s_bridge.initialize_response_sent = false;
    s_bridge.status.negotiated_protocol_version[0] = '\0';
    s_bridge.rx_length = 0;
    s_bridge.rx_active = false;
    s_bridge.rx_discard = false;
    bridge_set_state_locked(MCP_WS_HANDSHAKING);
    s_bridge.status.last_error = 0;
    s_bridge.status.last_http_status = 0;
    s_bridge.status.last_ws_close_code = 0;
    s_bridge.connected_at_us = esp_timer_get_time();
    xSemaphoreGive(s_bridge.lock);
    bridge_stop_timer(s_bridge.handshake_timer);
    esp_timer_start_once(s_bridge.handshake_timer,
                         CONFIG_MCP_WS_HANDSHAKE_TIMEOUT_MS * 1000ULL);
    ESP_LOGI(TAG, "WebSocket connected; waiting for MCP initialize");
    bridge_log_memory_snapshot("ws_connected");
}

void mcp_ws_handle_disconnected(const bridge_event_t *event)
{
    xSemaphoreTake(s_bridge.lock, portMAX_DELAY);
    if (event->client != NULL && event->client != s_bridge.client) {
        xSemaphoreGive(s_bridge.lock);
        return;
    }
    s_bridge.status.last_error = event->error;
    s_bridge.status.last_http_status = event->http_status;
    s_bridge.status.last_ws_close_code = event->close_code;
    if (s_bridge.connected_at_us > 0 &&
        esp_timer_get_time() - s_bridge.connected_at_us >=
            MCP_WS_STABLE_MS * 1000LL) {
        s_bridge.status.retry_count = 0;
    }
    xSemaphoreGive(s_bridge.lock);
    bridge_destroy_client();
    bridge_schedule_reconnect();
}

#endif /* CONFIG_MCP_WS_BRIDGE */
