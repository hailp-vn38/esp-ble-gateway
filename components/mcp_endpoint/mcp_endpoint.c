#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"
#include "mcp_endpoint.h"
#include "cbor_codec.h"
#include "command_dispatcher.h"

static const char *TAG = "mcp_endpoint";

// JSON-RPC 2.0 TOI GIAN cho AI Agent goi qua LAN.
// Ho tro 2 method: "list_tools" va "call_tool".
//
// Request mau:
// {"jsonrpc":"2.0","method":"call_tool",
//  "params":{"type":"device_command","device_id":"dev_A","command":"toggle","bool_value":true},
//  "id":1}

static int extract_int_field(const char *json, const char *key)
{
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *pos = strstr(json, pattern);
    if (pos == NULL) return 0;
    return atoi(pos + strlen(pattern));
}

static int extract_string_field(const char *json, const char *key, char *out, size_t out_cap)
{
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char *pos = strstr(json, pattern);
    if (pos == NULL) return -1;
    pos += strlen(pattern);
    const char *end = strchr(pos, '"');
    if (end == NULL) return -1;
    size_t val_len = end - pos;
    if (val_len >= out_cap) val_len = out_cap - 1;
    memcpy(out, pos, val_len);
    out[val_len] = '\0';
    return 0;
}

static void handle_list_tools(char *resp, size_t resp_cap, int req_id)
{
    snprintf(resp, resp_cap,
        "{\"jsonrpc\":\"2.0\",\"result\":{\"tools\":"
        "[\"add_device\",\"delete_device\",\"edit_device\",\"list_devices\","
        "\"get_status\",\"device_command\"]},\"id\":%d}", req_id);
}

static void handle_call_tool(const char *params_json, char *resp, size_t resp_cap, int req_id)
{
    gw_message_t msg;
    memset(&msg, 0, sizeof(msg));

    extract_string_field(params_json, "type", msg.type, sizeof(msg.type));
    extract_string_field(params_json, "command", msg.command, sizeof(msg.command));

    if (extract_string_field(params_json, "device_id", msg.device_id, sizeof(msg.device_id)) == 0 &&
        strlen(msg.device_id) > 0) {
        msg.has_device_id = 1;
    }

    msg.int_value = extract_int_field(params_json, "int_value");
    msg.bool_value = (strstr(params_json, "\"bool_value\":true") != NULL) ? 1 : 0;

    if (strlen(msg.type) == 0) {
        strncpy(msg.type, msg.has_device_id ? "device_command" : "gateway_command", sizeof(msg.type) - 1);
    }

    dispatch_result_t result;
    command_dispatcher_handle(&msg, &result);

    snprintf(resp, resp_cap,
        "{\"jsonrpc\":\"2.0\",\"result\":{\"success\":%s,\"message\":\"%s\"},\"id\":%d}",
        result.success ? "true" : "false", result.message, req_id);
}

static esp_err_t mcp_post_handler(httpd_req_t *req)
{
    char buf[512];
    int recv_len = req->content_len;
    if (recv_len >= (int)sizeof(buf)) recv_len = sizeof(buf) - 1;
    int len = httpd_req_recv(req, buf, recv_len);
    if (len <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[len] = '\0';

    char method[32] = {0};
    extract_string_field(buf, "method", method, sizeof(method));
    int req_id = extract_int_field(buf, "id");

    char resp[512];

    if (strcmp(method, "list_tools") == 0) {
        handle_list_tools(resp, sizeof(resp), req_id);
    } else if (strcmp(method, "call_tool") == 0) {
        const char *params_start = strstr(buf, "\"params\":");
        const char *params_json = (params_start != NULL) ? params_start : buf;
        handle_call_tool(params_json, resp, sizeof(resp), req_id);
    } else {
        snprintf(resp, sizeof(resp),
            "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32601,\"message\":\"Method not found\"},\"id\":%d}",
            req_id);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);

    ESP_LOGI(TAG, "MCP request: method=%s -> %s", method, resp);
    return ESP_OK;
}

int mcp_endpoint_register(httpd_handle_t server)
{
    httpd_uri_t mcp_route = {
        .uri = "/mcp",
        .method = HTTP_POST,
        .handler = mcp_post_handler,
    };

    esp_err_t err = httpd_register_uri_handler(server, &mcp_route);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /mcp route: %s", esp_err_to_name(err));
        return -1;
    }

    ESP_LOGI(TAG, "MCP endpoint registered at POST /mcp");
    return 0;
}
