#include <string.h>

#include "cJSON.h"
#include "mcp_core.h"
#include "unity.h"

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

TEST_CASE("Xiaozhi compact tools list is exact and teaches semantic flow", "[mcp][xiaozhi]")
{
    static const char request[] =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\",\"params\":{}}";
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
    TEST_ASSERT_EQUAL_INT(3, cJSON_GetArraySize(tools));
    const char *expected[] = {"get_status", "list_devices", "device_control"};
    for (size_t i = 0; i < 3; ++i) {
        cJSON *tool = cJSON_GetArrayItem(tools, i);
        TEST_ASSERT_EQUAL_STRING(
            expected[i], cJSON_GetStringValue(
                             cJSON_GetObjectItemCaseSensitive(tool, "name")));
    }
    cJSON *list_tool = cJSON_GetArrayItem(tools, 1);
    cJSON *control_tool = cJSON_GetArrayItem(tools, 2);
    TEST_ASSERT_NOT_NULL(strstr(cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(list_tool, "description")),
        "controls[].feature_id"));
    TEST_ASSERT_NOT_NULL(strstr(cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(control_tool, "description")),
        "prefer device_id and feature_id returned by list_devices"));
    cJSON_Delete(response);
}

TEST_CASE("Xiaozhi list_devices baseline records payload size",
          "[mcp][xiaozhi][baseline]")
{
    static const char request[] =
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"list_devices\",\"arguments\":{}}}";
    capture_t capture = {0};
    mcp_responder_t responder = responder_for(&capture);
    mcp_wire_context_t wire = xiaozhi_wire(true);
    TEST_ASSERT_EQUAL(ESP_OK, mcp_core_handle_json(
                                  request, strlen(request), &wire, &responder));
    TEST_ASSERT_GREATER_THAN_UINT32(0, capture.response_len);
    printf("PHASE0 list_devices_payload_bytes=%u\n",
           (unsigned)capture.response_len);
    cJSON *response = cJSON_Parse(capture.response);
    TEST_ASSERT_NOT_NULL(response);
    TEST_ASSERT_TRUE(cJSON_IsObject(
        cJSON_GetObjectItemCaseSensitive(response, "result")));
    cJSON_Delete(response);
}

TEST_CASE("Xiaozhi device_control routes describe read and set",
          "[mcp][xiaozhi][baseline]")
{
    static const char *operations[] = {"describe", "read", "set"};
    mcp_wire_context_t wire = xiaozhi_wire(true);

    for (size_t i = 0; i < 3; ++i) {
        char request[320];
        snprintf(request, sizeof(request),
                 "{\"jsonrpc\":\"2.0\",\"id\":%u,\"method\":\"tools/call\","
                 "\"params\":{\"name\":\"device_control\",\"arguments\":{"
                 "\"operation\":\"%s\",\"device\":\"phase0-missing\","
                 "\"feature\":\"power\",\"bool_value\":true}}}",
                 (unsigned)(i + 10), operations[i]);
        capture_t capture = {0};
        mcp_responder_t responder = responder_for(&capture);
        TEST_ASSERT_EQUAL(ESP_OK, mcp_core_handle_json(
                                      request, strlen(request), &wire,
                                      &responder));
        cJSON *response = cJSON_Parse(capture.response);
        TEST_ASSERT_NOT_NULL(response);
        cJSON *result = cJSON_GetObjectItemCaseSensitive(response, "result");
        TEST_ASSERT_TRUE(cJSON_IsObject(result));
        TEST_ASSERT_TRUE(cJSON_IsTrue(
            cJSON_GetObjectItemCaseSensitive(result, "isError")));
        cJSON_Delete(response);
    }
}
