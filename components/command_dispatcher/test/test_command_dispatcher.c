#include <string.h>

#include "cJSON.h"
#include "unity.h"

#include "../command_dispatcher_internal.h"
#include "../device_request_manager.h"
#include "command_dispatcher.h"
#include "device_store.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static gw_message_t s_captured_wire;
static bool s_mock_send_succeeds;
static bool s_mock_send_completes;
static bool s_mock_use_stale_request_id;
static uint32_t s_stale_request_id;

static int mock_is_connected(const char *device_id)
{
    return device_id != NULL && device_id[0] != '\0' ? 1 : 0;
}

static int mock_send(const char *device_id, const gw_message_t *msg)
{
    (void)device_id;
    if (!s_mock_send_succeeds) return -1;
    s_captured_wire = *msg;
    if (s_mock_send_completes) {
        gw_message_t ack = {.protocol_version = GW_PROTOCOL_VERSION};
        strlcpy(ack.type, "device_ack", sizeof(ack.type));
        strlcpy(ack.device_id, msg->device_id, sizeof(ack.device_id));
        strlcpy(ack.command, msg->command, sizeof(ack.command));
        ack.has_device_id = 1;
        ack.has_request_id = 1;
        ack.bool_value = 1;
        ack.request_id = s_mock_use_stale_request_id ? s_stale_request_id
                                                     : msg->request_id;
        command_dispatcher_on_device_notify(msg->device_id, &ack);
    }
    return 0;
}

static const device_command_hooks_t s_mock_hooks = {
    .send_command = mock_send,
    .is_connected = mock_is_connected,
};

static void fresh_frozen_dispatcher(void)
{
    command_dispatcher_reset_for_test();
    TEST_ASSERT_EQUAL_INT(0, command_dispatcher_init());
    TEST_ASSERT_EQUAL_INT(0, command_dispatcher_freeze_registry());
    device_command_set_hooks(&s_mock_hooks);
    s_mock_send_succeeds = true;
    s_mock_send_completes = true;
}

static gw_message_t gateway_message(const char *command)
{
    gw_message_t message = {.protocol_version = GW_PROTOCOL_VERSION};
    strlcpy(message.type, "gateway_command", sizeof(message.type));
    strlcpy(message.command, command, sizeof(message.command));
    return message;
}

static gw_message_t device_message(const char *device_id, const char *command)
{
    gw_message_t message = {.protocol_version = GW_PROTOCOL_VERSION};
    strlcpy(message.type, "device_command", sizeof(message.type));
    strlcpy(message.device_id, device_id, sizeof(message.device_id));
    message.has_device_id = 1;
    strlcpy(message.command, command, sizeof(message.command));
    return message;
}

static void stub_handler(const gw_message_t *message, dispatch_result_t *result)
{
    (void)message;
    (void)result;
}

static void dispatch_device_command(const char *device_id, const char *command,
                                    dispatch_result_t *result)
{
    gw_message_t message = device_message(device_id, command);
    command_dispatcher_handle(&message, result);
}

// ---------------------------------------------------------------------------
// Registry (§17.2)
// ---------------------------------------------------------------------------

TEST_CASE("dispatcher init is single shot", "[dispatcher]")
{
    command_dispatcher_reset_for_test();
    TEST_ASSERT_EQUAL_INT(0, command_dispatcher_init());
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE, command_dispatcher_init());
}

TEST_CASE("dispatcher registers default commands", "[dispatcher]")
{
    fresh_frozen_dispatcher();
    TEST_ASSERT_TRUE(command_dispatcher_is_registered("add_device"));
    TEST_ASSERT_TRUE(command_dispatcher_is_registered("delete_device"));
    TEST_ASSERT_TRUE(command_dispatcher_is_registered("edit_device"));
    TEST_ASSERT_TRUE(command_dispatcher_is_registered("list_devices"));
    TEST_ASSERT_TRUE(command_dispatcher_is_registered("get_status"));
    TEST_ASSERT_FALSE(command_dispatcher_is_registered("does_not_exist"));
}

TEST_CASE("dispatcher rejects invalid registrations", "[dispatcher]")
{
    fresh_frozen_dispatcher();
    // Registry is frozen: every registration must be rejected.
    TEST_ASSERT_EQUAL_INT(-1,
                          command_dispatcher_register("x", stub_handler));
    TEST_ASSERT_FALSE(command_dispatcher_is_registered("x"));
}

TEST_CASE("dispatcher rejects duplicate empty long name and null handler before freeze",
          "[dispatcher]")
{
    command_dispatcher_reset_for_test();
    TEST_ASSERT_EQUAL_INT(0, command_dispatcher_init());

    char long_name[GW_MSG_COMMAND_LEN + 1];
    memset(long_name, 'a', sizeof(long_name));
    long_name[sizeof(long_name) - 1] = '\0';

    TEST_ASSERT_EQUAL_INT(-1, command_dispatcher_register(NULL, stub_handler));
    TEST_ASSERT_EQUAL_INT(-1, command_dispatcher_register("ok", NULL));
    TEST_ASSERT_EQUAL_INT(-1, command_dispatcher_register("", stub_handler));
    TEST_ASSERT_EQUAL_INT(-1,
                          command_dispatcher_register(long_name, stub_handler));
    TEST_ASSERT_EQUAL_INT(0, command_dispatcher_register("ok", stub_handler));
    TEST_ASSERT_EQUAL_INT(-1, command_dispatcher_register("ok", stub_handler));

    // Fill up to DISPATCHER_MAX_COMMANDS, next registration must fail.
    char name[GW_MSG_COMMAND_LEN];
    for (int i = command_registry_count(); i < DISPATCHER_MAX_COMMANDS; i++) {
        snprintf(name, sizeof(name), "cmd_%d", i);
        TEST_ASSERT_EQUAL_INT(0, command_dispatcher_register(name, stub_handler));
    }
    TEST_ASSERT_EQUAL_INT(-1, command_dispatcher_register("overflow", stub_handler));
}

TEST_CASE("dispatcher copy out registered names", "[dispatcher]")
{
    fresh_frozen_dispatcher();
    char names[DISPATCHER_MAX_COMMANDS][GW_MSG_COMMAND_LEN];
    int count = command_dispatcher_get_registered_names(names,
                                                        DISPATCHER_MAX_COMMANDS);
    TEST_ASSERT_EQUAL_INT(6, count);
    // Mutating caller copies must not affect the registry.
    names[0][0] = 'X';
    TEST_ASSERT_TRUE(command_dispatcher_is_registered("add_device"));
}

TEST_CASE("dispatcher handle fails before registry freeze", "[dispatcher]")
{
    command_dispatcher_reset_for_test();
    TEST_ASSERT_EQUAL_INT(0, command_dispatcher_init());
    gw_message_t message = gateway_message("list_devices");
    dispatch_result_t result;
    command_dispatcher_handle(&message, &result);
    TEST_ASSERT_EQUAL_INT(DISPATCH_STATUS_INTERNAL_ERROR, result.status);
}

// ---------------------------------------------------------------------------
// Routing + boundary validation (§17.1, §15.1)
// ---------------------------------------------------------------------------

TEST_CASE("dispatcher rejects null message", "[dispatcher]")
{
    fresh_frozen_dispatcher();
    dispatch_result_t result;
    command_dispatcher_handle(NULL, &result);
    TEST_ASSERT_EQUAL_INT(DISPATCH_STATUS_INVALID_ARGUMENT, result.status);
}

TEST_CASE("dispatcher rejects unsupported protocol version", "[dispatcher]")
{
    fresh_frozen_dispatcher();
    gw_message_t message = gateway_message("list_devices");
    message.protocol_version = (uint8_t)(GW_PROTOCOL_VERSION + 1);
    dispatch_result_t result;
    command_dispatcher_handle(&message, &result);
    TEST_ASSERT_EQUAL_INT(DISPATCH_STATUS_INVALID_ARGUMENT, result.status);

    message.protocol_version = 0;
    command_dispatcher_handle(&message, &result);
    TEST_ASSERT_EQUAL_INT(DISPATCH_STATUS_INVALID_ARGUMENT, result.status);
}

TEST_CASE("dispatcher rejects unknown message type", "[dispatcher]")
{
    fresh_frozen_dispatcher();
    gw_message_t message = gateway_message("list_devices");
    strlcpy(message.type, "mystery_type", sizeof(message.type));
    dispatch_result_t result;
    command_dispatcher_handle(&message, &result);
    TEST_ASSERT_EQUAL_INT(DISPATCH_STATUS_NOT_FOUND, result.status);
}

TEST_CASE("dispatcher rejects device command without device id or command",
          "[dispatcher]")
{
    fresh_frozen_dispatcher();
    dispatch_result_t result;

    gw_message_t missing_device = device_message("plug-1", "set_power");
    missing_device.has_device_id = 0;
    command_dispatcher_handle(&missing_device, &result);
    TEST_ASSERT_EQUAL_INT(DISPATCH_STATUS_INVALID_ARGUMENT, result.status);

    gw_message_t missing_command = device_message("plug-1", "");
    command_dispatcher_handle(&missing_command, &result);
    TEST_ASSERT_EQUAL_INT(DISPATCH_STATUS_INVALID_ARGUMENT, result.status);
}

TEST_CASE("dispatcher reports unknown gateway command as not found",
          "[dispatcher]")
{
    fresh_frozen_dispatcher();
    gw_message_t message = gateway_message("does_not_exist");
    dispatch_result_t result;
    command_dispatcher_handle(&message, &result);
    TEST_ASSERT_EQUAL_INT(DISPATCH_STATUS_NOT_FOUND, result.status);
}

// ---------------------------------------------------------------------------
// Result contract (§17.5)
// ---------------------------------------------------------------------------

TEST_CASE("dispatcher returns JSON format for list devices", "[dispatcher]")
{
    fresh_frozen_dispatcher();
    TEST_ASSERT_EQUAL_INT(0, device_store_add("plug-1", "Desk"));
    gw_message_t message = gateway_message("list_devices");

    dispatch_result_t result;
    command_dispatcher_handle(&message, &result);
    TEST_ASSERT_TRUE(dispatch_result_is_ok(&result));
    TEST_ASSERT_EQUAL_INT(DISPATCH_RESULT_JSON, result.format);
    cJSON *array = cJSON_Parse(result.payload);
    TEST_ASSERT_TRUE(cJSON_IsArray(array));
    TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(array));
    cJSON_Delete(array);
}

TEST_CASE("text result truncates oversized payloads safely", "[dispatcher]")
{
    dispatch_result_t result;
    char big[DISPATCHER_MAX_RESULT_LEN + 64];
    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    command_dispatcher_set_text_result(&result, DISPATCH_STATUS_OK, "%s", big);
    TEST_ASSERT_EQUAL_INT(DISPATCH_RESULT_TEXT, result.format);
    TEST_ASSERT_EQUAL_INT(DISPATCH_STATUS_OK, result.status);
    TEST_ASSERT_EQUAL_CHAR('\0', result.payload[sizeof(result.payload) - 1]);
}

TEST_CASE("json result keeps null terminated payload", "[dispatcher]")
{
    dispatch_result_t result;
    command_dispatcher_set_json_result(&result, DISPATCH_STATUS_OK,
                                       "{\"persisted\":true}");
    TEST_ASSERT_EQUAL_INT(DISPATCH_RESULT_JSON, result.format);
    TEST_ASSERT_EQUAL_STRING("{\"persisted\":true}", result.payload);
}

// ---------------------------------------------------------------------------
// ACK correlation end-to-end (§17.3 cases 1, 6, 7, 9, 10)
// ---------------------------------------------------------------------------

TEST_CASE("device command completes with matching ACK", "[dispatcher]")
{
    fresh_frozen_dispatcher();
    gw_message_t message = device_message("relay-1", "set_power");

    dispatch_result_t result;
    command_dispatcher_handle(&message, &result);
    TEST_ASSERT_TRUE(dispatch_result_is_ok(&result));
    TEST_ASSERT_EQUAL_INT(2, s_captured_wire.protocol_version);
    TEST_ASSERT_TRUE(s_captured_wire.has_request_id);
    TEST_ASSERT_NOT_EQUAL(0, s_captured_wire.request_id);
    // The caller's message stays immutable.
    TEST_ASSERT_FALSE(message.has_request_id);
}

TEST_CASE("capability discovery query uses protocol v3", "[dispatcher]")
{
    fresh_frozen_dispatcher();
    dispatch_result_t result;
    dispatch_device_command("relay-1", "describe_capabilities", &result);
    TEST_ASSERT_TRUE(dispatch_result_is_ok(&result));
    TEST_ASSERT_EQUAL_INT(GW_PROTOCOL_VERSION,
                          s_captured_wire.protocol_version);
}

TEST_CASE("device command times out without ACK and releases slot",
          "[dispatcher]")
{
    fresh_frozen_dispatcher();
    s_mock_send_completes = false;

    dispatch_result_t result;
    dispatch_device_command("relay-1", "set_power", &result);
    TEST_ASSERT_EQUAL_INT(DISPATCH_STATUS_TIMEOUT, result.status);

    // Slot must be reusable afterwards.
    s_mock_send_completes = true;
    dispatch_device_command("relay-1", "set_power", &result);
    TEST_ASSERT_TRUE(dispatch_result_is_ok(&result));
}

TEST_CASE("stale ACK cannot complete a new request", "[dispatcher]")
{
    fresh_frozen_dispatcher();

    // Request A times out.
    s_mock_send_completes = false;
    dispatch_result_t result;
    dispatch_device_command("relay-1", "set_power", &result);
    TEST_ASSERT_EQUAL_INT(DISPATCH_STATUS_TIMEOUT, result.status);
    uint32_t stale_request_id = s_captured_wire.request_id;

    // Request B starts; A's late ACK arrives but must not wake B.
    s_mock_send_completes = true;
    s_mock_use_stale_request_id = true;
    s_stale_request_id = stale_request_id;
    dispatch_device_command("relay-1", "set_power", &result);
    TEST_ASSERT_EQUAL_INT(DISPATCH_STATUS_TIMEOUT, result.status);

    // B still completes with its own correctly-correlated ACK.
    s_mock_use_stale_request_id = false;
    dispatch_device_command("relay-1", "set_power", &result);
    TEST_ASSERT_TRUE(dispatch_result_is_ok(&result));
}

TEST_CASE("two devices correlate independently", "[dispatcher]")
{
    fresh_frozen_dispatcher();
    dispatch_result_t result_a;
    dispatch_result_t result_b;
    // Sequential commands on distinct devices both succeed immediately.
    dispatch_device_command("relay-1", "set_power", &result_a);
    dispatch_device_command("relay-2", "set_power", &result_b);
    TEST_ASSERT_TRUE(dispatch_result_is_ok(&result_a));
    TEST_ASSERT_TRUE(dispatch_result_is_ok(&result_b));
    TEST_ASSERT_NOT_EQUAL(s_captured_wire.request_id, 0);
}

TEST_CASE("send failure releases pending slot", "[dispatcher]")
{
    fresh_frozen_dispatcher();
    s_mock_send_succeeds = false;
    s_mock_send_completes = false;

    dispatch_result_t result;
    dispatch_device_command("relay-1", "set_power", &result);
    TEST_ASSERT_EQUAL_INT(DISPATCH_STATUS_TRANSPORT_ERROR, result.status);

    // Next command must not report BUSY (slot was released).
    s_mock_send_succeeds = true;
    s_mock_send_completes = true;
    dispatch_device_command("relay-1", "set_power", &result);
    TEST_ASSERT_TRUE(dispatch_result_is_ok(&result));
}

TEST_CASE("second concurrent command for same device reports busy",
          "[dispatcher]")
{
    fresh_frozen_dispatcher();
    pending_request_t *pending = NULL;
    TEST_ASSERT_EQUAL_INT(0, device_request_allocate("relay-1", "set_power",
                                                     &pending));
    dispatch_result_t result;
    dispatch_device_command("relay-1", "set_power", &result);
    TEST_ASSERT_EQUAL_INT(DISPATCH_STATUS_BUSY, result.status);
    device_request_release(pending);
}
