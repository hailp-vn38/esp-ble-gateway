#include <string.h>

#include "cJSON.h"
#include "unity.h"

#include "../command_dispatcher_internal.h"
#include "cbor_codec.h"
#include "command_dispatcher.h"

#include "test_mcp_transport.h"

// Conformance matrix for the 2026-07-28 wire mode plus the legacy-mode
// feature flag. Legacy behavior itself is covered by test_mcp_endpoint.c.

static void mcp_setup(void)
{
    command_dispatcher_reset_for_test();
    TEST_ASSERT_EQUAL_INT(0, command_dispatcher_init());
    TEST_ASSERT_EQUAL_INT(0, command_dispatcher_freeze_registry());
    mcp_auth_reset_rate_limit();
}

// ---------------------------------------------------------------------------
// Header handling
// ---------------------------------------------------------------------------

TEST_CASE("supported protocol version header enables the 2026 wire mode",
          "[mcp_conformance]")
{
    mcp_setup();
    TEST_ASSERT_EQUAL_INT(0, run_request(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}"));
    // io_reset wiped headers; re-run with the version header present.
    io_reset("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}");
    io_set_header("MCP-Protocol-Version", "2026-07-28");
    install_transport();
    memset(&g_req, 0, sizeof(g_req));
    g_req.content_len = (int)g_io.body_len;
    TEST_ASSERT_EQUAL_INT(0, mcp_handle_request(&g_req));

    cJSON *response = io_response_json();
    cJSON *result = cJSON_GetObjectItemCaseSensitive(response, "result");
    TEST_ASSERT_NOT_NULL(result);

    // ListToolsResult cache hints required by 2026-07-28.
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(result, "ttlMs"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(result, "cacheScope"));

    // Response _meta carries protocol version + server identity.
    cJSON *meta = cJSON_GetObjectItemCaseSensitive(response, "_meta");
    TEST_ASSERT_NOT_NULL(meta);
    cJSON *meta_version = cJSON_GetObjectItemCaseSensitive(
        meta, "io.modelcontextprotocol/protocolVersion");
    TEST_ASSERT_EQUAL_STRING("2026-07-28", cJSON_GetStringValue(meta_version));
    cJSON *server = cJSON_GetObjectItemCaseSensitive(
        meta, "io.modelcontextprotocol/server");
    TEST_ASSERT_EQUAL_STRING(
        "esp32-ble-gateway",
        cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(server, "name")));
    cJSON_Delete(response);
}

TEST_CASE("unsupported protocol version is -32022 with HTTP 400",
          "[mcp_conformance]")
{
    mcp_setup();
    io_reset("{}");
    io_set_header("MCP-Protocol-Version", "2025-06-18");
    install_transport();
    memset(&g_req, 0, sizeof(g_req));
    g_req.content_len = (int)g_io.body_len;
    TEST_ASSERT_EQUAL_INT(0, mcp_handle_request(&g_req));

    TEST_ASSERT_EQUAL_STRING("400 Bad Request", g_io.status_line);
    cJSON *response = io_response_json();
    TEST_ASSERT_EQUAL_INT(-32022,
                          (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(
                              cJSON_GetObjectItemCaseSensitive(response, "error"),
                              "code")));
    cJSON_Delete(response);
}

TEST_CASE("legacy disabled rejects requests without a version header",
          "[mcp_conformance]")
{
    mcp_setup();
    TEST_ASSERT_EQUAL_INT(0, mcp_codec_set_legacy_override(0));
    esp_err_t outcome = run_request(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}");
    int rc = mcp_codec_set_legacy_override(-1); // restore Kconfig default
    TEST_ASSERT_EQUAL_INT(0, outcome);
    TEST_ASSERT_EQUAL_INT(0, rc);

    TEST_ASSERT_EQUAL_STRING("400 Bad Request", g_io.status_line);
    cJSON *response = io_response_json();
    TEST_ASSERT_EQUAL_INT(-32022,
                          (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(
                              cJSON_GetObjectItemCaseSensitive(response, "error"),
                              "code")));
    cJSON_Delete(response);
}

// ---------------------------------------------------------------------------
// CallToolResult shape in 2026 mode
// ---------------------------------------------------------------------------

TEST_CASE("tools/call result uses resultType, content and isError",
          "[mcp_conformance]")
{
    mcp_setup();
    io_reset("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"get_status\"}}");
    io_set_header("MCP-Protocol-Version", "2026-07-28");
    install_transport();
    memset(&g_req, 0, sizeof(g_req));
    g_req.content_len = (int)g_io.body_len;
    TEST_ASSERT_EQUAL_INT(0, mcp_handle_request(&g_req));

    cJSON *response = io_response_json();
    cJSON *result = cJSON_GetObjectItemCaseSensitive(response, "result");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(response, "error"));

    TEST_ASSERT_EQUAL_STRING(
        "complete",
        cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(result, "resultType")));
    TEST_ASSERT_FALSE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "isError")));

    cJSON *content = cJSON_GetObjectItemCaseSensitive(result, "content");
    TEST_ASSERT_TRUE(cJSON_IsArray(content));
    cJSON *first = cJSON_GetArrayItem(content, 0);
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_EQUAL_STRING("text",
                             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(first, "type")));
    TEST_ASSERT_NOT_NULL(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(first, "text")));
    cJSON_Delete(response);
}

TEST_CASE("server/discover returns identity and capabilities",
          "[mcp_conformance]")
{
    mcp_setup();
    io_reset("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"server/discover\"}");
    io_set_header("MCP-Protocol-Version", "2026-07-28");
    install_transport();
    memset(&g_req, 0, sizeof(g_req));
    g_req.content_len = (int)g_io.body_len;
    TEST_ASSERT_EQUAL_INT(0, mcp_handle_request(&g_req));

    cJSON *response = io_response_json();
    cJSON *result = cJSON_GetObjectItemCaseSensitive(response, "result");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQUAL_STRING(
        "esp32-ble-gateway",
        cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(result, "name")));
    TEST_ASSERT_EQUAL_STRING(
        "2026-07-28",
        cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(result, "protocolVersion")));
    cJSON *capabilities = cJSON_GetObjectItemCaseSensitive(result, "capabilities");
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(capabilities, "tools"));
    cJSON_Delete(response);
}
