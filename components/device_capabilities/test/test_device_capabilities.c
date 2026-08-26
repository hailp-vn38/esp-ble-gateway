#include <string.h>

#include "device_capabilities.h"
#include "device_store.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "unity.h"

static gw_message_t s_submitted;
static device_cap_submit_done_fn s_done;
static void *s_done_context;
static bool s_initialized;

static esp_err_t mock_submit(const gw_message_t *message,
                             device_cap_submit_done_fn done, void *context)
{
    s_submitted = *message;
    s_done = done;
    s_done_context = context;
    return ESP_OK;
}

static void prepare(const char *device_id)
{
    if (!s_initialized) {
        TEST_ASSERT_EQUAL(ESP_OK, device_capabilities_init());
        s_initialized = true;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    device_capabilities_reset_for_test();
    device_capabilities_set_submitter(mock_submit);
    memset(&s_submitted, 0, sizeof(s_submitted));
    s_done = NULL;
    s_done_context = NULL;
    TEST_ASSERT_EQUAL(DEVICE_STORE_OK,
                      device_store_add(device_id, device_id, "test"));
}

static gw_message_t capability_message(const char *type, const char *device_id,
                                       uint32_t snapshot_id)
{
    gw_message_t message = {
        .protocol_version = 3,
        .has_device_id = 1,
        .snapshot_id = snapshot_id,
        .has_snapshot_id = 1,
    };
    strlcpy(message.type, type, sizeof(message.type));
    strlcpy(message.device_id, device_id, sizeof(message.device_id));
    strlcpy(message.command, DEVICE_CAP_RESERVED_COMMAND,
            sizeof(message.command));
    return message;
}

TEST_CASE("capability discovery commits an atomic snapshot",
          "[device_capabilities]")
{
    prepare("cap-lamp");
    TEST_ASSERT_EQUAL(ESP_OK, device_capabilities_on_ready("cap-lamp"));
    vTaskDelay(pdMS_TO_TICKS(30));
    TEST_ASSERT_EQUAL_STRING(DEVICE_CAP_RESERVED_COMMAND,
                             s_submitted.command);

    gw_message_t begin = capability_message("capabilities_begin", "cap-lamp", 9);
    begin.total = 2;
    begin.has_total = 1;
    begin.capability_revision = 4;
    begin.has_capability_revision = 1;
    TEST_ASSERT_TRUE(device_capabilities_on_notify("cap-lamp", &begin));

    gw_message_t bool_item = capability_message("capability_item", "cap-lamp", 9);
    strlcpy(bool_item.command, "set_power", sizeof(bool_item.command));
    bool_item.sequence = 0;
    bool_item.has_sequence = 1;
    bool_item.value_type = DEVICE_CAP_VALUE_BOOL;
    bool_item.has_value_type = 1;
    strlcpy(bool_item.capability_label, "Power",
            sizeof(bool_item.capability_label));
    TEST_ASSERT_TRUE(device_capabilities_on_notify("cap-lamp", &bool_item));

    gw_message_t int_item = capability_message("capability_item", "cap-lamp", 9);
    strlcpy(int_item.command, "set_brightness", sizeof(int_item.command));
    int_item.sequence = 1;
    int_item.has_sequence = 1;
    int_item.value_type = DEVICE_CAP_VALUE_INT;
    int_item.has_value_type = 1;
    int_item.min_value = 0;
    int_item.has_min_value = 1;
    int_item.max_value = 100;
    int_item.has_max_value = 1;
    int_item.step = 5;
    int_item.has_step = 1;
    TEST_ASSERT_TRUE(device_capabilities_on_notify("cap-lamp", &int_item));

    gw_message_t end = capability_message("capabilities_end", "cap-lamp", 9);
    end.total = 2;
    end.has_total = 1;
    TEST_ASSERT_TRUE(device_capabilities_on_notify("cap-lamp", &end));
    vTaskDelay(pdMS_TO_TICKS(50));

    device_capability_snapshot_t snapshot;
    TEST_ASSERT_EQUAL(ESP_OK,
                      device_capabilities_get("cap-lamp", &snapshot));
    TEST_ASSERT_EQUAL(DEVICE_CAP_STATE_READY, snapshot.state);
    TEST_ASSERT_EQUAL_UINT32(4, snapshot.revision);
    TEST_ASSERT_EQUAL_UINT32(2, snapshot.count);
    TEST_ASSERT_EQUAL_STRING("set_power", snapshot.items[0].command);
    TEST_ASSERT_EQUAL_STRING("set_brightness", snapshot.items[1].command);

    gw_message_t command = {
        .has_device_id = 1,
        .has_int_value = 1,
        .int_value = 55,
    };
    strlcpy(command.device_id, "cap-lamp", sizeof(command.device_id));
    strlcpy(command.command, "set_brightness", sizeof(command.command));
    TEST_ASSERT_EQUAL(DEVICE_CAP_VALID,
                      device_capabilities_validate_command(&command, NULL));
    command.int_value = 53;
    TEST_ASSERT_EQUAL(DEVICE_CAP_VALID_ARGUMENT,
                      device_capabilities_validate_command(&command, NULL));
    strlcpy(command.command, "missing", sizeof(command.command));
    TEST_ASSERT_EQUAL(DEVICE_CAP_VALID_UNSUPPORTED_COMMAND,
                      device_capabilities_validate_command(&command, NULL));

    TEST_ASSERT_NOT_NULL(s_done);
    s_done(DEVICE_CAP_SUBMIT_OK, s_done_context);
    vTaskDelay(pdMS_TO_TICKS(20));
}

TEST_CASE("incomplete capability snapshot is never committed",
          "[device_capabilities]")
{
    prepare("cap-bad");
    TEST_ASSERT_EQUAL(ESP_OK, device_capabilities_on_ready("cap-bad"));
    vTaskDelay(pdMS_TO_TICKS(30));

    gw_message_t begin = capability_message("capabilities_begin", "cap-bad", 2);
    begin.total = 2;
    begin.has_total = 1;
    begin.capability_revision = 1;
    begin.has_capability_revision = 1;
    device_capabilities_on_notify("cap-bad", &begin);

    gw_message_t end = capability_message("capabilities_end", "cap-bad", 2);
    end.total = 2;
    end.has_total = 1;
    device_capabilities_on_notify("cap-bad", &end);
    vTaskDelay(pdMS_TO_TICKS(40));

    device_capability_snapshot_t snapshot;
    TEST_ASSERT_EQUAL(ESP_OK, device_capabilities_get("cap-bad", &snapshot));
    TEST_ASSERT_EQUAL(DEVICE_CAP_STATE_ERROR, snapshot.state);
    TEST_ASSERT_EQUAL_UINT32(0, snapshot.count);

    TEST_ASSERT_NOT_NULL(s_done);
    s_done(DEVICE_CAP_SUBMIT_ERROR, s_done_context);
    vTaskDelay(pdMS_TO_TICKS(20));
}
