#include <string.h>

#include "cJSON.h"
#include "unity.h"

#include "../command_dispatcher_internal.h"
#include "cbor_codec.h"
#include "command_dispatcher.h"

#include "test_mcp_transport.h"

// Conformance matrix for the 2026-07-28 wire mode and 2025-11-25 compat.

static void mcp_setup(void)
{
    command_dispatcher_reset_for_test();
    TEST_ASSERT_EQUAL_INT(0, command_dispatcher_init());
    TEST_ASSERT_EQUAL_INT(0, command_dispatcher_freeze_registry());
    mcp_auth_reset_rate_limit();
}

// ---------------------------------------------------------------------------
// MCP 2026 header handling
// ---------------------------------------------------------------------------

TEST_CASE("supported protocol version header enables the 2026 wire mode",
          "[mcp_conformance]")
{
    mcp_setup();
    io_reset("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"," 
             "\"params\":{\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\"," 
             "\"io.modelcontextprotocol/clientCapabilities\":{}}}}");
    io_set_header("MCP-Protocol-Version", "2026-07-28");
    io_set_header("Mcp-Method", "tools/list");
    install_transport();
    memset(&g_req, 0, sizeof(g_req));
    g_req.content_len = (int)g_io.body_len;
    TEST_ASSERT_EQUAL_INT(0, mcp_handle_request(&g_req));

    cJSON *response = io_response_json();
    cJSON *result = cJSON_GetObjectItemCaseSensitive(response, "result");
    TEST_ASSERT_NOT_NULL(result);

    TEST_ASSERT_EQUAL_STRING(
        "complete",
        cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(result, "resultType")));

    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(result, "ttlMs"));
    TEST_ASSERT_EQUAL_STRING(
        "private",
        cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(result, "cacheScope")));

    // serverInfo in result._meta (§12.4)
    cJSON *meta = cJSON_GetObjectItemCaseSensitive(result, "_meta");
    TEST_ASSERT_NOT_NULL(meta);
    cJSON *server_info = cJSON_GetObjectItemCaseSensitive(
        meta, "io.modelcontextprotocol/serverInfo");
    TEST_ASSERT_NOT_NULL(server_info);
    TEST_ASSERT_EQUAL_STRING(
        "esp32-ble-gateway",
        cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(server_info, "name")));
    TEST_ASSERT_EQUAL_STRING(
        "1.0.0",
        cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(server_info, "version")));
    cJSON_Delete(response);
}

TEST_CASE("unsupported protocol version is -32022 with HTTP 400",
          "[mcp_conformance]")
{
    mcp_setup();
    io_reset("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}");
    io_set_header("MCP-Protocol-Version", "2025-06-18");
    install_transport();
    memset(&g_req, 0, sizeof(g_req));
    g_req.content_len = (int)g_io.body_len;
    TEST_ASSERT_EQUAL_INT(0, mcp_handle_request(&g_req));

    TEST_ASSERT_EQUAL_STRING("400 Bad Request", g_io.status_line);
    cJSON *response = io_response_json();
    cJSON *error = cJSON_GetObjectItemCaseSensitive(response, "error");
    TEST_ASSERT_NOT_NULL(error);
    TEST_ASSERT_EQUAL_INT(-32022,
                          (int)cJSON_GetNumberValue(
                              cJSON_GetObjectItemCaseSensitive(error, "code")));
    // error.data should contain supported and requested versions
    cJSON *data = cJSON_GetObjectItemCaseSensitive(error, "data");
    TEST_ASSERT_NOT_NULL(data);
    cJSON *supported = cJSON_GetObjectItemCaseSensitive(data, "supported");
    TEST_ASSERT_NOT_NULL(supported);
    TEST_ASSERT_TRUE(cJSON_IsArray(supported));
    cJSON_Delete(response);
}

TEST_CASE("version header missing follows configured compatibility mode",
          "[mcp_conformance]")
{
    mcp_setup();
    // A versionless request uses the legacy path only when compatibility is
    // enabled; otherwise it is rejected as an unsupported protocol version.
    esp_err_t outcome = run_request(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}");
    TEST_ASSERT_EQUAL_INT(0, outcome);

    cJSON *response = io_response_json();
#if CONFIG_MCP_COMPAT_2025
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(response, "result"));
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(response, "error"));
#else
    TEST_ASSERT_EQUAL_STRING("400 Bad Request", g_io.status_line);
    cJSON *error = cJSON_GetObjectItemCaseSensitive(response, "error");
    TEST_ASSERT_NOT_NULL(error);
    int code = (int)cJSON_GetNumberValue(
        cJSON_GetObjectItemCaseSensitive(error, "code"));
    TEST_ASSERT_EQUAL_INT(-32022, code);
#endif
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
             "\"params\":{\"name\":\"get_status\","
             "\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\","
             "\"io.modelcontextprotocol/clientCapabilities\":{}}}}");
    io_set_header("MCP-Protocol-Version", "2026-07-28");
    io_set_header("Mcp-Method", "tools/call");
    io_set_header("Mcp-Name", "get_status");
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

    // serverInfo in result._meta
    cJSON *meta = cJSON_GetObjectItemCaseSensitive(result, "_meta");
    TEST_ASSERT_NOT_NULL(meta);
    cJSON *server_info = cJSON_GetObjectItemCaseSensitive(
        meta, "io.modelcontextprotocol/serverInfo");
    TEST_ASSERT_NOT_NULL(server_info);
    cJSON_Delete(response);
}

TEST_CASE("server/discover returns target MCP 2026-07-28 shape",
          "[mcp_conformance]")
{
    mcp_setup();
    io_reset("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"server/discover\","
             "\"params\":{\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\","
             "\"io.modelcontextprotocol/clientCapabilities\":{}}}}");
    io_set_header("MCP-Protocol-Version", "2026-07-28");
    io_set_header("Mcp-Method", "server/discover");
    install_transport();
    memset(&g_req, 0, sizeof(g_req));
    g_req.content_len = (int)g_io.body_len;
    TEST_ASSERT_EQUAL_INT(0, mcp_handle_request(&g_req));

    cJSON *response = io_response_json();
    cJSON *result = cJSON_GetObjectItemCaseSensitive(response, "result");
    TEST_ASSERT_NOT_NULL(result);

    TEST_ASSERT_EQUAL_STRING(
        "complete",
        cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(result, "resultType")));

    cJSON *supported_versions = cJSON_GetObjectItemCaseSensitive(
        result, "supportedVersions");
    TEST_ASSERT_NOT_NULL(supported_versions);
    TEST_ASSERT_TRUE(cJSON_IsArray(supported_versions));
    TEST_ASSERT_EQUAL_STRING(
        "2026-07-28",
        cJSON_GetStringValue(cJSON_GetArrayItem(supported_versions, 0)));

    cJSON *capabilities = cJSON_GetObjectItemCaseSensitive(result, "capabilities");
    TEST_ASSERT_NOT_NULL(capabilities);
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(capabilities, "tools"));

    // serverInfo in result._meta
    cJSON *meta = cJSON_GetObjectItemCaseSensitive(result, "_meta");
    TEST_ASSERT_NOT_NULL(meta);
    cJSON *server_info = cJSON_GetObjectItemCaseSensitive(
        meta, "io.modelcontextprotocol/serverInfo");
    TEST_ASSERT_NOT_NULL(server_info);
    TEST_ASSERT_EQUAL_STRING(
        "esp32-ble-gateway",
        cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(server_info, "name")));

    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(result, "instructions"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(result, "ttlMs"));
    TEST_ASSERT_EQUAL_STRING(
        "private",
        cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(result, "cacheScope")));
    cJSON_Delete(response);
}

// ---------------------------------------------------------------------------
// Header/body consistency
// ---------------------------------------------------------------------------

TEST_CASE("missing Mcp-Method for MCP 2026 request returns -32020",
          "[mcp_conformance]")
{
    mcp_setup();
    io_reset("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\","
             "\"params\":{\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\","
             "\"io.modelcontextprotocol/clientCapabilities\":{}}}}");
    io_set_header("MCP-Protocol-Version", "2026-07-28");
    install_transport();
    memset(&g_req, 0, sizeof(g_req));
    g_req.content_len = (int)g_io.body_len;
    TEST_ASSERT_EQUAL_INT(0, mcp_handle_request(&g_req));

    TEST_ASSERT_EQUAL_STRING("400 Bad Request", g_io.status_line);
    cJSON *response = io_response_json();
    cJSON *error = cJSON_GetObjectItemCaseSensitive(response, "error");
    TEST_ASSERT_NOT_NULL(error);
    TEST_ASSERT_EQUAL_INT(-32020,
                          (int)cJSON_GetNumberValue(
                              cJSON_GetObjectItemCaseSensitive(error, "code")));
    cJSON_Delete(response);
}

TEST_CASE("Mcp-Method mismatch returns -32020",
          "[mcp_conformance]")
{
    mcp_setup();
    io_reset("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\","
             "\"params\":{\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\","
             "\"io.modelcontextprotocol/clientCapabilities\":{}}}}");
    io_set_header("MCP-Protocol-Version", "2026-07-28");
    io_set_header("Mcp-Method", "tools/call");
    install_transport();
    memset(&g_req, 0, sizeof(g_req));
    g_req.content_len = (int)g_io.body_len;
    TEST_ASSERT_EQUAL_INT(0, mcp_handle_request(&g_req));

    TEST_ASSERT_EQUAL_STRING("400 Bad Request", g_io.status_line);
    cJSON *response = io_response_json();
    cJSON *error = cJSON_GetObjectItemCaseSensitive(response, "error");
    TEST_ASSERT_NOT_NULL(error);
    TEST_ASSERT_EQUAL_INT(-32020,
                          (int)cJSON_GetNumberValue(
                              cJSON_GetObjectItemCaseSensitive(error, "code")));
    cJSON_Delete(response);
}

TEST_CASE("tools/call missing Mcp-Name returns -32020",
          "[mcp_conformance]")
{
    mcp_setup();
    io_reset("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"get_status\","
             "\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\","
             "\"io.modelcontextprotocol/clientCapabilities\":{}}}}");
    io_set_header("MCP-Protocol-Version", "2026-07-28");
    io_set_header("Mcp-Method", "tools/call");
    install_transport();
    memset(&g_req, 0, sizeof(g_req));
    g_req.content_len = (int)g_io.body_len;
    TEST_ASSERT_EQUAL_INT(0, mcp_handle_request(&g_req));

    TEST_ASSERT_EQUAL_STRING("400 Bad Request", g_io.status_line);
    cJSON *response = io_response_json();
    cJSON *error = cJSON_GetObjectItemCaseSensitive(response, "error");
    TEST_ASSERT_NOT_NULL(error);
    TEST_ASSERT_EQUAL_INT(-32020,
                          (int)cJSON_GetNumberValue(
                              cJSON_GetObjectItemCaseSensitive(error, "code")));
    cJSON_Delete(response);
}

TEST_CASE("tools/call Mcp-Name mismatch returns -32020",
          "[mcp_conformance]")
{
    mcp_setup();
    io_reset("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"get_status\","
             "\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\","
             "\"io.modelcontextprotocol/clientCapabilities\":{}}}}");
    io_set_header("MCP-Protocol-Version", "2026-07-28");
    io_set_header("Mcp-Method", "tools/call");
    io_set_header("Mcp-Name", "wrong_name");
    install_transport();
    memset(&g_req, 0, sizeof(g_req));
    g_req.content_len = (int)g_io.body_len;
    TEST_ASSERT_EQUAL_INT(0, mcp_handle_request(&g_req));

    TEST_ASSERT_EQUAL_STRING("400 Bad Request", g_io.status_line);
    cJSON *response = io_response_json();
    cJSON *error = cJSON_GetObjectItemCaseSensitive(response, "error");
    TEST_ASSERT_NOT_NULL(error);
    TEST_ASSERT_EQUAL_INT(
        -32020,
        (int)cJSON_GetNumberValue(
            cJSON_GetObjectItemCaseSensitive(error, "code")));
    cJSON_Delete(response);
}

// ---------------------------------------------------------------------------
// Required _meta validation
// ---------------------------------------------------------------------------

TEST_CASE("missing required _meta returns -32602",
          "[mcp_conformance]")
{
    mcp_setup();
    io_reset("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\","
             "\"params\":{}}");
    io_set_header("MCP-Protocol-Version", "2026-07-28");
    io_set_header("Mcp-Method", "tools/list");
    install_transport();
    memset(&g_req, 0, sizeof(g_req));
    g_req.content_len = (int)g_io.body_len;
    TEST_ASSERT_EQUAL_INT(0, mcp_handle_request(&g_req));

    cJSON *response = io_response_json();
    cJSON *error = cJSON_GetObjectItemCaseSensitive(response, "error");
    TEST_ASSERT_NOT_NULL(error);
    TEST_ASSERT_EQUAL_INT(-32602,
                          (int)cJSON_GetNumberValue(
                              cJSON_GetObjectItemCaseSensitive(error, "code")));
    cJSON_Delete(response);
}

TEST_CASE("missing _meta.protocolVersion returns -32602",
          "[mcp_conformance]")
{
    mcp_setup();
    io_reset("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\","
             "\"params\":{\"_meta\":{\"io.modelcontextprotocol/clientCapabilities\":{}}}}");
    io_set_header("MCP-Protocol-Version", "2026-07-28");
    io_set_header("Mcp-Method", "tools/list");
    install_transport();
    memset(&g_req, 0, sizeof(g_req));
    g_req.content_len = (int)g_io.body_len;
    TEST_ASSERT_EQUAL_INT(0, mcp_handle_request(&g_req));

    cJSON *response = io_response_json();
    cJSON *error = cJSON_GetObjectItemCaseSensitive(response, "error");
    TEST_ASSERT_NOT_NULL(error);
    TEST_ASSERT_EQUAL_INT(-32602,
                          (int)cJSON_GetNumberValue(
                              cJSON_GetObjectItemCaseSensitive(error, "code")));
    cJSON_Delete(response);
}

TEST_CASE("missing _meta.clientCapabilities returns -32602",
          "[mcp_conformance]")
{
    mcp_setup();
    io_reset("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\","
             "\"params\":{\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\"}}}");
    io_set_header("MCP-Protocol-Version", "2026-07-28");
    io_set_header("Mcp-Method", "tools/list");
    install_transport();
    memset(&g_req, 0, sizeof(g_req));
    g_req.content_len = (int)g_io.body_len;
    TEST_ASSERT_EQUAL_INT(0, mcp_handle_request(&g_req));

    cJSON *response = io_response_json();
    cJSON *error = cJSON_GetObjectItemCaseSensitive(response, "error");
    TEST_ASSERT_NOT_NULL(error);
    TEST_ASSERT_EQUAL_INT(-32602,
                          (int)cJSON_GetNumberValue(
                              cJSON_GetObjectItemCaseSensitive(error, "code")));
    cJSON_Delete(response);
}

// ---------------------------------------------------------------------------
// Unknown method -> HTTP 404 + JSON-RPC -32601
// ---------------------------------------------------------------------------

TEST_CASE("unknown method returns HTTP 404 and -32601",
          "[mcp_conformance]")
{
    mcp_setup();
    io_reset("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"nonexistent/method\"," 
             "\"params\":{\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\"," 
             "\"io.modelcontextprotocol/clientCapabilities\":{}}}}");
    io_set_header("MCP-Protocol-Version", "2026-07-28");
    io_set_header("Mcp-Method", "nonexistent/method");
    install_transport();
    memset(&g_req, 0, sizeof(g_req));
    g_req.content_len = (int)g_io.body_len;
    TEST_ASSERT_EQUAL_INT(0, mcp_handle_request(&g_req));

    TEST_ASSERT_EQUAL_STRING("404 Not Found", g_io.status_line);
    cJSON *response = io_response_json();
    cJSON *error = cJSON_GetObjectItemCaseSensitive(response, "error");
    TEST_ASSERT_NOT_NULL(error);
    TEST_ASSERT_EQUAL_INT(-32601,
                          (int)cJSON_GetNumberValue(
                              cJSON_GetObjectItemCaseSensitive(error, "code")));
    cJSON_Delete(response);
}

// ---------------------------------------------------------------------------
// tools/list surface (no admin tools, no tool_names)
// ---------------------------------------------------------------------------

TEST_CASE("tools/list only contains control profile tools",
          "[mcp_conformance]")
{
    mcp_setup();
    io_reset("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\","
             "\"params\":{\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\","
             "\"io.modelcontextprotocol/clientCapabilities\":{}}}}");
    io_set_header("MCP-Protocol-Version", "2026-07-28");
    io_set_header("Mcp-Method", "tools/list");
    install_transport();
    memset(&g_req, 0, sizeof(g_req));
    g_req.content_len = (int)g_io.body_len;
    TEST_ASSERT_EQUAL_INT(0, mcp_handle_request(&g_req));

    cJSON *response = io_response_json();
    cJSON *result = cJSON_GetObjectItemCaseSensitive(response, "result");
    TEST_ASSERT_NOT_NULL(result);

    cJSON *tools = cJSON_GetObjectItemCaseSensitive(result, "tools");
    TEST_ASSERT_NOT_NULL(tools);
    TEST_ASSERT_TRUE(cJSON_IsArray(tools));

    // 4 control profile tools only
    TEST_ASSERT_EQUAL_INT(4, cJSON_GetArraySize(tools));

    // No tool_names on wire (§12.9)
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(result, "tool_names"));

    // Verify no admin tools
    for (int i = 0; i < cJSON_GetArraySize(tools); i++) {
        cJSON *tool = cJSON_GetArrayItem(tools, i);
        const char *name =
            cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(tool, "name"));
        TEST_ASSERT_NOT_NULL(name);
        TEST_ASSERT_TRUE(strcmp(name, "add_device") != 0);
        TEST_ASSERT_TRUE(strcmp(name, "edit_device") != 0);
        TEST_ASSERT_TRUE(strcmp(name, "delete_device") != 0);
    }
    cJSON_Delete(response);
}

// ---------------------------------------------------------------------------
// MCP 2025 compatibility tests
// ---------------------------------------------------------------------------

TEST_CASE("initialize with exact supported version returns InitializeResult",
          "[mcp_2025]")
{
    mcp_setup();
    io_reset("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
             "\"params\":{\"protocolVersion\":\"2025-11-25\","
             "\"capabilities\":{},"
             "\"clientInfo\":{\"name\":\"test-client\",\"version\":\"1.0\"}}}");
    install_transport();
    memset(&g_req, 0, sizeof(g_req));
    g_req.content_len = (int)g_io.body_len;
    TEST_ASSERT_EQUAL_INT(0, mcp_handle_request(&g_req));

    cJSON *response = io_response_json();
    cJSON *result = cJSON_GetObjectItemCaseSensitive(response, "result");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(response, "error"));

    TEST_ASSERT_EQUAL_STRING(
        "2025-11-25",
        cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(result, "protocolVersion")));

    cJSON *capabilities = cJSON_GetObjectItemCaseSensitive(result, "capabilities");
    TEST_ASSERT_NOT_NULL(capabilities);
    cJSON *tools = cJSON_GetObjectItemCaseSensitive(capabilities, "tools");
    TEST_ASSERT_NOT_NULL(tools);

    cJSON *server_info = cJSON_GetObjectItemCaseSensitive(result, "serverInfo");
    TEST_ASSERT_NOT_NULL(server_info);
    TEST_ASSERT_EQUAL_STRING("esp32-ble-gateway",
                             cJSON_GetStringValue(
                                 cJSON_GetObjectItemCaseSensitive(server_info, "name")));
    cJSON_Delete(response);
}

TEST_CASE("initialize with different version gets counter-offered",
          "[mcp_2025]")
{
    mcp_setup();
    io_reset("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
             "\"params\":{\"protocolVersion\":\"2025-06-18\","
             "\"capabilities\":{},"
             "\"clientInfo\":{\"name\":\"old-client\",\"version\":\"1.0\"}}}");
    install_transport();
    memset(&g_req, 0, sizeof(g_req));
    g_req.content_len = (int)g_io.body_len;
    TEST_ASSERT_EQUAL_INT(0, mcp_handle_request(&g_req));

    cJSON *response = io_response_json();
    cJSON *result = cJSON_GetObjectItemCaseSensitive(response, "result");
    TEST_ASSERT_NOT_NULL(result);
    // Counter-offer: server returns 2025-11-25 regardless of client proposal
    TEST_ASSERT_EQUAL_STRING(
        "2025-11-25",
        cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(result, "protocolVersion")));
    cJSON_Delete(response);
}

TEST_CASE("notifications/initialized returns 202 Accepted",
          "[mcp_2025]")
{
    mcp_setup();
    io_reset("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
    install_transport();
    memset(&g_req, 0, sizeof(g_req));
    g_req.content_len = (int)g_io.body_len;
    TEST_ASSERT_EQUAL_INT(0, mcp_handle_request(&g_req));
    TEST_ASSERT_EQUAL_STRING("202 Accepted", g_io.status_line);
    TEST_ASSERT_EQUAL_UINT32(0, g_io.response_len);
}

TEST_CASE("notifications/initialized with version header returns 202",
          "[mcp_2025]")
{
    mcp_setup();
    io_reset("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
    io_set_header("MCP-Protocol-Version", "2025-11-25");
    install_transport();
    memset(&g_req, 0, sizeof(g_req));
    g_req.content_len = (int)g_io.body_len;
    TEST_ASSERT_EQUAL_INT(0, mcp_handle_request(&g_req));
    TEST_ASSERT_EQUAL_STRING("202 Accepted", g_io.status_line);
}

TEST_CASE("2025 tools/list returns proper MCP shape",
          "[mcp_2025]")
{
    mcp_setup();
    io_reset("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}");
    io_set_header("MCP-Protocol-Version", "2025-11-25");
    install_transport();
    memset(&g_req, 0, sizeof(g_req));
    g_req.content_len = (int)g_io.body_len;
    TEST_ASSERT_EQUAL_INT(0, mcp_handle_request(&g_req));

    cJSON *response = io_response_json();
    cJSON *result = cJSON_GetObjectItemCaseSensitive(response, "result");
    TEST_ASSERT_NOT_NULL(result);

    // 2025 does NOT have resultType
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(result, "resultType"));
    // 2025 does NOT have tool_names
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(result, "tool_names"));

    cJSON *tools = cJSON_GetObjectItemCaseSensitive(result, "tools");
    TEST_ASSERT_NOT_NULL(tools);
    TEST_ASSERT_TRUE(cJSON_IsArray(tools));
    cJSON_Delete(response);
}

TEST_CASE("2025 tools/call returns CallToolResult with content and isError",
          "[mcp_2025]")
{
    mcp_setup();
    io_reset("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"get_status\"}}");
    io_set_header("MCP-Protocol-Version", "2025-11-25");
    install_transport();
    memset(&g_req, 0, sizeof(g_req));
    g_req.content_len = (int)g_io.body_len;
    TEST_ASSERT_EQUAL_INT(0, mcp_handle_request(&g_req));

    cJSON *response = io_response_json();
    cJSON *result = cJSON_GetObjectItemCaseSensitive(response, "result");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(response, "error"));

    // CallToolResult shape
    cJSON *content = cJSON_GetObjectItemCaseSensitive(result, "content");
    TEST_ASSERT_NOT_NULL(content);
    TEST_ASSERT_TRUE(cJSON_IsArray(content));
    TEST_ASSERT_FALSE(cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(result, "isError")));

    cJSON *first = cJSON_GetArrayItem(content, 0);
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_EQUAL_STRING("text",
                             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(first, "type")));
    cJSON_Delete(response);
}
