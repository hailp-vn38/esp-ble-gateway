#include <string.h>

#include "cJSON.h"
#include "unity.h"

#include "cbor_codec.h"
#include "command_dispatcher.h"
#include "../mcp_endpoint_internal.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void ensure_dispatcher(void)
{
    int rc = command_dispatcher_init();
    TEST_ASSERT_TRUE(rc == 0 || rc == ESP_ERR_INVALID_STATE);
    TEST_ASSERT_EQUAL_INT(0, command_dispatcher_freeze_registry());
}

static cJSON *make_params(const char *json_str)
{
    cJSON *obj = cJSON_Parse(json_str);
    TEST_ASSERT_NOT_NULL(obj);
    return obj;
}

// ---------------------------------------------------------------------------
// mcp_tools_list
// ---------------------------------------------------------------------------

TEST_CASE("mcp_tools_list returns tools array", "[mcp_endpoint]")
{
    ensure_dispatcher();
    cJSON *result = mcp_tools_list();
    TEST_ASSERT_NOT_NULL(result);

    cJSON *tools = cJSON_GetObjectItemCaseSensitive(result, "tools");
    TEST_ASSERT_TRUE(cJSON_IsArray(tools));
    TEST_ASSERT_GREATER_THAN(0, cJSON_GetArraySize(tools));

    // Each entry must have name, description, inputSchema
    cJSON *first = cJSON_GetArrayItem(tools, 0);
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(first, "name"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(first, "description"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(first, "inputSchema"));

    cJSON_Delete(result);
}

TEST_CASE("mcp_tools_list tool_names matches tools length", "[mcp_endpoint]")
{
    ensure_dispatcher();
    cJSON *result = mcp_tools_list();
    TEST_ASSERT_NOT_NULL(result);

    cJSON *tools = cJSON_GetObjectItemCaseSensitive(result, "tools");
    cJSON *names = cJSON_GetObjectItemCaseSensitive(result, "tool_names");
    TEST_ASSERT_TRUE(cJSON_IsArray(names));
    TEST_ASSERT_EQUAL_INT(cJSON_GetArraySize(tools), cJSON_GetArraySize(names));

    cJSON_Delete(result);
}

// ---------------------------------------------------------------------------
// mcp_tools_call — happy paths via gateway commands
// ---------------------------------------------------------------------------

TEST_CASE("mcp_tools_call list_devices succeeds", "[mcp_endpoint]")
{
    ensure_dispatcher();
    cJSON *params = make_params("{\"name\":\"list_devices\"}");
    mcp_rpc_error_t err = {0};
    cJSON *result = mcp_tools_call(params, &err);
    cJSON_Delete(params);

    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQUAL_INT(0, err.code);
    // Current wire format: {success, message, data?}
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(result, "success"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(result, "message"));
    cJSON_Delete(result);
}

TEST_CASE("mcp_tools_call get_status succeeds", "[mcp_endpoint]")
{
    ensure_dispatcher();
    cJSON *params = make_params("{\"name\":\"get_status\"}");
    mcp_rpc_error_t err = {0};
    cJSON *result = mcp_tools_call(params, &err);
    cJSON_Delete(params);

    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQUAL_INT(0, err.code);
    cJSON_Delete(result);
}

// ---------------------------------------------------------------------------
// mcp_tools_call — error paths
// ---------------------------------------------------------------------------

TEST_CASE("mcp_tools_call params not object returns -32602", "[mcp_endpoint]")
{
    ensure_dispatcher();
    cJSON *params = cJSON_CreateArray(); // not an object
    mcp_rpc_error_t err = {0};
    cJSON *result = mcp_tools_call(params, &err);
    cJSON_Delete(params);

    TEST_ASSERT_NULL(result);
    TEST_ASSERT_EQUAL_INT(-32602, err.code);
}

TEST_CASE("mcp_tools_call missing name/command returns -32602", "[mcp_endpoint]")
{
    ensure_dispatcher();
    cJSON *params = make_params("{}");
    mcp_rpc_error_t err = {0};
    cJSON *result = mcp_tools_call(params, &err);
    cJSON_Delete(params);

    TEST_ASSERT_NULL(result);
    TEST_ASSERT_EQUAL_INT(-32602, err.code);
}

TEST_CASE("mcp_tools_call arguments not object returns -32602", "[mcp_endpoint]")
{
    ensure_dispatcher();
    cJSON *params = make_params("{\"name\":\"get_status\",\"arguments\":42}");
    mcp_rpc_error_t err = {0};
    cJSON *result = mcp_tools_call(params, &err);
    cJSON_Delete(params);

    TEST_ASSERT_NULL(result);
    TEST_ASSERT_EQUAL_INT(-32602, err.code);
}

TEST_CASE("mcp_tools_call invalid type returns -32602", "[mcp_endpoint]")
{
    ensure_dispatcher();
    cJSON *params = make_params("{\"name\":\"get_status\",\"type\":\"bad_type\"}");
    mcp_rpc_error_t err = {0};
    cJSON *result = mcp_tools_call(params, &err);
    cJSON_Delete(params);

    TEST_ASSERT_NULL(result);
    TEST_ASSERT_EQUAL_INT(-32602, err.code);
}

TEST_CASE("mcp_tools_call NULL params returns -32602", "[mcp_endpoint]")
{
    ensure_dispatcher();
    mcp_rpc_error_t err = {0};
    cJSON *result = mcp_tools_call(NULL, &err);
    TEST_ASSERT_NULL(result);
    TEST_ASSERT_EQUAL_INT(-32602, err.code);
}

// NOTE: Hidden command surface (unknown tool + device_id -> device_command)
// is not tested here because device_command_handle calls ble_central_is_connected
// which requires BLE hardware. It is documented in the plan and will be fixed
// in Step 4 by removing the fallback path entirely.
