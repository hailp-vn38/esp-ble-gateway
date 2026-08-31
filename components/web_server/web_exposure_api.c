#include "web_modules.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "device_schema.h"
#include "device_store.h"
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
    device_entry_t device = {0};
    bool has_display_name =
        device_store_get(device_id, &device) == DEVICE_STORE_OK &&
        device.name[0] != '\0' && strcmp(device.name, device.device_id) != 0;

    // Get exposure capacity.
    mcp_exposure_capacity_t capacity;
    mcp_tool_exposure_get_capacity(&capacity);

    // Get catalog revision.
    uint32_t revision = mcp_tool_catalog_get_revision();

    cJSON *root = cJSON_CreateObject();
    cJSON *commands = cJSON_CreateArray();
    if (root == NULL || commands == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(commands);
        return web_send_api_error(request, "500 Internal Server Error",
                                  "Out of memory");
    }

    cJSON_AddStringToObject(root, "device_id", device_id);
    cJSON_AddNumberToObject(root, "catalog_revision", revision);

    cJSON *cap_obj = cJSON_CreateObject();
    if (cap_obj != NULL) {
        cJSON_AddNumberToObject(cap_obj, "enabled", capacity.enabled);
        cJSON_AddNumberToObject(cap_obj, "max_enabled", capacity.max_enabled);
        cJSON_AddNumberToObject(cap_obj, "records", capacity.records);
        cJSON_AddNumberToObject(cap_obj, "max_records", capacity.max_records);
        cJSON_AddItemToObject(root, "capacity", cap_obj);
    }

    if (cap_err == ESP_OK && cap.has_committed && cap.tool_count > 0) {
        for (size_t i = 0; i < cap.tool_count; i++) {
            const device_schema_tool_t *c = &cap.tools[i];
            cJSON *item = cJSON_CreateObject();
            if (item == NULL) continue;

            cJSON_AddStringToObject(item, "command", c->command);
            cJSON_AddStringToObject(item, "label", c->label);

            const char *vt_str = c->value_type == 1 ? "boolean"
                                     : c->value_type == 2 ? "integer"
                                           : "none";
            cJSON_AddStringToObject(item, "value_type", vt_str);
            cJSON_AddBoolToObject(item, "destructive",
                                  (c->flags & DEVICE_SCHEMA_FLAG_DESTRUCTIVE) != 0);
            cJSON_AddBoolToObject(item, "idempotent",
                                  (c->flags & DEVICE_SCHEMA_FLAG_IDEMPOTENT) != 0);
            if (c->value_type == 2) {
                cJSON_AddNumberToObject(item, "minimum", c->min_value);
                cJSON_AddNumberToObject(item, "maximum", c->max_value);
                cJSON_AddNumberToObject(item, "step", c->step);
                cJSON_AddStringToObject(item, "unit", c->unit);
            }

            // Check exposure state.
            mcp_tool_exposure_t exposure;
            if (mcp_tool_exposure_get(device_id, c->command,
                                      &exposure) == ESP_OK) {
                cJSON_AddBoolToObject(item, "enabled", true);
                const char *state_str =
                    exposure.state == MCP_EXPOSURE_ENABLED
                        ? "enabled"
                        : exposure.state == MCP_EXPOSURE_NEEDS_REVIEW
                              ? "needs_review"
                              : "orphaned";
                cJSON_AddStringToObject(item, "state", state_str);
                cJSON_AddStringToObject(item, "tool_name", exposure.tool_name);
            } else {
                cJSON_AddBoolToObject(item, "enabled", false);
                cJSON_AddStringToObject(item, "state", "disabled");
                // Generate tool name deterministically.
                char tool_name[MCP_DYNAMIC_TOOL_NAME_MAX + 1];
                if (has_display_name &&
                    mcp_tool_name_generate(device.name, c->command,
                                           tool_name,
                                           sizeof(tool_name)) == ESP_OK) {
                    cJSON_AddStringToObject(item, "tool_name", tool_name);
                }
            }

            cJSON_AddItemToArray(commands, item);
        }
    }

    cJSON_AddItemToObject(root, "commands", commands);

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

    // Single command mode.
    const cJSON *command_item = cJSON_GetObjectItemCaseSensitive(json, "command");
    if (command_item != NULL) {
        const char *command = cJSON_IsString(command_item)
                                  ? command_item->valuestring
                                  : NULL;
        if (command == NULL || command[0] == '\0') {
            cJSON_Delete(json);
            return web_send_api_error_code(request, "400 Bad Request",
                                           "Invalid command",
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
        esp_err_t err;
        if (enabled) {
            mcp_exposure_enable_options_t opts = {0};
            const cJSON *confirm =
                cJSON_GetObjectItemCaseSensitive(json, "confirm_destructive");
            opts.confirm_destructive =
                cJSON_IsBool(confirm) && cJSON_IsTrue(confirm);
            err = mcp_tool_exposure_enable(device_id, command, &opts);
        } else {
            err = mcp_tool_exposure_disable(device_id, command);
        }

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
        if (err == ESP_ERR_INVALID_ARG) {
            return web_send_api_error_code(request, "400 Bad Request",
                                           "Invalid request",
                                           "invalid_request");
        }
        if (err != ESP_OK) {
            return web_send_api_error(request, "500 Internal Server Error",
                                      "Enable/disable failed");
        }

        cJSON *response = cJSON_CreateObject();
        cJSON_AddBoolToObject(response, "success", true);
        return web_send_json(request, response);
    }

    // Bulk mode.
    const cJSON *commands_item = cJSON_GetObjectItemCaseSensitive(json, "commands");
    if (commands_item != NULL && cJSON_IsArray(commands_item)) {
        int array_size = cJSON_GetArraySize(commands_item);
        cJSON *response = cJSON_CreateObject();
        cJSON *results = cJSON_CreateArray();
        bool any_error = false;

        for (int i = 0; i < array_size; i++) {
            cJSON *cmd_obj = cJSON_GetArrayItem(commands_item, i);
            const char *cmd = web_get_json_string(cmd_obj, "command",
                                                  GW_MSG_COMMAND_LEN, true);
            const cJSON *en = cJSON_GetObjectItemCaseSensitive(cmd_obj, "enabled");
            if (cmd == NULL || !cJSON_IsBool(en)) {
                cJSON *err_item = cJSON_CreateObject();
                cJSON_AddNumberToObject(err_item, "index", i);
                cJSON_AddStringToObject(err_item, "error", "invalid_request");
                cJSON_AddItemToArray(results, err_item);
                any_error = true;
                continue;
            }

            esp_err_t err;
            if (cJSON_IsTrue(en)) {
                mcp_exposure_enable_options_t opts = {0};
                const cJSON *confirm =
                    cJSON_GetObjectItemCaseSensitive(cmd_obj, "confirm_destructive");
                opts.confirm_destructive =
                    cJSON_IsBool(confirm) && cJSON_IsTrue(confirm);
                err = mcp_tool_exposure_enable(device_id, cmd, &opts);
            } else {
                err = mcp_tool_exposure_disable(device_id, cmd);
            }

            cJSON *result_item = cJSON_CreateObject();
            cJSON_AddStringToObject(result_item, "command", cmd);
            cJSON_AddBoolToObject(result_item, "success", err == ESP_OK);
            if (err != ESP_OK) {
                cJSON_AddStringToObject(result_item, "error",
                                        esp_err_to_name(err));
                any_error = true;
            }
            cJSON_AddItemToArray(results, result_item);
        }

        cJSON_AddItemToObject(response, "results", results);
        cJSON_AddBoolToObject(response, "success", !any_error);

        cJSON_Delete(json);
        httpd_resp_set_status(request, any_error ? "409 Conflict" : "200 OK");
        return web_send_json(request, response);
    }

    cJSON_Delete(json);
    return web_send_api_error_code(request, "400 Bad Request",
                                   "Provide 'command' or 'commands' array",
                                   "invalid_request");
}

// ---------------------------------------------------------------------------
// Exposure API route registration
// ---------------------------------------------------------------------------

esp_err_t web_exposure_api_register(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        {.uri = "/api/mcp/exposures", .method = HTTP_GET,
         .handler = exposure_get_handler},
        {.uri = "/api/mcp/exposures", .method = HTTP_PUT,
         .handler = exposure_put_handler},
    };
    return web_register_routes(server, routes, WEB_ARRAY_SIZE(routes));
}
