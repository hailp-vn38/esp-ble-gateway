#include "mcp_ws_bridge.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
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
#include "nvs.h"
#include "nvs_flash.h"

#include "mcp_core.h"
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

#define MCP_WS_NVS_NAMESPACE "mcp_ws"
#define MCP_WS_NVS_ENABLED   "enabled"
#define MCP_WS_NVS_ENDPOINT  "endpoint"
#define MCP_WS_TX_TIMEOUT_MS 5000
#define MCP_WS_STABLE_MS     30000
#define MCP_WS_BACKOFF_STEPS 7

static const char *TAG = "mcp_ws_bridge";

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

typedef struct {
    uint32_t generation;
    bool owned;
} ws_responder_context_t;

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

static bridge_state_t s_bridge;

static bool default_enabled(void)
{
#ifdef CONFIG_MCP_WS_DEFAULT_ENABLED
    return true;
#else
    return false;
#endif
}

bool mcp_ws_bridge_is_supported(void) { return true; }

static bool queue_event(const bridge_event_t *event)
{
    return s_bridge.queue != NULL &&
           xQueueSend(s_bridge.queue, event, 0) == pdTRUE;
}

static bool endpoint_valid(const char *endpoint)
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

static void endpoint_display(const char *endpoint, char *out, size_t out_size)
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

static esp_err_t config_load(mcp_ws_config_t *config)
{
    memset(config, 0, sizeof(*config));
    nvs_handle_t nvs;
    esp_err_t result = nvs_open(MCP_WS_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        config->enabled = default_enabled();
        return ESP_OK;
    }
    if (result != ESP_OK) return result;
    uint8_t enabled = 0;
    bool has_enabled_key =
        nvs_get_u8(nvs, MCP_WS_NVS_ENABLED, &enabled) == ESP_OK;
    size_t size = sizeof(config->endpoint);
    esp_err_t endpoint_result =
        nvs_get_str(nvs, MCP_WS_NVS_ENDPOINT, config->endpoint, &size);
    if (endpoint_result != ESP_OK && endpoint_result != ESP_ERR_NVS_NOT_FOUND) {
        result = endpoint_result;
    }
    nvs_close(nvs);
    config->enabled = has_enabled_key ? (enabled != 0) : default_enabled();
    if (config->endpoint[0] != '\0' && !endpoint_valid(config->endpoint)) {
        ESP_LOGW(TAG, "Stored endpoint is invalid; bridge disabled");
        config->enabled = false;
        config->endpoint[0] = '\0';
    }
    return result;
}

static esp_err_t config_store(const mcp_ws_config_t *config)
{
    nvs_handle_t nvs;
    esp_err_t result = nvs_open(MCP_WS_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (result != ESP_OK) return result;
    result = nvs_set_u8(nvs, MCP_WS_NVS_ENABLED, config->enabled ? 1 : 0);
    if (result == ESP_OK) {
        result = config->endpoint[0] != '\0'
                     ? nvs_set_str(nvs, MCP_WS_NVS_ENDPOINT, config->endpoint)
                     : nvs_erase_key(nvs, MCP_WS_NVS_ENDPOINT);
        if (result == ESP_ERR_NVS_NOT_FOUND) result = ESP_OK;
    }
    if (result == ESP_OK) result = nvs_commit(nvs);
    nvs_close(nvs);
    return result;
}

static void set_state_locked(mcp_ws_state_t state)
{
    s_bridge.status.state = state;
}

static void reset_protocol_locked(void)
{
    s_bridge.initialize_response_sent = false;
    s_bridge.status.negotiated_protocol_version[0] = '\0';
}

static void reset_rx_locked(void)
{
    s_bridge.rx_length = 0;
    s_bridge.rx_active = false;
    s_bridge.rx_discard = false;
}

static void invalidate_connection_locked(void)
{
    s_bridge.status.generation++;
    reset_protocol_locked();
    reset_rx_locked();
}

static void timer_queue_callback(void *arg)
{
    bridge_event_t event = {.type = (bridge_event_type_t)(intptr_t)arg};
    queue_event(&event);
}

static void stop_timer(esp_timer_handle_t timer)
{
    if (timer != NULL && esp_timer_is_active(timer)) esp_timer_stop(timer);
}

static void schedule_reconnect(void)
{
    static const uint32_t delays[] = {1000, 2000, 4000, 8000,
                                      16000, 30000, 60000};
    xSemaphoreTake(s_bridge.lock, portMAX_DELAY);
    if (!s_bridge.started || !s_bridge.config.enabled ||
        s_bridge.config.endpoint[0] == '\0' || !s_bridge.network_up) {
        set_state_locked(s_bridge.config.enabled ? MCP_WS_WAIT_NETWORK
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
    set_state_locked(MCP_WS_BACKOFF);
    xSemaphoreGive(s_bridge.lock);
    stop_timer(s_bridge.reconnect_timer);
    esp_timer_start_once(s_bridge.reconnect_timer, (uint64_t)delay * 1000);
}

static void destroy_client(void)
{
    esp_websocket_client_handle_t client = NULL;
    xSemaphoreTake(s_bridge.lock, portMAX_DELAY);
    client = s_bridge.client;
    s_bridge.client = NULL;
    invalidate_connection_locked();
    xSemaphoreGive(s_bridge.lock);
    stop_timer(s_bridge.handshake_timer);
    if (client != NULL) {
        esp_websocket_client_stop(client);
        esp_websocket_client_destroy(client);
    }
}

static void websocket_event_handler(void *handler_args, esp_event_base_t base,
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
        queue_event(&event);
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
        queue_event(&event);
        return;
    }
    if (event_id == WEBSOCKET_EVENT_DISCONNECTED ||
        event_id == WEBSOCKET_EVENT_CLOSED) {
        event.type = BRIDGE_EVENT_WS_DISCONNECTED;
        event.close_code = data != NULL ? data->close_status_code : 0;
        queue_event(&event);
        return;
    }
    if (event_id != WEBSOCKET_EVENT_DATA || data == NULL) return;
    if (data->op_code == WS_TRANSPORT_OPCODES_CLOSE ||
        data->op_code == WS_TRANSPORT_OPCODES_PING ||
        data->op_code == WS_TRANSPORT_OPCODES_PONG) {
        return;
    }

    xSemaphoreTake(s_bridge.lock, portMAX_DELAY);
    bool start = data->op_code == WS_TRANSPORT_OPCODES_TEXT &&
                 data->payload_offset == 0;
    bool continuation = data->op_code == WS_TRANSPORT_OPCODES_CONT;
    if (start) {
        if (s_bridge.rx_active) reset_rx_locked();
        s_bridge.rx_active = true;
    } else if (!continuation && data->op_code == WS_TRANSPORT_OPCODES_BINARY) {
        reset_rx_locked();
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
            char *message = malloc(s_bridge.rx_length + 1);
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
                if (!queue_event(&rx)) free(message);
            }
        } else {
            ESP_LOGW(TAG, "Dropped oversized WebSocket message");
        }
        reset_rx_locked();
    }
    xSemaphoreGive(s_bridge.lock);
}

static esp_err_t connect_client(void)
{
    char endpoint[MCP_WS_ENDPOINT_MAX_LEN];
    xSemaphoreTake(s_bridge.lock, portMAX_DELAY);
    if (!s_bridge.started || !s_bridge.network_up || !s_bridge.config.enabled ||
        s_bridge.config.endpoint[0] == '\0' || s_bridge.client != NULL) {
        xSemaphoreGive(s_bridge.lock);
        return ESP_ERR_INVALID_STATE;
    }
    strlcpy(endpoint, s_bridge.config.endpoint, sizeof(endpoint));
    set_state_locked(MCP_WS_CONNECTING);
    xSemaphoreGive(s_bridge.lock);

    char display[MCP_WS_ENDPOINT_DISPLAY_MAX_LEN];
    endpoint_display(endpoint, display, sizeof(display));
    ESP_LOGI(TAG, "Connecting external MCP endpoint %s", display);
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
                                  websocket_event_handler, &s_bridge);
    xSemaphoreTake(s_bridge.lock, portMAX_DELAY);
    s_bridge.client = client;
    xSemaphoreGive(s_bridge.lock);
    esp_err_t result = esp_websocket_client_start(client);
    if (result != ESP_OK) {
        destroy_client();
    }
    return result;
}

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
    char *copy = malloc(len + 1);
    if (copy == NULL) return ESP_ERR_NO_MEM;
    memcpy(copy, json, len);
    copy[len] = '\0';
    bridge_event_t event = {
        .type = BRIDGE_EVENT_TX_MESSAGE,
        .generation = responder->generation,
        .payload = copy,
        .payload_len = len,
    };
    if (!queue_event(&event)) {
        free(copy);
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

static void handle_rx_message(bridge_event_t *event)
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
        xSemaphoreTake(s_bridge.lock, portMAX_DELAY);
        if (event->generation == s_bridge.status.generation &&
            s_bridge.initialize_response_sent) {
            set_state_locked(MCP_WS_READY);
            s_bridge.status.retry_count = 0;
            stop_timer(s_bridge.handshake_timer);
            ESP_LOGI(TAG, "External MCP session ready (2024-11-05)");
        }
        xSemaphoreGive(s_bridge.lock);
    }
    cJSON_Delete(root);
}

static void handle_tx_message(bridge_event_t *event)
{
    esp_websocket_client_handle_t client;
    xSemaphoreTake(s_bridge.lock, portMAX_DELAY);
    bool current = event->generation == s_bridge.status.generation &&
                   s_bridge.client != NULL &&
                   (s_bridge.status.state == MCP_WS_HANDSHAKING ||
                    s_bridge.status.state == MCP_WS_READY);
    client = s_bridge.client;
    xSemaphoreGive(s_bridge.lock);
    if (!current || !esp_websocket_client_is_connected(client)) return;
    int sent = esp_websocket_client_send_text(
        client, event->payload, (int)event->payload_len,
        pdMS_TO_TICKS(MCP_WS_TX_TIMEOUT_MS));
    if (sent != (int)event->payload_len) {
        ESP_LOGW(TAG, "WebSocket TX failed");
    }
}

static void handle_connected(const bridge_event_t *event)
{
    xSemaphoreTake(s_bridge.lock, portMAX_DELAY);
    if (event->client != s_bridge.client) {
        xSemaphoreGive(s_bridge.lock);
        return;
    }
    s_bridge.status.generation++;
    reset_protocol_locked();
    reset_rx_locked();
    set_state_locked(MCP_WS_HANDSHAKING);
    s_bridge.status.last_error = 0;
    s_bridge.status.last_http_status = 0;
    s_bridge.status.last_ws_close_code = 0;
    s_bridge.connected_at_us = esp_timer_get_time();
    xSemaphoreGive(s_bridge.lock);
    stop_timer(s_bridge.handshake_timer);
    esp_timer_start_once(s_bridge.handshake_timer,
                         CONFIG_MCP_WS_HANDSHAKE_TIMEOUT_MS * 1000ULL);
    ESP_LOGI(TAG, "WebSocket connected; waiting for MCP initialize");
}

static void handle_disconnected(const bridge_event_t *event)
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
    destroy_client();
    schedule_reconnect();
}

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
            esp_err_t result = connect_client();
            if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
                schedule_reconnect();
            }
            break;
        }
        case BRIDGE_EVENT_NETWORK_DOWN:
            stop_timer(s_bridge.reconnect_timer);
            destroy_client();
            xSemaphoreTake(s_bridge.lock, portMAX_DELAY);
            set_state_locked(s_bridge.config.enabled ? MCP_WS_WAIT_NETWORK
                                                    : MCP_WS_DISABLED);
            xSemaphoreGive(s_bridge.lock);
            break;
        case BRIDGE_EVENT_WS_CONNECTED:
            handle_connected(&event);
            break;
        case BRIDGE_EVENT_WS_DISCONNECTED:
            handle_disconnected(&event);
            break;
        case BRIDGE_EVENT_RX_MESSAGE:
            handle_rx_message(&event);
            break;
        case BRIDGE_EVENT_TX_MESSAGE:
            handle_tx_message(&event);
            break;
        case BRIDGE_EVENT_RELOAD:
            stop_timer(s_bridge.reconnect_timer);
            destroy_client();
            xSemaphoreTake(s_bridge.lock, portMAX_DELAY);
            if (!s_bridge.started || !s_bridge.config.enabled) {
                set_state_locked(MCP_WS_DISABLED);
            } else if (!s_bridge.network_up ||
                       s_bridge.config.endpoint[0] == '\0') {
                set_state_locked(MCP_WS_WAIT_NETWORK);
            }
            bool reconnect = s_bridge.started && s_bridge.network_up &&
                             s_bridge.config.enabled &&
                             s_bridge.config.endpoint[0] != '\0';
            s_bridge.status.retry_count = 0;
            xSemaphoreGive(s_bridge.lock);
            if (reconnect) {
                bridge_event_t connect = {.type = BRIDGE_EVENT_CONNECT};
                queue_event(&connect);
            }
            break;
        case BRIDGE_EVENT_STOP:
            stop_timer(s_bridge.reconnect_timer);
            destroy_client();
            xSemaphoreTake(s_bridge.lock, portMAX_DELAY);
            set_state_locked(MCP_WS_DISABLED);
            xSemaphoreGive(s_bridge.lock);
            break;
        case BRIDGE_EVENT_HANDSHAKE_TIMEOUT:
            xSemaphoreTake(s_bridge.lock, portMAX_DELAY);
            bool timed_out = s_bridge.status.state == MCP_WS_HANDSHAKING;
            if (timed_out) s_bridge.status.last_error = ESP_ERR_TIMEOUT;
            xSemaphoreGive(s_bridge.lock);
            if (timed_out) handle_disconnected(&event);
            break;
        }
        free(event.payload);
    }
}

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
            queue_event(&event);
        }
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xSemaphoreTake(s_bridge.lock, portMAX_DELAY);
        s_bridge.network_up = false;
        invalidate_connection_locked();
        set_state_locked(s_bridge.config.enabled ? MCP_WS_WAIT_NETWORK
                                                 : MCP_WS_DISABLED);
        xSemaphoreGive(s_bridge.lock);
        bridge_event_t event = {.type = BRIDGE_EVENT_NETWORK_DOWN};
        queue_event(&event);
    }
}

esp_err_t mcp_ws_bridge_init(void)
{
    if (s_bridge.initialized) return ESP_ERR_INVALID_STATE;
    memset(&s_bridge, 0, sizeof(s_bridge));

    s_bridge.lock = xSemaphoreCreateMutex();
    if (s_bridge.lock == NULL) goto fail;

    s_bridge.queue = xQueueCreate(CONFIG_MCP_WS_EVENT_QUEUE_DEPTH,
                                  sizeof(bridge_event_t));
    if (s_bridge.queue == NULL) goto fail;

    s_bridge.rx_buffer = malloc(CONFIG_MCP_WS_MAX_RX_MESSAGE + 1);
    if (s_bridge.rx_buffer == NULL) goto fail;

    esp_err_t result = config_load(&s_bridge.config);
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
    return ESP_OK;

fail:
    if (s_bridge.rx_buffer != NULL) { free(s_bridge.rx_buffer); s_bridge.rx_buffer = NULL; }
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
    set_state_locked(!s_bridge.config.enabled
                         ? MCP_WS_DISABLED
                         : (connect ? MCP_WS_CONNECTING : MCP_WS_WAIT_NETWORK));
    xSemaphoreGive(s_bridge.lock);
    if (connect) {
        bridge_event_t event = {.type = BRIDGE_EVENT_CONNECT};
        if (!queue_event(&event)) return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t mcp_ws_bridge_stop(void)
{
    if (!s_bridge.initialized) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_bridge.lock, portMAX_DELAY);
    s_bridge.started = false;
    invalidate_connection_locked();
    set_state_locked(MCP_WS_DISABLED);
    xSemaphoreGive(s_bridge.lock);
    bridge_event_t event = {.type = BRIDGE_EVENT_STOP};
    return queue_event(&event) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t mcp_ws_bridge_reload(void)
{
    if (!s_bridge.initialized) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_bridge.lock, portMAX_DELAY);
    invalidate_connection_locked();
    bool reconnect = s_bridge.started && s_bridge.network_up &&
                     s_bridge.config.enabled &&
                     s_bridge.config.endpoint[0] != '\0';
    set_state_locked(!s_bridge.config.enabled
                         ? MCP_WS_DISABLED
                         : (reconnect ? MCP_WS_CONNECTING
                                      : MCP_WS_WAIT_NETWORK));
    xSemaphoreGive(s_bridge.lock);
    bridge_event_t event = {.type = BRIDGE_EVENT_RELOAD};
    return queue_event(&event) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t mcp_ws_bridge_get_status(mcp_ws_status_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (!s_bridge.initialized) {
        mcp_ws_config_t config = {0};
        esp_err_t result = config_load(&config);
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
    out->runtime_enabled = true;
    out->restart_required = false;
    out->endpoint_configured = s_bridge.config.endpoint[0] != '\0';
    xSemaphoreGive(s_bridge.lock);
    return ESP_OK;
}

esp_err_t mcp_ws_bridge_config_set(const mcp_ws_config_t *config)
{
    if (config == NULL ||
        (config->endpoint[0] != '\0' && !endpoint_valid(config->endpoint)) ||
        (config->enabled && config->endpoint[0] == '\0')) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = config_store(config);
    if (result != ESP_OK) return result;
    if (s_bridge.initialized) {
        xSemaphoreTake(s_bridge.lock, portMAX_DELAY);
        s_bridge.config = *config;
        s_bridge.status.enabled = config->enabled;
        s_bridge.status.endpoint_configured = config->endpoint[0] != '\0';
        invalidate_connection_locked();
        bool reconnect = s_bridge.started && s_bridge.network_up &&
                         config->enabled && config->endpoint[0] != '\0';
        set_state_locked(!config->enabled
                             ? MCP_WS_DISABLED
                             : (reconnect ? MCP_WS_CONNECTING
                                          : MCP_WS_WAIT_NETWORK));
        xSemaphoreGive(s_bridge.lock);
        bridge_event_t event = {.type = BRIDGE_EVENT_RELOAD};
        result = queue_event(&event) ? ESP_OK : ESP_ERR_NO_MEM;
    }
    return result;
}

esp_err_t mcp_ws_bridge_config_get_public(mcp_ws_public_config_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    mcp_ws_config_t config = {0};
    if (s_bridge.initialized) {
        xSemaphoreTake(s_bridge.lock, portMAX_DELAY);
        config = s_bridge.config;
        xSemaphoreGive(s_bridge.lock);
    } else {
        esp_err_t result = config_load(&config);
        if (result != ESP_OK) return result;
    }
    memset(out, 0, sizeof(*out));
    out->enabled = config.enabled;
    out->endpoint_configured = config.endpoint[0] != '\0';
    endpoint_display(config.endpoint, out->endpoint_display,
                     sizeof(out->endpoint_display));
    memset(&config, 0, sizeof(config));
    return ESP_OK;
}

esp_err_t mcp_ws_bridge_config_update(bool has_enabled, bool enabled,
                                      bool has_endpoint,
                                      const char *endpoint)
{
    if (has_endpoint && endpoint == NULL) return ESP_ERR_INVALID_ARG;
    if (has_endpoint &&
        strnlen(endpoint, MCP_WS_ENDPOINT_MAX_LEN) >= MCP_WS_ENDPOINT_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    if (has_endpoint && endpoint[0] != '\0' &&
        strncmp(endpoint, "ws://", 5) != 0 &&
        strncmp(endpoint, "wss://", 6) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    mcp_ws_config_t config = {0};
    if (s_bridge.initialized) {
        xSemaphoreTake(s_bridge.lock, portMAX_DELAY);
        config = s_bridge.config;
        xSemaphoreGive(s_bridge.lock);
    } else {
        esp_err_t result = config_load(&config);
        if (result != ESP_OK) return result;
    }

    bool changed_enabled = has_enabled && (enabled != config.enabled);
    if (has_enabled) config.enabled = enabled;
    if (has_endpoint) {
        strlcpy(config.endpoint, endpoint, sizeof(config.endpoint));
    }

    esp_err_t result = config_store(&config);
    if (result != ESP_OK) {
        memset(&config, 0, sizeof(config));
        return result;
    }

    if (s_bridge.initialized) {
        xSemaphoreTake(s_bridge.lock, portMAX_DELAY);
        if (has_endpoint) {
            strlcpy(s_bridge.config.endpoint, config.endpoint,
                    sizeof(s_bridge.config.endpoint));
            s_bridge.status.endpoint_configured =
                config.endpoint[0] != '\0';
        }
        if (has_enabled) {
            s_bridge.config.enabled = config.enabled;
            s_bridge.status.enabled = config.enabled;
        }
        xSemaphoreGive(s_bridge.lock);

        if (changed_enabled) {
            ESP_LOGI(TAG, "Enable state changed; restart required to apply");
        } else if (has_endpoint && s_bridge.started && s_bridge.config.enabled) {
            xSemaphoreTake(s_bridge.lock, portMAX_DELAY);
            invalidate_connection_locked();
            bool reconnect = s_bridge.network_up &&
                             s_bridge.config.endpoint[0] != '\0';
            set_state_locked(reconnect ? MCP_WS_CONNECTING
                                       : MCP_WS_WAIT_NETWORK);
            xSemaphoreGive(s_bridge.lock);
            bridge_event_t event = {.type = BRIDGE_EVENT_RELOAD};
            result = queue_event(&event) ? ESP_OK : ESP_ERR_NO_MEM;
        }
    }

    memset(&config, 0, sizeof(config));
    return result;
}

esp_err_t mcp_ws_bridge_config_clear(void)
{
    const mcp_ws_config_t config = {0};
    return mcp_ws_bridge_config_set(&config);
}

esp_err_t mcp_ws_bridge_config_load(mcp_ws_config_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    return config_load(out);
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
