#include "web_modules.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "command_dispatcher.h"
#include "device_schema.h"
#include "ble_central.h"
#include "web_http.h"

static esp_err_t capabilities_get_handler(httpd_req_t *request)
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
    gw_message_t message = {
        .protocol_version = GW_PROTOCOL_VERSION,
        .has_device_id = true,
    };
    strlcpy(message.type, "gateway_command", sizeof(message.type));
    strlcpy(message.command, "list_device_capabilities",
            sizeof(message.command));
    strlcpy(message.device_id, device_id, sizeof(message.device_id));
    dispatch_result_t result;
    command_dispatcher_handle(&message, &result);
    web_send_dispatch_result(request, &result);
    return ESP_OK;
}

static esp_err_t capabilities_refresh_handler(httpd_req_t *request)
{
    char body[WEB_COMMAND_BODY_MAX_LEN];
    web_body_status_t body_status;
    cJSON *json = web_parse_request_json(request, body, sizeof(body),
                                         &body_status);
    if (json == NULL) return web_send_body_error(request, body_status);
    const char *device_id = web_get_json_string(
        json, "device_id", GW_MSG_DEVICE_ID_LEN, true);
    if (device_id == NULL) {
        cJSON_Delete(json);
        return web_send_api_error_code(request, "400 Bad Request",
                                       "Missing device_id", "invalid_request");
    }

    /* Preflight: device exists? */
    device_schema_snapshot_t snapshot;
    esp_err_t snapshot_error = device_schema_get(device_id, &snapshot);
    if (snapshot_error == ESP_ERR_NOT_FOUND) {
        cJSON_Delete(json);
        return web_send_api_error_code(request, "404 Not Found",
                                       "Device not found", "device_not_found");
    }

    /* Preflight: BLE runtime ready? */
    ble_central_device_status_t ble_status;
    bool ble_ready =
        ble_central_get_device_status(device_id, &ble_status) == BLE_CENTRAL_OK &&
        ble_status.ready;
    if (!ble_ready) {
        cJSON_Delete(json);
        return web_send_api_error_code(request, "502 Bad Gateway",
                                       "Device is not BLE ready",
                                       "device_not_connected");
    }

    uint32_t generation = 0;
    esp_err_t error = device_schema_refresh(device_id, &generation);
    cJSON_Delete(json);
    if (error == ESP_ERR_NOT_FOUND) {
        return web_send_api_error_code(request, "404 Not Found",
                                       "Device not found", "device_not_found");
    }
    if (error == ESP_ERR_INVALID_STATE) {
        return web_send_api_error_code(request, "409 Conflict",
                                       "Capability operation already in progress",
                                       "device_busy");
    }
    if (error != ESP_OK) {
        return web_send_api_error_code(request, "503 Service Unavailable",
                                       "Capability refresh queue is full",
                                       "queue_full");
    }

    httpd_resp_set_status(request, "202 Accepted");
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "accepted", true);
    cJSON_AddStringToObject(response, "device_id", device_id);
    cJSON_AddNumberToObject(response, "generation", generation);
    return web_send_json(request, response);
}

esp_err_t web_capability_api_register(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        {.uri = "/api/capabilities", .method = HTTP_GET,
         .handler = capabilities_get_handler},
        {.uri = "/api/capabilities/refresh", .method = HTTP_POST,
         .handler = capabilities_refresh_handler},
    };
    return web_register_routes(server, routes, WEB_ARRAY_SIZE(routes));
}
