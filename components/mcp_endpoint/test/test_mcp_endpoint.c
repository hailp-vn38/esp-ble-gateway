#include <limits.h>
#include <string.h>

#include "cJSON.h"
#include "unity.h"

#include "../command_dispatcher_internal.h"
#include "cbor_codec.h"
#include "command_dispatcher.h"
#include "device_store.h"

#include "test_mcp_transport.h"

// ---------------------------------------------------------------------------
// Fixtures (Unity setUp/tearDown live in test_main.c)
// ---------------------------------------------------------------------------

static void mcp_setup(void)
{
    command_dispatcher_reset_for_test();
    TEST_ASSERT_EQUAL_INT(0, command_dispatcher_init());
    TEST_ASSERT_EQUAL_INT(0, command_dispatcher_freeze_registry());
    mcp_auth_reset_rate_limit();
}

static bool s_mock_send_ok = true;
static bool s_mock_ack_completes = true;
static gw_message_t s_captured_wire;

static int dev_mock_is_connected(const char *device_id)
{
    return device_id != NULL && device_id[0] != '\0' ? 1 : 0;
}

static int dev_mock_send(const char *device_id, const gw_message_t *msg)
{
    (void)device_id;
    if (!s_mock_send_ok) return -1;
    s_captured_wire = *msg;
    if (s_mock_ack_completes) {
        gw_message_t ack = {.protocol_version = GW_PROTOCOL_VERSION};
        strlcpy(ack.type, "device_ack", sizeof(ack.type));
        strlcpy(ack.device_id, msg->device_id, sizeof(ack.device_id));
        strlcpy(ack.command, msg->command, sizeof(ack.command));
        ack.has_device_id = 1;
        ack.has_request_id = 1;
        ack.bool_value = 1;
        ack.request_id = msg->request_id;
        command_dispatcher_on_device_notify(msg->device_id, &ack);
    }
    return 0;
}

static void install_device_hooks(void)
{
    static const device_command_hooks_t hooks = {
        .send_command = dev_mock_send,
        .is_connected = dev_mock_is_connected,
    };
    device_command_set_hooks(&hooks);
    s_mock_send_ok = true;
    s_mock_ack_completes = true;
}

// ---------------------------------------------------------------------------
// tools/list
// ---------------------------------------------------------------------------

TEST_CASE("tools/list returns registered tools with schemas", "[mcp_endpoint]")
{
    mcp_setup();
    TEST_ASSERT_EQUAL_INT(0, run_request(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}"));
    cJSON *response = io_response_json();

    cJSON *result = cJSON_GetObjectItemCaseSensitive(response, "result");
    TEST_ASSERT_NOT_NULL(result);
    cJSON *tools = cJSON_GetObjectItemCaseSensitive(result, "tools");
    TEST_ASSERT_TRUE(cJSON_IsArray(tools));
    TEST_ASSERT_EQUAL_INT(6, cJSON_GetArraySize(tools));

    cJSON *first = cJSON_GetArrayItem(tools, 0);
    TEST_ASSERT_EQUAL_STRING("add_device",
                             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(first, "name")));
    cJSON *schema = cJSON_GetObjectItemCaseSensitive(first, "inputSchema");
    TEST_ASSERT_TRUE(cJSON_IsObject(schema));

    cJSON_Delete(response);
}

TEST_CASE("tools/list exposes annotations from the registry", "[mcp_endpoint]")
{
    mcp_setup();
    TEST_ASSERT_EQUAL_INT(0, run_request(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}"));
    cJSON *response = io_response_json();
    cJSON *tools = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(response, "result"), "tools");

    bool saw_destructive = false;
    cJSON *tool = NULL;
    cJSON_ArrayForEach(tool, tools) {
        cJSON *annotations =
            cJSON_GetObjectItemCaseSensitive(tool, "annotations");
        TEST_ASSERT_TRUE(cJSON_IsObject(annotations));
        const char *name = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(tool, "name"));
        if (strcmp(name, "delete_device") == 0) {
            saw_destructive = true;
            TEST_ASSERT_TRUE(cJSON_IsTrue(
                cJSON_GetObjectItemCaseSensitive(annotations, "destructiveHint")));
        }
    }
    TEST_ASSERT_TRUE(saw_destructive);
    cJSON_Delete(response);
}

TEST_CASE("legacy alias list_tools still works", "[mcp_endpoint]")
{
    mcp_setup();
    TEST_ASSERT_EQUAL_INT(0, run_request(
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"list_tools\"}"));
    cJSON *response = io_response_json();
    TEST_ASSERT_NOT_NULL(
        cJSON_GetObjectItemCaseSensitive(response, "result"));
    cJSON_Delete(response);
}

// ---------------------------------------------------------------------------
// tools/call — gateway commands
// ---------------------------------------------------------------------------

TEST_CASE("tools/call get_status dispatches and returns result",
          "[mcp_endpoint]")
{
    mcp_setup();
    TEST_ASSERT_EQUAL_INT(0, run_request(
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"get_status\"}}"));
    cJSON *response = io_response_json();

    cJSON *result = cJSON_GetObjectItemCaseSensitive(response, "result");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(result, "success"));
    cJSON_Delete(response);
}

TEST_CASE("legacy params.command form resolves registered commands",
          "[mcp_endpoint]")
{
    mcp_setup();
    TEST_ASSERT_EQUAL_INT(0, run_request(
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"call_tool\","
        "\"params\":{\"command\":\"get_status\"}}"));
    cJSON *response = io_response_json();
    TEST_ASSERT_NOT_NULL(
        cJSON_GetObjectItemCaseSensitive(response, "result"));
    cJSON_Delete(response);
}

TEST_CASE("unknown tool is a protocol error -32602", "[mcp_endpoint]")
{
    mcp_setup();
    TEST_ASSERT_EQUAL_INT(0, run_request(
        "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"nope\",\"device_id\":\"lamp-1\"}}"));
    cJSON *response = io_response_json();

    cJSON *error = cJSON_GetObjectItemCaseSensitive(response, "error");
    TEST_ASSERT_NOT_NULL(error);
    TEST_ASSERT_EQUAL_INT(-32602,
                          (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(error, "code")));
    cJSON_Delete(response);
}

// ---------------------------------------------------------------------------
// JSON-RPC validation
// ---------------------------------------------------------------------------

TEST_CASE("non-object JSON root is Invalid Request -32600, not Parse error",
          "[mcp_endpoint]")
{
    mcp_setup();
    TEST_ASSERT_EQUAL_INT(0, run_request("[1,2,3]"));
    cJSON *response = io_response_json();
    TEST_ASSERT_EQUAL_INT(-32600,
                          (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(
                              cJSON_GetObjectItemCaseSensitive(response, "error"),
                              "code")));
    cJSON_Delete(response);
}

TEST_CASE("malformed JSON is Parse error -32700", "[mcp_endpoint]")
{
    mcp_setup();
    TEST_ASSERT_EQUAL_INT(0, run_request("{\"jsonrpc\":"));
    cJSON *response = io_response_json();
    TEST_ASSERT_EQUAL_INT(-32700,
                          (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(
                              cJSON_GetObjectItemCaseSensitive(response, "error"),
                              "code")));
    cJSON_Delete(response);
}

TEST_CASE("wrong jsonrpc version is rejected with -32600", "[mcp_endpoint]")
{
    mcp_setup();
    TEST_ASSERT_EQUAL_INT(0, run_request(
        "{\"jsonrpc\":\"1.0\",\"id\":1,\"method\":\"tools/list\"}"));
    cJSON *response = io_response_json();
    TEST_ASSERT_EQUAL_INT(-32600,
                          (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(
                              cJSON_GetObjectItemCaseSensitive(response, "error"),
                              "code")));
    cJSON_Delete(response);
}

TEST_CASE("unknown method returns -32601", "[mcp_endpoint]")
{
    mcp_setup();
    TEST_ASSERT_EQUAL_INT(0, run_request(
        "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"resources/list\"}"));
    cJSON *response = io_response_json();
    TEST_ASSERT_EQUAL_INT(-32601,
                          (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(
                              cJSON_GetObjectItemCaseSensitive(response, "error"),
                              "code")));
    cJSON_Delete(response);
}

TEST_CASE("id string, number and null are echoed back", "[mcp_endpoint]")
{
    mcp_setup();
    const char *cases[] = {
        "{\"jsonrpc\":\"2.0\",\"id\":\"abc\",\"method\":\"tools/list\"}",
        "{\"jsonrpc\":\"2.0\",\"id\":42,\"method\":\"tools/list\"}",
        "{\"jsonrpc\":\"2.0\",\"id\":null,\"method\":\"tools/list\"}",
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        TEST_ASSERT_EQUAL_INT(0, run_request(cases[i]));
        cJSON *response = io_response_json();
        cJSON *id = cJSON_GetObjectItemCaseSensitive(response, "id");
        if (i == 0) TEST_ASSERT_EQUAL_STRING("abc", cJSON_GetStringValue(id));
        else if (i == 1) TEST_ASSERT_EQUAL_INT(42, (int)cJSON_GetNumberValue(id));
        else TEST_ASSERT_TRUE(cJSON_IsNull(id));
        cJSON_Delete(response);
    }
}

// ---------------------------------------------------------------------------
// Notifications
// ---------------------------------------------------------------------------

TEST_CASE("notification without id gets 204 No Content", "[mcp_endpoint]")
{
    mcp_setup();
    TEST_ASSERT_EQUAL_INT(0, run_request(
        "{\"jsonrpc\":\"2.0\",\"method\":\"tools/list\"}"));
    TEST_ASSERT_EQUAL_INT(1, g_io.responses_sent);
    TEST_ASSERT_EQUAL_UINT32(0, g_io.response_len);
    TEST_ASSERT_EQUAL_STRING("204 No Content", g_io.status_line);
}

// ---------------------------------------------------------------------------
// Receive path: limits, timeouts, retries
// ---------------------------------------------------------------------------

TEST_CASE("oversize body is 413 with Connection close and no body read",
          "[mcp_endpoint]")
{
    mcp_setup();
    io_reset("{}");
    install_transport();
    memset(&g_req, 0, sizeof(g_req));
    g_req.content_len = 4097;
    TEST_ASSERT_EQUAL_INT(0, mcp_handle_request(&g_req));

    TEST_ASSERT_EQUAL_STRING("413 Content Too Large", g_io.status_line);
    TEST_ASSERT_TRUE(g_io.connection_closed);
    TEST_ASSERT_EQUAL_INT(0, g_io.recv_calls); // body never read
    TEST_ASSERT_EQUAL_INT(1, g_io.responses_sent);

    cJSON *response = cJSON_Parse(g_io.response);
    TEST_ASSERT_EQUAL_INT(-32600,
                          (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(
                              cJSON_GetObjectItemCaseSensitive(response, "error"),
                              "code")));
    cJSON_Delete(response);
}

TEST_CASE("empty body is Invalid Request -32600", "[mcp_endpoint]")
{
    mcp_setup();
    TEST_ASSERT_EQUAL_INT(0, run_request(""));
    cJSON *response = io_response_json();
    TEST_ASSERT_EQUAL_INT(-32600,
                          (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(
                              cJSON_GetObjectItemCaseSensitive(response, "error"),
                              "code")));
    cJSON_Delete(response);
}

TEST_CASE("recv timeout retries are bounded at three", "[mcp_endpoint]")
{
    mcp_setup();
    io_reset("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}");
    g_io.timeouts_before_data = INT_MAX; // never delivers any data
    install_transport();
    memset(&g_req, 0, sizeof(g_req));
    g_req.content_len = (int)g_io.body_len;
    TEST_ASSERT_EQUAL_INT(0, mcp_handle_request(&g_req));

    // 1 initial attempt + 3 retries, then give up: no infinite loop.
    TEST_ASSERT_EQUAL_INT(4, g_io.recv_calls);
    TEST_ASSERT_EQUAL_INT(1, g_io.responses_sent);
}

TEST_CASE("peer socket error mid-body aborts the read", "[mcp_endpoint]")
{
    mcp_setup();
    io_reset("{\"jsonrpc\":\"2.0\"");
    g_io.recv_fail_after = 1; // first recv ok, second fails
    install_transport();
    memset(&g_req, 0, sizeof(g_req));
    g_req.content_len = 100;
    TEST_ASSERT_EQUAL_INT(0, mcp_handle_request(&g_req));
    TEST_ASSERT_EQUAL_INT(2, g_io.recv_calls);
    TEST_ASSERT_EQUAL_INT(1, g_io.responses_sent);
    TEST_ASSERT_EQUAL_INT(-32700,
                          (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(
                              cJSON_GetObjectItemCaseSensitive(io_response_json(),
                                                               "error"),
                              "code")));
}

// ---------------------------------------------------------------------------
// Auth gate
// ---------------------------------------------------------------------------

TEST_CASE("wrong content type is 415 with connection close", "[mcp_endpoint]")
{
    mcp_setup();
    io_reset("{}");
    io_set_header("Content-Type", "text/plain");
    install_transport();
    memset(&g_req, 0, sizeof(g_req));
    g_req.content_len = (int)g_io.body_len;
    TEST_ASSERT_EQUAL_INT(0, mcp_handle_request(&g_req));

    TEST_ASSERT_EQUAL_STRING("415 Unsupported Media Type", g_io.status_line);
    TEST_ASSERT_TRUE(g_io.connection_closed);
    TEST_ASSERT_EQUAL_INT(0, g_io.recv_calls);
}

TEST_CASE("host outside allowlist is 403", "[mcp_endpoint]")
{
    mcp_setup();
    io_reset("{}");
    io_set_header("Host", "evil.example.com");
    install_transport();
    memset(&g_req, 0, sizeof(g_req));
    g_req.content_len = (int)g_io.body_len;
    TEST_ASSERT_EQUAL_INT(0, mcp_handle_request(&g_req));

    TEST_ASSERT_EQUAL_STRING("403 Forbidden", g_io.status_line);
    TEST_ASSERT_TRUE(g_io.connection_closed);
}

TEST_CASE("host allowlist ignores port and case", "[mcp_endpoint]")
{
    mcp_setup();
    io_reset("{}");
    io_set_header("Host", "GATEWAY.LOCAL:8080");
    install_transport();
    memset(&g_req, 0, sizeof(g_req));
    g_req.content_len = (int)g_io.body_len;
    TEST_ASSERT_EQUAL_INT(0, mcp_handle_request(&g_req));
    TEST_ASSERT_EQUAL_INT(1, g_io.responses_sent); // normal response followed
}

TEST_CASE("cross-origin request is forbidden", "[mcp_endpoint]")
{
    mcp_setup();
    io_reset("{}");
    io_set_header("Origin", "http://evil.example.com/payload.html");
    install_transport();
    memset(&g_req, 0, sizeof(g_req));
    g_req.content_len = (int)g_io.body_len;
    TEST_ASSERT_EQUAL_INT(0, mcp_handle_request(&g_req));
    TEST_ASSERT_EQUAL_STRING("403 Forbidden", g_io.status_line);
}

TEST_CASE("rate limiter rejects the burst beyond capacity with 429",
          "[mcp_endpoint]")
{
    mcp_setup();
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_EQUAL_INT(0, run_request(
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}"));
    }
    TEST_ASSERT_EQUAL_INT(0, run_request(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}"));
    TEST_ASSERT_EQUAL_STRING("429 Too Many Requests", g_io.status_line);
}

// ---------------------------------------------------------------------------
// device_command routing
// ---------------------------------------------------------------------------

TEST_CASE("allowlisted device command executes through the dispatcher",
          "[mcp_endpoint]")
{
    mcp_setup();
    install_device_hooks();
    TEST_ASSERT_EQUAL_INT(0, run_request(
        "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"device_command\","
        "\"arguments\":{\"device_id\":\"relay-1\",\"command\":\"toggle\"}}}"));
    cJSON *response = io_response_json();
    cJSON *result = cJSON_GetObjectItemCaseSensitive(response, "result");
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(response, "error"));
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(result, "success"));
    cJSON_Delete(response);
}

TEST_CASE("command outside the allowlist is a tool error, not protocol error",
          "[mcp_endpoint]")
{
    mcp_setup();
    TEST_ASSERT_EQUAL_INT(0, run_request(
        "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"device_command\","
        "\"arguments\":{\"device_id\":\"relay-1\",\"command\":\"factory_reset\"}}}"));
    cJSON *response = io_response_json();

    // Tool-level failure stays inside result; no JSON-RPC error object.
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(response, "error"));
    cJSON *result = cJSON_GetObjectItemCaseSensitive(response, "result");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(
        cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "success")));
    cJSON_Delete(response);
}

TEST_CASE("device_command without command field is -32602", "[mcp_endpoint]")
{
    mcp_setup();
    TEST_ASSERT_EQUAL_INT(0, run_request(
        "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"device_command\","
        "\"arguments\":{\"device_id\":\"relay-1\"}}}"));
    cJSON *response = io_response_json();
    TEST_ASSERT_EQUAL_INT(-32602,
                          (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(
                              cJSON_GetObjectItemCaseSensitive(response, "error"),
                              "code")));
    cJSON_Delete(response);
}
