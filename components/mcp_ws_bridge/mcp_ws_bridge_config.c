#include "mcp_ws_bridge_internal.h"

#ifdef CONFIG_MCP_WS_BRIDGE

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "mcp_ws_bridge";

#define MCP_WS_NVS_NAMESPACE "mcp_ws"
#define MCP_WS_NVS_ENABLED   "enabled"
#define MCP_WS_NVS_ENDPOINT  "endpoint"

/* ── Default enabled ────────────────────────────────────────────────── */

static bool default_enabled(void)
{
#ifdef CONFIG_MCP_WS_DEFAULT_ENABLED
    return true;
#else
    return false;
#endif
}

/* ── NVS config load/store ──────────────────────────────────────────── */

esp_err_t mcp_ws_config_load(mcp_ws_config_t *config)
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
    if (config->endpoint[0] != '\0' &&
        !mcp_ws_endpoint_valid(config->endpoint)) {
        ESP_LOGW(TAG, "Stored endpoint is invalid; bridge disabled");
        config->enabled = false;
        config->endpoint[0] = '\0';
    }
    return result;
}

esp_err_t mcp_ws_config_store(const mcp_ws_config_t *config)
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

/* ── Public config API ──────────────────────────────────────────────── */

esp_err_t mcp_ws_bridge_config_set(const mcp_ws_config_t *config)
{
    if (config == NULL ||
        (config->endpoint[0] != '\0' &&
         !mcp_ws_endpoint_valid(config->endpoint)) ||
        (config->enabled && config->endpoint[0] == '\0')) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = mcp_ws_config_store(config);
    if (result != ESP_OK) return result;
    if (s_bridge.initialized) {
        xSemaphoreTake(s_bridge.lock, portMAX_DELAY);
        s_bridge.config = *config;
        s_bridge.status.enabled = config->enabled;
        s_bridge.status.endpoint_configured = config->endpoint[0] != '\0';
        bridge_invalidate_connection_locked();
        bool reconnect = s_bridge.started && s_bridge.network_up &&
                         config->enabled && config->endpoint[0] != '\0';
        bridge_set_state_locked(!config->enabled
                                    ? MCP_WS_DISABLED
                                    : (reconnect ? MCP_WS_CONNECTING
                                                 : MCP_WS_WAIT_NETWORK));
        xSemaphoreGive(s_bridge.lock);
        bridge_event_t event = {.type = BRIDGE_EVENT_RELOAD};
        result = bridge_queue_event(&event) ? ESP_OK : ESP_ERR_NO_MEM;
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
        esp_err_t result = mcp_ws_config_load(&config);
        if (result != ESP_OK) return result;
    }
    memset(out, 0, sizeof(*out));
    out->enabled = config.enabled;
    out->endpoint_configured = config.endpoint[0] != '\0';
    mcp_ws_endpoint_display(config.endpoint, out->endpoint_display,
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
        esp_err_t result = mcp_ws_config_load(&config);
        if (result != ESP_OK) return result;
    }

    bool changed_enabled = has_enabled && (enabled != config.enabled);
    if (has_enabled) config.enabled = enabled;
    if (has_endpoint) {
        strlcpy(config.endpoint, endpoint, sizeof(config.endpoint));
    }

    esp_err_t result = mcp_ws_config_store(&config);
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
            bridge_invalidate_connection_locked();
            bool reconnect = s_bridge.network_up &&
                             s_bridge.config.endpoint[0] != '\0';
            bridge_set_state_locked(reconnect ? MCP_WS_CONNECTING
                                               : MCP_WS_WAIT_NETWORK);
            xSemaphoreGive(s_bridge.lock);
            bridge_event_t event = {.type = BRIDGE_EVENT_RELOAD};
            result = bridge_queue_event(&event) ? ESP_OK : ESP_ERR_NO_MEM;
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
    return mcp_ws_config_load(out);
}

#endif /* CONFIG_MCP_WS_BRIDGE */
