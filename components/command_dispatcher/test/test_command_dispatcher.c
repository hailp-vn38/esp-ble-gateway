#include <string.h>

#include "cJSON.h"
#include "unity.h"

#include "command_dispatcher.h"
#include "device_store.h"

static void test_command(const gw_message_t *message, dispatch_result_t *result)
{
    result->success = strcmp(message->command, "test_command") == 0;
    strlcpy(result->message, "custom result", sizeof(result->message));
}

TEST_CASE("dispatcher registry is dynamic and rejects duplicates", "[dispatcher]")
{
    TEST_ASSERT_EQUAL_INT(0, command_dispatcher_init());
    TEST_ASSERT_TRUE(command_dispatcher_is_registered("list_devices"));
    TEST_ASSERT_EQUAL_INT(0,
                          command_dispatcher_register("test_command", test_command));
    TEST_ASSERT_EQUAL_INT(-1,
                          command_dispatcher_register("test_command", test_command));

    const char *names[DISPATCHER_MAX_COMMANDS];
    int count = command_dispatcher_get_registered_names(names,
                                                        DISPATCHER_MAX_COMMANDS);
    TEST_ASSERT_EQUAL_INT(6, count);
    TEST_ASSERT_EQUAL_STRING("test_command", names[count - 1]);
}

TEST_CASE("dispatcher returns a JSON device snapshot", "[dispatcher]")
{
    TEST_ASSERT_EQUAL_INT(0, command_dispatcher_init());
    TEST_ASSERT_EQUAL_INT(0, device_store_add("plug-1", "Desk", "switch"));
    gw_message_t message = {.protocol_version = GW_PROTOCOL_VERSION};
    strlcpy(message.type, "gateway_command", sizeof(message.type));
    strlcpy(message.command, "list_devices", sizeof(message.command));

    dispatch_result_t result;
    command_dispatcher_handle(&message, &result);
    TEST_ASSERT_TRUE(result.success);
    cJSON *array = cJSON_Parse(result.message);
    TEST_ASSERT_TRUE(cJSON_IsArray(array));
    TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(array));
    cJSON *entry = cJSON_GetArrayItem(array, 0);
    TEST_ASSERT_EQUAL_STRING(
        "plug-1", cJSON_GetObjectItemCaseSensitive(entry, "device_id")->valuestring);
    cJSON_Delete(array);
}

TEST_CASE("dispatcher reports unknown gateway command", "[dispatcher]")
{
    TEST_ASSERT_EQUAL_INT(0, command_dispatcher_init());
    gw_message_t message = {.protocol_version = GW_PROTOCOL_VERSION};
    strlcpy(message.type, "gateway_command", sizeof(message.type));
    strlcpy(message.command, "does_not_exist", sizeof(message.command));
    dispatch_result_t result;
    command_dispatcher_handle(&message, &result);
    TEST_ASSERT_FALSE(result.success);
    TEST_ASSERT_NOT_NULL(strstr(result.message, "Unknown gateway command"));
}
