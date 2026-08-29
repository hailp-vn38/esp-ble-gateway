#include <string.h>

#include "cJSON.h"
#include "command_dispatcher.h"
#include "mcp_core.h"
#include "unity.h"

#include "../command_dispatcher_internal.h"

typedef struct {
    char response[2048];
    size_t response_len;
    unsigned json_count;
    unsigned none_count;
} capture_t;

static esp_err_t capture_json(void *context, const char *json, size_t len,
                              const mcp_response_meta_t *meta)
{
    (void)meta;
    capture_t *capture = context;
    TEST_ASSERT_LESS_THAN(sizeof(capture->response), len);
    memcpy(capture->response, json, len);
    capture->response[len] = '\0';
    capture->response_len = len;
    capture->json_count++;
    return ESP_OK;
}

static esp_err_t capture_none(void *context, const mcp_response_meta_t *meta)
{
    (void)meta;
    ((capture_t *)context)->none_count++;
    return ESP_OK;
}

static bool capture_alive(void *context)
{
    return context != NULL;
}

static mcp_responder_t responder_for(capture_t *capture)
{
    return (mcp_responder_t){
        .context = capture,
        .send_json = capture_json,
        .send_none = capture_none,
        .is_alive = capture_alive,
    };
}

static mcp_wire_context_t xiaozhi_wire(bool initialized)
{
    mcp_wire_context_t wire = {
        .transport = MCP_TRANSPORT_WS,
        .authenticated = true,
        .trusted_transport = true,
        .has_protocol_version = initialized,
    };
    if (initialized) {
        strlcpy(wire.protocol_version, "2024-11-05",
                sizeof(wire.protocol_version));
    }
    return wire;
}

TEST_CASE("Xiaozhi initialize negotiates MCP 2024-11-05", "[mcp][xiaozhi]")
{
    static const char request[] =
        "{\"jsonrpc\":\"2.0\",\"id\":0,\"method\":\"initialize\","
        "\"params\":{\"protocolVersion\":\"2024-11-05\","
        "\"capabilities\":{},\"clientInfo\":{\"name\":\"xz-mcp-broker\","
        "\"version\":\"0.0.1\"}}}";
    capture_t capture = {0};
    mcp_responder_t responder = responder_for(&capture);
    mcp_wire_context_t wire = xiaozhi_wire(false);

    TEST_ASSERT_EQUAL(ESP_OK, mcp_core_handle_json(
                                  request, strlen(request), &wire, &responder));
    TEST_ASSERT_EQUAL_UINT(1, capture.json_count);
    cJSON *response = cJSON_Parse(capture.response);
    TEST_ASSERT_NOT_NULL(response);
    cJSON *result = cJSON_GetObjectItemCaseSensitive(response, "result");
    TEST_ASSERT_EQUAL_STRING(
        "2024-11-05",
        cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(
            result, "protocolVersion")));
    TEST_ASSERT_TRUE(cJSON_IsObject(
        cJSON_GetObjectItemCaseSensitive(result, "capabilities")));
    TEST_ASSERT_TRUE(cJSON_IsObject(
        cJSON_GetObjectItemCaseSensitive(result, "serverInfo")));
    cJSON_Delete(response);
}

TEST_CASE("Xiaozhi initialized notification has no response", "[mcp][xiaozhi]")
{
    static const char request[] =
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
    capture_t capture = {0};
    mcp_responder_t responder = responder_for(&capture);
    mcp_wire_context_t wire = xiaozhi_wire(true);

    TEST_ASSERT_EQUAL(ESP_OK, mcp_core_handle_json(
                                  request, strlen(request), &wire, &responder));
    TEST_ASSERT_EQUAL_UINT(0, capture.json_count);
    TEST_ASSERT_EQUAL_UINT(1, capture.none_count);
}

TEST_CASE("Xiaozhi ping receives empty JSON-RPC result", "[mcp][xiaozhi]")
{
    static const char request[] =
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"ping\",\"params\":{}}";
    capture_t capture = {0};
    mcp_responder_t responder = responder_for(&capture);
    mcp_wire_context_t wire = xiaozhi_wire(true);

    TEST_ASSERT_EQUAL(ESP_OK, mcp_core_handle_json(
                                  request, strlen(request), &wire, &responder));
    cJSON *response = cJSON_Parse(capture.response);
    TEST_ASSERT_NOT_NULL(response);
    TEST_ASSERT_EQUAL(2, (int)cJSON_GetNumberValue(
                             cJSON_GetObjectItemCaseSensitive(response, "id")));
    TEST_ASSERT_TRUE(cJSON_IsObject(
        cJSON_GetObjectItemCaseSensitive(response, "result")));
    cJSON_Delete(response);
}

TEST_CASE("Xiaozhi tools list uses shared MCP catalog", "[mcp][xiaozhi]")
{
    static const char request[] =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\",\"params\":{}}";
    command_dispatcher_reset_for_test();
    TEST_ASSERT_EQUAL(0, command_dispatcher_init());
    TEST_ASSERT_EQUAL(0, command_dispatcher_freeze_registry());
    capture_t capture = {0};
    mcp_responder_t responder = responder_for(&capture);
    mcp_wire_context_t wire = xiaozhi_wire(true);

    TEST_ASSERT_EQUAL(ESP_OK, mcp_core_handle_json(
                                  request, strlen(request), &wire, &responder));
    cJSON *response = cJSON_Parse(capture.response);
    TEST_ASSERT_NOT_NULL(response);
    cJSON *result = cJSON_GetObjectItemCaseSensitive(response, "result");
    cJSON *tools = cJSON_GetObjectItemCaseSensitive(result, "tools");
    TEST_ASSERT_TRUE(cJSON_IsArray(tools));
    TEST_ASSERT_GREATER_THAN(0, cJSON_GetArraySize(tools));
    cJSON_Delete(response);
}
