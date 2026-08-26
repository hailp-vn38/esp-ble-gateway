#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "nvs.h"

#include "sdkconfig.h"

#include "mcp_endpoint_internal.h"

#define MCP_NVS_NAMESPACE "mcp"
#define MCP_NVS_LEGACY_KEY "legacy"

// Legacy mode resolution order: NVS runtime override -> Kconfig default.
// The override is read per request so an OTA-era flip takes effect without a
// reboot; failures fall back to the compiled default.
bool mcp_codec_legacy_enabled(void)
{
    nvs_handle_t handle;
    if (nvs_open(MCP_NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        uint8_t value = 0;
        esp_err_t err = nvs_get_u8(handle, MCP_NVS_LEGACY_KEY, &value);
        nvs_close(handle);
        if (err == ESP_OK) return value != 0;
    }
    return CONFIG_MCP_LEGACY_MODE;
}

int mcp_codec_set_legacy_override(int value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(MCP_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return -1;
    if (value < 0) {
        err = nvs_erase_key(handle, MCP_NVS_LEGACY_KEY);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    } else {
        err = nvs_set_u8(handle, MCP_NVS_LEGACY_KEY, value != 0 ? 1 : 0);
    }
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK ? 0 : -1;
}

static char *request_header(httpd_req_t *req, const char *name)
{
    return mcp_transport_get()->get_header(req, name);
}

int mcp_codec_parse_meta(httpd_req_t *req, mcp_request_meta_t *meta)
{
    memset(meta, 0, sizeof(*meta));

    char *version = request_header(req, "MCP-Protocol-Version");
    if (version == NULL) {
        // No header: legacy clients keep working only in legacy mode.
        return mcp_codec_legacy_enabled() ? 0 : MCP_ERR_VERSION;
    }
    bool supported = strcmp(version, MCP_PROTOCOL_VERSION_2026) == 0;
    free(version);
    if (!supported) return MCP_ERR_VERSION;

    meta->mcp_2026 = true;

    char *method = request_header(req, "Mcp-Method");
    if (method != NULL) {
        strlcpy(meta->mcp_method, method, sizeof(meta->mcp_method));
        free(method);
    }
    char *name = request_header(req, "Mcp-Name");
    if (name != NULL) {
        strlcpy(meta->mcp_name, name, sizeof(meta->mcp_name));
        free(name);
    }
    return 0;
}

cJSON *mcp_codec_build_discovery(void)
{
    cJSON *discovery = cJSON_CreateObject();
    cJSON *capabilities = cJSON_CreateObject();
    cJSON *tools = cJSON_CreateObject();
    if (discovery == NULL || capabilities == NULL || tools == NULL) {
        cJSON_Delete(discovery);
        cJSON_Delete(capabilities);
        cJSON_Delete(tools);
        return NULL;
    }
    cJSON_AddStringToObject(discovery, "name", MCP_SERVER_NAME);
    cJSON_AddStringToObject(discovery, "version", MCP_SERVER_VERSION);
    cJSON_AddStringToObject(discovery, "protocolVersion",
                            MCP_PROTOCOL_VERSION_2026);
    cJSON_AddBoolToObject(tools, "listChanged", false);
    cJSON_AddItemToObject(capabilities, "tools", tools);
    cJSON_AddItemToObject(discovery, "capabilities", capabilities);
    return discovery;
}
