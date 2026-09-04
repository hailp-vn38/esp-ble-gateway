#include "web_modules.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "device_schema.h"
#include "device_store.h"
#include "device_template.h"
#include "mcp_tool_exposure.h"
#include "web_http.h"

// ---------------------------------------------------------------------------
// GET /api/mcp/exposures — Web Auth protected (§22)
// ---------------------------------------------------------------------------

static esp_err_t exposure_get_handler(httpd_req_t *request)
{
    char query[128];
    char device_id[GW_MSG_DEVICE_ID_LEN] = {0};
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK ||
        web_get_query_value(query, "device_id", device_id,
                            sizeof(device_id)) != ESP_OK ||
        device_id[0] == '\0') {
        return web_send_api_error_code(request, "400 Bad Request",
                                       "Missing device_id", "invalid_request");
    }

    // Get capability snapshot.
    device_schema_snapshot_t cap;
    esp_err_t cap_err = device_schema_get(device_id, &cap);

    // Get exposure capacity.
    mcp_exposure_capacity_t capacity;
    mcp_tool_exposure_get_capacity(&capacity);

    uint32_t revision = mcp_tool_exposure_get_policy_revision();

    cJSON *root = cJSON_CreateObject();
    cJSON *features = cJSON_CreateArray();
    cJSON *cap_obj = cJSON_CreateObject();
    if (root == NULL || features == NULL || cap_obj == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(features);
        cJSON_Delete(cap_obj);
        return web_send_api_error(request, "500 Internal Server Error",
                                  "Out of memory");
    }

    cJSON_AddStringToObject(root, "device_id", device_id);
    cJSON_AddNumberToObject(root, "policy_revision", revision);

    cJSON_AddNumberToObject(cap_obj, "enabled", capacity.enabled);
    cJSON_AddNumberToObject(cap_obj, "max_enabled", capacity.max_enabled);
    cJSON_AddNumberToObject(cap_obj, "records", capacity.records);
    cJSON_AddNumberToObject(cap_obj, "max_records", capacity.max_records);
    cJSON_AddItemToObject(root, "capacity", cap_obj);

    if (cap_err == ESP_OK && cap.has_committed) {
        /* Feature-oriented response (compact-friendly) */
        for (size_t f = 0; f < cap.feature_count; f++) {
            const device_schema_feature_t *feat = &cap.features[f];
            cJSON *fitem = cJSON_CreateObject();
            if (fitem == NULL) continue;

            cJSON_AddStringToObject(fitem, "feature_id", feat->feature_id);

            const device_template_t *tpl = device_template_resolve(
                feat->feature_type, feat->feature_schema_version);
            if (tpl != NULL) {
                cJSON_AddStringToObject(fitem, "type",
                                        device_template_feature_name(feat->feature_type));
                cJSON_AddStringToObject(fitem, "semantic_name", tpl->semantic_name);
            }

            const char *prop_name = device_template_property_name(feat->property_id);
            cJSON_AddStringToObject(fitem, "property", prop_name);

            device_template_value_type_t vtype =
                device_template_property_value_type(feat->property_id);
            cJSON_AddStringToObject(fitem, "value_type",
                                    vtype == DEVICE_TEMPLATE_VALUE_BOOL ? "bool" :
                                    vtype == DEVICE_TEMPLATE_VALUE_INT ? "int" : "none");

            /* Writable command info */
            if (feat->writable_tool_index >= 0 &&
                (size_t)feat->writable_tool_index < cap.tool_count) {
                const device_schema_tool_t *tool =
                    &cap.tools[feat->writable_tool_index];
                cJSON_AddStringToObject(fitem, "command", tool->command);
                cJSON_AddBoolToObject(fitem, "destructive",
                                      (tool->flags & DEVICE_SCHEMA_FLAG_DESTRUCTIVE) != 0);
            }

            /* Exposure status per feature */
            mcp_tool_exposure_t exposure;
            if (mcp_tool_exposure_get_feature(device_id, feat->feature_id,
                                               &exposure) == ESP_OK) {
                cJSON_AddBoolToObject(fitem, "control_enabled",
                                      exposure.control_enabled);
                const char *state_str =
                    exposure.state == MCP_EXPOSURE_ENABLED ? "enabled" :
                    exposure.state == MCP_EXPOSURE_NEEDS_REVIEW ? "needs_review" :
                    "orphaned";
                cJSON_AddStringToObject(fitem, "health", state_str);
            } else {
                cJSON_AddBoolToObject(fitem, "control_enabled", false);
                cJSON_AddStringToObject(fitem, "health", "missing");
            }

            cJSON_AddItemToArray(features, fitem);
        }
    }

    cJSON_AddItemToObject(root, "features", features);

    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return web_send_json(request, root);
}

// ---------------------------------------------------------------------------
// PUT /api/mcp/exposures — admin-protected, single/bulk (§27.4)
// ---------------------------------------------------------------------------

static esp_err_t exposure_put_handler(httpd_req_t *request)
{
    char body[WEB_COMMAND_BODY_MAX_LEN];
    web_body_status_t body_status;
    cJSON *json = web_parse_request_json(request, body, sizeof(body),
                                         &body_status);
    if (json == NULL) {
        return web_send_body_error(request, body_status);
    }

    const char *device_id = web_get_json_string(
        json, "device_id", GW_MSG_DEVICE_ID_LEN, true);
    if (device_id == NULL) {
        cJSON_Delete(json);
        return web_send_api_error_code(request, "400 Bad Request",
                                       "Missing device_id", "invalid_request");
    }

    const cJSON *feature_item = cJSON_GetObjectItemCaseSensitive(json, "feature_id");

    /* Compact mode: accept feature_id + enabled */
    if (feature_item != NULL && cJSON_IsString(feature_item)) {
        const char *feature_id = cJSON_GetStringValue(feature_item);
        if (feature_id == NULL || feature_id[0] == '\0') {
            cJSON_Delete(json);
            return web_send_api_error_code(request, "400 Bad Request",
                                           "Invalid feature_id",
                                           "invalid_request");
        }

        const cJSON *enabled_item =
            cJSON_GetObjectItemCaseSensitive(json, "enabled");
        if (!cJSON_IsBool(enabled_item)) {
            cJSON_Delete(json);
            return web_send_api_error_code(request, "400 Bad Request",
                                           "enabled must be boolean",
                                           "invalid_request");
        }

        bool enabled = cJSON_IsTrue(enabled_item);

        /* Validate that this is a writable committed semantic feature. */
        device_schema_snapshot_t cap;
        if (device_schema_get(device_id, &cap) != ESP_OK || !cap.has_committed) {
            cJSON_Delete(json);
            return web_send_api_error_code(request, "404 Not Found",
                                           "No committed capabilities",
                                           "capabilities_not_ready");
        }

        const char *command = NULL;
        for (size_t f = 0; f < cap.feature_count; f++) {
            if (strcmp(cap.features[f].feature_id, feature_id) == 0) {
                int8_t idx = cap.features[f].writable_tool_index;
                if (idx >= 0 && (size_t)idx < cap.tool_count) {
                    command = cap.tools[idx].command;
                }
                break;
            }
        }

        if (command == NULL) {
            cJSON_Delete(json);
            return web_send_api_error_code(request, "404 Not Found",
                                           "Feature not found or read-only",
                                           "not_found");
        }

        esp_err_t err = mcp_tool_exposure_set_feature_enabled(
            device_id, feature_id, enabled);

        cJSON_Delete(json);

        if (err == ESP_ERR_NOT_FOUND) {
            return web_send_api_error_code(request, "404 Not Found",
                                           "Device or command not found",
                                           "not_found");
        }
        if (err == ESP_ERR_INVALID_STATE) {
            return web_send_api_error_code(request, "409 Conflict",
                                           "No committed capabilities",
                                           "capabilities_not_ready");
        }
        if (err == ESP_ERR_NO_MEM) {
            return web_send_api_error_code(request, "409 Conflict",
                                           "Capacity exceeded",
                                           "mcp_tool_capacity_exceeded");
        }
        if (err == ESP_ERR_NOT_SUPPORTED) {
            return web_send_api_error_code(request, "403 Forbidden",
                                           "Destructive command blocked by policy",
                                           "destructive_blocked");
        }
        if (err != ESP_OK) {
            return web_send_api_error(request, "500 Internal Server Error",
                                      "Enable/disable failed");
        }

        mcp_tool_exposure_t updated;
        if (mcp_tool_exposure_get_feature(device_id, feature_id, &updated) != ESP_OK) {
            return web_send_api_error(request, "500 Internal Server Error",
                                      "Exposure was updated but could not be read");
        }

        cJSON *response = cJSON_CreateObject();
        if (response == NULL) return ESP_ERR_NO_MEM;
        cJSON_AddBoolToObject(response, "success", true);
        cJSON_AddStringToObject(response, "device_id", device_id);
        cJSON_AddStringToObject(response, "feature_id", feature_id);
        cJSON_AddBoolToObject(response, "control_enabled",
                              updated.control_enabled);
        cJSON_AddStringToObject(response, "health",
                                updated.state == MCP_EXPOSURE_ENABLED ? "enabled" :
                                updated.state == MCP_EXPOSURE_NEEDS_REVIEW ? "needs_review" :
                                "orphaned");
        cJSON_AddNumberToObject(response, "policy_revision",
                                mcp_tool_exposure_get_policy_revision());
        return web_send_json(request, response);
    }

    /* No matching mode. */
    cJSON_Delete(json);
    return web_send_api_error_code(request, "400 Bad Request",
                                   "Provide 'feature_id' and 'enabled'",
                                   "invalid_request");
}

// ---------------------------------------------------------------------------
// Exposure API route registration
// ---------------------------------------------------------------------------

esp_err_t web_exposure_api_register(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        WEB_URI_INIT("/api/mcp/exposures", HTTP_GET, exposure_get_handler),
        WEB_URI_INIT("/api/mcp/exposures", HTTP_PUT, exposure_put_handler),
    };
    return web_register_routes(server, routes, WEB_ARRAY_SIZE(routes));
}
