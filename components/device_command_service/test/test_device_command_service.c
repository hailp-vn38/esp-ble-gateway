#include "device_command_service.h"
#include "device_command_service_internal.h"

#include <string.h>

#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "device_schema.h"
#include "device_store.h"
#include "unity.h"

/* ── Mock transport ──────────────────────────────────────────────────── */

static int mock_send_rc = 0;
static int mock_connected = 1;
static gw_message_t last_sent_message;
static bool send_called = false;
static uint32_t send_count = 0;

static int mock_send_command(const char *device_id, const gw_message_t *msg)
{
    send_called = true;
    send_count++;
    strlcpy(last_sent_message.device_id, device_id,
            sizeof(last_sent_message.device_id));
    last_sent_message = *msg;
    return mock_send_rc;
}

static int mock_is_connected(const char *device_id)
{
    (void)device_id;
    return mock_connected;
}

static device_command_transport_hooks_t mock_hooks = {
    .send_command = mock_send_command,
    .is_connected = mock_is_connected,
};

static device_schema_submit_done_fn schema_done;
static void *schema_done_context;

static esp_err_t schema_submitter(const gw_message_t *message,
                                  device_schema_submit_done_fn done,
                                  void *context)
{
    (void)message;
    schema_done = done;
    schema_done_context = context;
    return ESP_OK;
}

static void seed_typed_schema(const char *device_id, const char *command,
                              uint32_t snapshot_id, uint8_t value_type,
                              int32_t min_value, int32_t max_value,
                              uint32_t step, const char *feature_id,
                              uint8_t property_id)
{
    device_store_add(device_id, device_id);
    schema_done = NULL;
    schema_done_context = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, device_schema_on_ready(device_id));
    vTaskDelay(pdMS_TO_TICKS(30));

    gw_message_t begin = {0};
    begin.protocol_version = GW_PROTOCOL_VERSION;
    strlcpy(begin.type, "capabilities_begin", sizeof(begin.type));
    strlcpy(begin.device_id, device_id, sizeof(begin.device_id));
    begin.has_device_id = begin.has_snapshot_id = begin.has_total = 1;
    begin.has_feature_total = begin.has_capability_revision = 1;
    begin.snapshot_id = snapshot_id;
    begin.total = 1;
    begin.feature_total = feature_id != NULL ? 1 : 0;
    begin.capability_revision = 1;
    TEST_ASSERT_TRUE(device_schema_on_notify(device_id, &begin));

    gw_message_t item = {0};
    item.protocol_version = GW_PROTOCOL_VERSION;
    strlcpy(item.type, "capability_item", sizeof(item.type));
    strlcpy(item.device_id, device_id, sizeof(item.device_id));
    strlcpy(item.command, command, sizeof(item.command));
    strlcpy(item.capability_label, command, sizeof(item.capability_label));
    item.has_device_id = item.has_snapshot_id = item.has_sequence = 1;
    item.has_value_type = item.has_capability_flags = 1;
    item.snapshot_id = snapshot_id;
    item.sequence = 0;
    item.value_type = value_type;
    if (value_type == 2) {
        item.has_min_value = item.has_max_value = item.has_step = 1;
        item.min_value = min_value;
        item.max_value = max_value;
        item.step = step;
    }
    TEST_ASSERT_TRUE(device_schema_on_notify(device_id, &item));

    if (feature_id != NULL) {
        gw_message_t feature = {0};
        feature.protocol_version = GW_PROTOCOL_VERSION;
        strlcpy(feature.type, "feature_item", sizeof(feature.type));
        strlcpy(feature.device_id, device_id, sizeof(feature.device_id));
        strlcpy(feature.feature_id, feature_id, sizeof(feature.feature_id));
        strlcpy(feature.feature_tool, command, sizeof(feature.feature_tool));
        feature.has_device_id = feature.has_snapshot_id = 1;
        feature.has_sequence = feature.has_feature_id = 1;
        feature.has_feature_type = feature.has_property_id = 1;
        feature.has_feature_tool = 1;
        feature.snapshot_id = snapshot_id;
        feature.sequence = 1;
        feature.feature_type = GW_FEATURE_GENERIC_RELAY;
        feature.property_id = property_id;
        TEST_ASSERT_TRUE(device_schema_on_notify(device_id, &feature));
    }

    gw_message_t end = {0};
    end.protocol_version = GW_PROTOCOL_VERSION;
    strlcpy(end.type, "capabilities_end", sizeof(end.type));
    strlcpy(end.device_id, device_id, sizeof(end.device_id));
    end.has_device_id = end.has_snapshot_id = end.has_total = 1;
    end.snapshot_id = snapshot_id;
    end.total = 1;
    TEST_ASSERT_TRUE(device_schema_on_notify(device_id, &end));
    vTaskDelay(pdMS_TO_TICKS(80));
    if (schema_done != NULL) schema_done(DEVICE_SCHEMA_SUBMIT_OK,
                                         schema_done_context);
    vTaskDelay(pdMS_TO_TICKS(50));
}

static void seed_schema(const char *device_id, const char *command,
                        uint32_t snapshot_id)
{
    seed_typed_schema(device_id, command, snapshot_id, 1, 0, 0, 0,
                      NULL, 0);
}

/* ── Completion tracking ─────────────────────────────────────────────── */

static device_command_result_t last_result;
static bool completion_called = false;
static uint32_t completion_count = 0;
static void *last_completion_context = NULL;

static void test_completion(const device_command_result_t *result, void *ctx)
{
    last_result = *result;
    completion_called = true;
    completion_count++;
    last_completion_context = ctx;
}

/* ── Helpers ─────────────────────────────────────────────────────────── */

static void reset_test_state(void)
{
    mock_send_rc = 0;
    mock_connected = 1;
    send_called = false;
    send_count = 0;
    completion_called = false;
    completion_count = 0;
    last_completion_context = NULL;
    memset(&last_result, 0, sizeof(last_result));
    memset(&last_sent_message, 0, sizeof(last_sent_message));
    device_schema_reset_for_test();
    TEST_ASSERT_EQUAL(ESP_OK, device_schema_init());
    device_schema_set_submitter(schema_submitter);
    seed_schema("dev1", "set_led", 101);
    seed_schema("dev2", "set_fan", 102);
}

static device_command_request_t make_control_request(const char *device_id,
                                                      const char *command)
{
    device_command_request_t req = {0};
    req.origin = DEVICE_CMD_ORIGIN_CONTROL;
    strlcpy(req.device_id, device_id, sizeof(req.device_id));
    strlcpy(req.command, command, sizeof(req.command));
    return req;
}

static void wait_for_completion(void)
{
    for (int i = 0; i < 20 && !completion_called; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static gw_message_t make_ack(const char *device_id, const char *command,
                             uint32_t request_id, bool accepted)
{
    gw_message_t ack = {0};
    strlcpy(ack.type, "device_ack", sizeof(ack.type));
    strlcpy(ack.device_id, device_id, sizeof(ack.device_id));
    strlcpy(ack.command, command, sizeof(ack.command));
    ack.request_id = request_id;
    ack.has_request_id = 1;
    ack.bool_value = accepted ? 1 : 0;
    ack.has_device_id = 1;
    return ack;
}

/* ── Tests ───────────────────────────────────────────────────────────── */

TEST_CASE("service init/deinit", "[device_command_service]")
{
    reset_test_state();
    device_command_service_set_hooks(&mock_hooks);

    TEST_ASSERT_EQUAL(ESP_OK, device_command_service_init());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, device_command_service_init());

    device_command_service_deinit();
}

TEST_CASE("null request rejected", "[device_command_service]")
{
    reset_test_state();
    device_command_service_set_hooks(&mock_hooks);
    device_command_service_init();

    esp_err_t err = device_command_service_submit(NULL, test_completion, NULL);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, err);

    device_command_service_deinit();
}

TEST_CASE("null completion rejected", "[device_command_service]")
{
    reset_test_state();
    device_command_service_set_hooks(&mock_hooks);
    device_command_service_init();

    device_command_request_t req = make_control_request("dev1", "set_led");
    esp_err_t err = device_command_service_submit(&req, NULL, NULL);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, err);

    device_command_service_deinit();
}

TEST_CASE("control rejects unadvertised command without BLE send",
          "[device_command_service][phase2]")
{
    reset_test_state();
    device_command_service_set_hooks(&mock_hooks);
    TEST_ASSERT_EQUAL(ESP_OK, device_command_service_init());
    device_command_request_t req = make_control_request("dev1", "raw_command");
    req.has_bool_value = true;
    TEST_ASSERT_EQUAL(ESP_OK, device_command_service_submit(
                                  &req, test_completion, NULL));
    wait_for_completion();
    TEST_ASSERT_EQUAL(DEVICE_CMD_STATUS_UNSUPPORTED_COMMAND,
                      last_result.status);
    TEST_ASSERT_EQUAL_UINT32(0, send_count);
    device_command_service_deinit();
}

TEST_CASE("control distinguishes schema not ready",
          "[device_command_service][phase2]")
{
    reset_test_state();
    device_command_service_set_hooks(&mock_hooks);
    TEST_ASSERT_EQUAL(ESP_OK, device_command_service_init());
    device_command_request_t req = make_control_request("unknown", "set_led");
    req.has_bool_value = true;
    TEST_ASSERT_EQUAL(ESP_OK, device_command_service_submit(
                                  &req, test_completion, NULL));
    wait_for_completion();
    TEST_ASSERT_EQUAL(DEVICE_CMD_STATUS_SCHEMA_NOT_READY, last_result.status);
    TEST_ASSERT_EQUAL_UINT32(0, send_count);
    device_command_service_deinit();
}

TEST_CASE("control rejects wrong value type without BLE send",
          "[device_command_service][phase2]")
{
    reset_test_state();
    device_command_service_set_hooks(&mock_hooks);
    TEST_ASSERT_EQUAL(ESP_OK, device_command_service_init());
    device_command_request_t req = make_control_request("dev1", "set_led");
    req.has_int_value = true;
    req.int_value = 1;
    TEST_ASSERT_EQUAL(ESP_OK, device_command_service_submit(
                                  &req, test_completion, NULL));
    wait_for_completion();
    TEST_ASSERT_EQUAL(DEVICE_CMD_STATUS_TYPE_MISMATCH, last_result.status);
    TEST_ASSERT_EQUAL_UINT32(0, send_count);
    device_command_service_deinit();
}

static void assert_int_validation(int32_t value,
                                  device_command_status_t expected_status,
                                  uint32_t expected_sends)
{
    device_command_service_set_hooks(&mock_hooks);
    TEST_ASSERT_EQUAL(ESP_OK, device_command_service_init());
    device_command_request_t req = make_control_request("dev-int", "set_level");
    req.has_int_value = true;
    req.int_value = value;
    TEST_ASSERT_EQUAL(ESP_OK, device_command_service_submit(
                                  &req, test_completion, NULL));
    if (expected_status == DEVICE_CMD_STATUS_OK) {
        for (int i = 0; i < 20 && send_count == 0; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        TEST_ASSERT_EQUAL_UINT32(expected_sends, send_count);
    } else {
        wait_for_completion();
        TEST_ASSERT_EQUAL(expected_status, last_result.status);
        TEST_ASSERT_EQUAL_UINT32(expected_sends, send_count);
    }
    device_command_service_deinit();
}

TEST_CASE("control enforces integer range and step",
          "[device_command_service][phase2]")
{
    reset_test_state();
    seed_typed_schema("dev-int", "set_level", 103, 2, 10, 100, 5,
                      NULL, 0);
    assert_int_validation(5, DEVICE_CMD_STATUS_RANGE_ERROR, 0);

    completion_called = false; completion_count = 0; send_count = 0;
    assert_int_validation(105, DEVICE_CMD_STATUS_RANGE_ERROR, 0);

    completion_called = false; completion_count = 0; send_count = 0;
    assert_int_validation(12, DEVICE_CMD_STATUS_RANGE_ERROR, 0);

    completion_called = false; completion_count = 0; send_count = 0;
    assert_int_validation(55, DEVICE_CMD_STATUS_OK, 1);
}

TEST_CASE("control validates semantic feature and property mapping",
          "[device_command_service][phase2]")
{
    reset_test_state();
    seed_typed_schema("dev-feature", "set_relay", 104, 1, 0, 0, 0,
                      "relay_1", GW_PROP_ON_OFF);
    device_command_service_set_hooks(&mock_hooks);
    TEST_ASSERT_EQUAL(ESP_OK, device_command_service_init());

    device_command_request_t invalid =
        make_control_request("dev-feature", "set_relay");
    invalid.has_bool_value = true;
    invalid.has_feature_id = true;
    strlcpy(invalid.feature_id, "relay_1", sizeof(invalid.feature_id));
    invalid.has_property_id = true;
    invalid.property_id = GW_PROP_LEVEL;
    TEST_ASSERT_EQUAL(ESP_OK, device_command_service_submit(
                                  &invalid, test_completion, NULL));
    wait_for_completion();
    TEST_ASSERT_EQUAL(DEVICE_CMD_STATUS_INVALID_ARGUMENT, last_result.status);
    TEST_ASSERT_EQUAL_UINT32(0, send_count);
    device_command_service_deinit();

    completion_called = false; completion_count = 0; send_count = 0;
    device_command_service_set_hooks(&mock_hooks);
    TEST_ASSERT_EQUAL(ESP_OK, device_command_service_init());
    invalid.property_id = GW_PROP_ON_OFF;
    TEST_ASSERT_EQUAL(ESP_OK, device_command_service_submit(
                                  &invalid, test_completion, NULL));
    vTaskDelay(pdMS_TO_TICKS(30));
    TEST_ASSERT_EQUAL_UINT32(1, send_count);
    device_command_service_deinit();
}

TEST_CASE("invalid origin schema discovery rejects non-describe", "[device_command_service]")
{
    reset_test_state();
    device_command_service_set_hooks(&mock_hooks);
    device_command_service_init();

    device_command_request_t req = {0};
    req.origin = DEVICE_CMD_ORIGIN_SCHEMA_DISCOVERY;
    strlcpy(req.device_id, "dev1", sizeof(req.device_id));
    strlcpy(req.command, "set_led", sizeof(req.command));

    esp_err_t err = device_command_service_submit(&req, test_completion, NULL);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    vTaskDelay(pdMS_TO_TICKS(50));
    TEST_ASSERT_TRUE(completion_called);
    TEST_ASSERT_EQUAL(DEVICE_CMD_STATUS_UNSUPPORTED_COMMAND, last_result.status);

    device_command_service_deinit();
}

TEST_CASE("origin state read requires feature_id", "[device_command_service]")
{
    reset_test_state();
    device_command_service_set_hooks(&mock_hooks);
    device_command_service_init();

    device_command_request_t req = {0};
    req.origin = DEVICE_CMD_ORIGIN_STATE_READ;
    strlcpy(req.device_id, "dev1", sizeof(req.device_id));
    strlcpy(req.command, "read_feature_state", sizeof(req.command));
    /* Missing feature_id */

    esp_err_t err = device_command_service_submit(&req, test_completion, NULL);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    vTaskDelay(pdMS_TO_TICKS(50));
    TEST_ASSERT_TRUE(completion_called);
    TEST_ASSERT_EQUAL(DEVICE_CMD_STATUS_INVALID_ARGUMENT, last_result.status);

    device_command_service_deinit();
}

TEST_CASE("origin state read requires property_id",
          "[device_command_service][phase2]")
{
    reset_test_state();
    device_command_service_set_hooks(&mock_hooks);
    TEST_ASSERT_EQUAL(ESP_OK, device_command_service_init());
    device_command_request_t req = {0};
    req.origin = DEVICE_CMD_ORIGIN_STATE_READ;
    strlcpy(req.device_id, "dev1", sizeof(req.device_id));
    strlcpy(req.command, "read_feature_state", sizeof(req.command));
    req.has_feature_id = true;
    strlcpy(req.feature_id, "relay_1", sizeof(req.feature_id));
    TEST_ASSERT_EQUAL(ESP_OK, device_command_service_submit(
                                  &req, test_completion, NULL));
    wait_for_completion();
    TEST_ASSERT_EQUAL(DEVICE_CMD_STATUS_INVALID_ARGUMENT, last_result.status);
    TEST_ASSERT_EQUAL_UINT32(0, send_count);
    device_command_service_deinit();
}

TEST_CASE("valid state read reaches BLE transport",
          "[device_command_service][phase2]")
{
    reset_test_state();
    device_command_service_set_hooks(&mock_hooks);
    TEST_ASSERT_EQUAL(ESP_OK, device_command_service_init());
    device_command_request_t req = {0};
    req.origin = DEVICE_CMD_ORIGIN_STATE_READ;
    strlcpy(req.device_id, "dev1", sizeof(req.device_id));
    strlcpy(req.command, "read_feature_state", sizeof(req.command));
    req.has_feature_id = true;
    strlcpy(req.feature_id, "relay_1", sizeof(req.feature_id));
    req.has_property_id = true;
    req.property_id = GW_PROP_ON_OFF;
    TEST_ASSERT_EQUAL(ESP_OK, device_command_service_submit(
                                  &req, test_completion, NULL));
    vTaskDelay(pdMS_TO_TICKS(30));
    TEST_ASSERT_EQUAL_UINT32(1, send_count);
    device_command_service_deinit();
}

TEST_CASE("one pending per device returns BUSY", "[device_command_service]")
{
    reset_test_state();
    mock_send_rc = 0;
    device_command_service_set_hooks(&mock_hooks);
    device_command_service_init();

    /* First request */
    device_command_request_t req1 = make_control_request("dev1", "set_led");
    req1.has_bool_value = true;
    req1.bool_value = true;
    esp_err_t err = device_command_service_submit(&req1, test_completion, NULL);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    vTaskDelay(pdMS_TO_TICKS(20));
    TEST_ASSERT_TRUE(send_called);
    TEST_ASSERT_EQUAL(1, device_command_service_get_pending_count());

    /* Second request to same device should fail */
    completion_called = false;
    device_command_request_t req2 = make_control_request("dev1", "set_led");
    req2.has_bool_value = true;
    req2.bool_value = false;
    err = device_command_service_submit(&req2, test_completion, NULL);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    vTaskDelay(pdMS_TO_TICKS(50));
    TEST_ASSERT_TRUE(completion_called);
    TEST_ASSERT_EQUAL(DEVICE_CMD_STATUS_BUSY, last_result.status);

    device_command_service_deinit();
}

TEST_CASE("different devices can be pending concurrently", "[device_command_service]")
{
    reset_test_state();
    mock_send_rc = 0;
    device_command_service_set_hooks(&mock_hooks);
    device_command_service_init();

    device_command_request_t req1 = make_control_request("dev1", "set_led");
    req1.has_bool_value = true;
    req1.bool_value = true;
    TEST_ASSERT_EQUAL(ESP_OK, device_command_service_submit(&req1, test_completion, NULL));

    device_command_request_t req2 = make_control_request("dev2", "set_fan");
    req2.has_bool_value = true;
    req2.bool_value = true;
    TEST_ASSERT_EQUAL(ESP_OK, device_command_service_submit(&req2, test_completion, NULL));

    vTaskDelay(pdMS_TO_TICKS(50));
    TEST_ASSERT_EQUAL(2, device_command_service_get_pending_count());

    device_command_service_deinit();
}

TEST_CASE("pending table capacity fails with typed queue full status",
          "[device_command_service][phase2]")
{
    reset_test_state();
    seed_schema("dev3", "set_value", 103);
    seed_schema("dev4", "set_value", 104);
    seed_schema("dev5", "set_value", 105);
    device_command_service_set_hooks(&mock_hooks);
    TEST_ASSERT_EQUAL(ESP_OK, device_command_service_init());

    const char *ids[] = {"dev1", "dev2", "dev3", "dev4"};
    const char *commands[] = {"set_led", "set_fan", "set_value", "set_value"};
    for (size_t i = 0; i < 4; i++) {
        device_command_request_t req = make_control_request(ids[i], commands[i]);
        req.has_bool_value = true;
        TEST_ASSERT_EQUAL(ESP_OK, device_command_service_submit(
                                      &req, test_completion, NULL));
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    TEST_ASSERT_EQUAL_UINT32(4, device_command_service_get_pending_count());

    completion_called = false;
    completion_count = 0;
    device_command_request_t overflow =
        make_control_request("dev5", "set_value");
    overflow.has_bool_value = true;
    TEST_ASSERT_EQUAL(ESP_OK, device_command_service_submit(
                                  &overflow, test_completion, NULL));
    wait_for_completion();
    TEST_ASSERT_EQUAL_UINT32(1, completion_count);
    TEST_ASSERT_EQUAL(DEVICE_CMD_STATUS_QUEUE_FULL, last_result.status);
    TEST_ASSERT_EQUAL_UINT32(4, device_command_service_get_pending_count());
    device_command_service_deinit();
}

TEST_CASE("event queue full returns bounded submit failure",
          "[device_command_service][phase2]")
{
    reset_test_state();
    device_command_service_set_hooks(&mock_hooks);
    TEST_ASSERT_EQUAL(ESP_OK, device_command_service_init());
    vTaskSuspend(g_dcs.task);

    device_command_request_t req = make_control_request("dev1", "set_led");
    req.has_bool_value = true;
    for (size_t i = 0; i < DCS_QUEUE_LEN; i++) {
        TEST_ASSERT_EQUAL(ESP_OK, device_command_service_submit(
                                      &req, test_completion, NULL));
    }
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, device_command_service_submit(
                                          &req, test_completion, NULL));
    device_command_service_stats_t stats = {0};
    device_command_service_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(1, stats.queue_full);

    vTaskResume(g_dcs.task);
    vTaskDelay(pdMS_TO_TICKS(100));
    device_command_service_deinit();
}

TEST_CASE("ACK completes pending request", "[device_command_service]")
{
    reset_test_state();
    mock_send_rc = 0;
    device_command_service_set_hooks(&mock_hooks);
    device_command_service_init();

    device_command_request_t req = make_control_request("dev1", "set_led");
    req.has_bool_value = true;
    req.bool_value = true;
    TEST_ASSERT_EQUAL(ESP_OK, device_command_service_submit(&req, test_completion, NULL));

    vTaskDelay(pdMS_TO_TICKS(30));
    TEST_ASSERT_TRUE(send_called);
    uint32_t request_id = last_sent_message.request_id;
    TEST_ASSERT_NOT_EQUAL(0, request_id);

    /* Simulate ACK */
    gw_message_t ack = {0};
    strlcpy(ack.type, "device_ack", sizeof(ack.type));
    strlcpy(ack.device_id, "dev1", sizeof(ack.device_id));
    strlcpy(ack.command, "set_led", sizeof(ack.command));
    ack.request_id = request_id;
    ack.has_request_id = 1;
    ack.bool_value = 1; /* accepted */
    ack.has_device_id = 1;

    completion_called = false;
    TEST_ASSERT_TRUE(device_command_service_on_notify("dev1", &ack));

    vTaskDelay(pdMS_TO_TICKS(50));
    TEST_ASSERT_TRUE(completion_called);
    TEST_ASSERT_EQUAL(DEVICE_CMD_STATUS_OK, last_result.status);
    TEST_ASSERT_TRUE(last_result.accepted);
    TEST_ASSERT_EQUAL(0, device_command_service_get_pending_count());

    device_command_service_deinit();
}

TEST_CASE("duplicate ACK completes exactly once",
          "[device_command_service][phase2]")
{
    reset_test_state();
    device_command_service_set_hooks(&mock_hooks);
    TEST_ASSERT_EQUAL(ESP_OK, device_command_service_init());
    device_command_request_t req = make_control_request("dev1", "set_led");
    req.has_bool_value = true;
    TEST_ASSERT_EQUAL(ESP_OK, device_command_service_submit(
                                  &req, test_completion, NULL));
    vTaskDelay(pdMS_TO_TICKS(30));
    gw_message_t ack = make_ack("dev1", "set_led",
                                last_sent_message.request_id, true);
    TEST_ASSERT_TRUE(device_command_service_on_notify("dev1", &ack));
    TEST_ASSERT_TRUE(device_command_service_on_notify("dev1", &ack));
    vTaskDelay(pdMS_TO_TICKS(50));
    TEST_ASSERT_EQUAL_UINT32(1, completion_count);
    TEST_ASSERT_EQUAL_UINT32(0, device_command_service_get_pending_count());
    device_command_service_deinit();
}

TEST_CASE("rejected ACK completes pending with DEVICE_REJECTED",
          "[device_command_service][baseline]")
{
    reset_test_state();
    device_command_service_set_hooks(&mock_hooks);
    device_command_service_init();

    device_command_request_t req = make_control_request("dev1", "set_led");
    req.has_bool_value = true;
    req.bool_value = true;
    TEST_ASSERT_EQUAL(ESP_OK,
                      device_command_service_submit(&req, test_completion, NULL));
    vTaskDelay(pdMS_TO_TICKS(30));

    gw_message_t ack = {0};
    strlcpy(ack.type, "device_ack", sizeof(ack.type));
    strlcpy(ack.device_id, "dev1", sizeof(ack.device_id));
    strlcpy(ack.command, "set_led", sizeof(ack.command));
    ack.request_id = last_sent_message.request_id;
    ack.has_request_id = 1;
    ack.bool_value = 0;
    ack.has_device_id = 1;

    completion_called = false;
    TEST_ASSERT_TRUE(device_command_service_on_notify("dev1", &ack));
    vTaskDelay(pdMS_TO_TICKS(50));
    TEST_ASSERT_TRUE(completion_called);
    TEST_ASSERT_EQUAL(DEVICE_CMD_STATUS_DEVICE_REJECTED, last_result.status);
    TEST_ASSERT_FALSE(last_result.accepted);
    TEST_ASSERT_EQUAL(0, device_command_service_get_pending_count());

    device_command_service_deinit();
}

TEST_CASE("missing ACK completes pending with TIMEOUT",
          "[device_command_service][baseline]")
{
    reset_test_state();
    device_command_service_set_hooks(&mock_hooks);
    device_command_service_init();

    device_command_request_t req = make_control_request("dev1", "set_led");
    req.has_bool_value = true;
    req.bool_value = true;
    TEST_ASSERT_EQUAL(ESP_OK,
                      device_command_service_submit(&req, test_completion, NULL));

    vTaskDelay(pdMS_TO_TICKS(2300));
    TEST_ASSERT_TRUE(completion_called);
    TEST_ASSERT_EQUAL(DEVICE_CMD_STATUS_TIMEOUT, last_result.status);
    TEST_ASSERT_EQUAL(0, device_command_service_get_pending_count());

    device_command_service_deinit();
}

TEST_CASE("late ACK after timeout is ignored",
          "[device_command_service][phase2]")
{
    reset_test_state();
    device_command_service_set_hooks(&mock_hooks);
    TEST_ASSERT_EQUAL(ESP_OK, device_command_service_init());
    device_command_request_t req = make_control_request("dev1", "set_led");
    req.has_bool_value = true;
    TEST_ASSERT_EQUAL(ESP_OK, device_command_service_submit(
                                  &req, test_completion, NULL));
    vTaskDelay(pdMS_TO_TICKS(30));
    uint32_t request_id = last_sent_message.request_id;
    vTaskDelay(pdMS_TO_TICKS(2200));
    TEST_ASSERT_EQUAL_UINT32(1, completion_count);
    TEST_ASSERT_EQUAL(DEVICE_CMD_STATUS_TIMEOUT, last_result.status);
    gw_message_t ack = make_ack("dev1", "set_led", request_id, true);
    TEST_ASSERT_TRUE(device_command_service_on_notify("dev1", &ack));
    vTaskDelay(pdMS_TO_TICKS(50));
    TEST_ASSERT_EQUAL_UINT32(1, completion_count);
    device_command_service_deinit();
}

TEST_CASE("wrong request_id does not complete", "[device_command_service]")
{
    reset_test_state();
    mock_send_rc = 0;
    device_command_service_set_hooks(&mock_hooks);
    device_command_service_init();

    device_command_request_t req = make_control_request("dev1", "set_led");
    req.has_bool_value = true;
    req.bool_value = true;
    TEST_ASSERT_EQUAL(ESP_OK, device_command_service_submit(&req, test_completion, NULL));

    vTaskDelay(pdMS_TO_TICKS(30));

    /* ACK with wrong request_id */
    gw_message_t ack = {0};
    strlcpy(ack.type, "device_ack", sizeof(ack.type));
    strlcpy(ack.device_id, "dev1", sizeof(ack.device_id));
    strlcpy(ack.command, "set_led", sizeof(ack.command));
    ack.request_id = 999999;
    ack.has_request_id = 1;
    ack.bool_value = 1;
    ack.has_device_id = 1;

    completion_called = false;
    device_command_service_on_notify("dev1", &ack);

    vTaskDelay(pdMS_TO_TICKS(50));
    TEST_ASSERT_FALSE(completion_called);
    TEST_ASSERT_EQUAL(1, device_command_service_get_pending_count());

    device_command_service_deinit();
}

TEST_CASE("disconnect completes pending with NOT_CONNECTED", "[device_command_service]")
{
    reset_test_state();
    mock_send_rc = 0;
    device_command_service_set_hooks(&mock_hooks);
    device_command_service_init();

    device_command_request_t req = make_control_request("dev1", "set_led");
    req.has_bool_value = true;
    req.bool_value = true;
    TEST_ASSERT_EQUAL(ESP_OK, device_command_service_submit(&req, test_completion, NULL));

    vTaskDelay(pdMS_TO_TICKS(30));
    TEST_ASSERT_EQUAL(1, device_command_service_get_pending_count());

    completion_called = false;
    device_command_service_on_disconnect("dev1");

    vTaskDelay(pdMS_TO_TICKS(50));
    TEST_ASSERT_TRUE(completion_called);
    TEST_ASSERT_EQUAL_UINT32(1, completion_count);
    TEST_ASSERT_EQUAL(DEVICE_CMD_STATUS_NOT_CONNECTED, last_result.status);
    TEST_ASSERT_EQUAL(0, device_command_service_get_pending_count());

    device_command_service_deinit();
}

TEST_CASE("cancel completes pending exactly once",
          "[device_command_service][phase2]")
{
    reset_test_state();
    device_command_service_set_hooks(&mock_hooks);
    TEST_ASSERT_EQUAL(ESP_OK, device_command_service_init());
    device_command_request_t req = make_control_request("dev1", "set_led");
    req.has_bool_value = true;
    TEST_ASSERT_EQUAL(ESP_OK, device_command_service_submit(
                                  &req, test_completion, NULL));
    vTaskDelay(pdMS_TO_TICKS(30));
    uint32_t request_id = last_sent_message.request_id;
    TEST_ASSERT_EQUAL(ESP_OK, device_command_service_cancel_device("dev1"));
    wait_for_completion();
    TEST_ASSERT_EQUAL(DEVICE_CMD_STATUS_CANCELLED, last_result.status);
    TEST_ASSERT_EQUAL_UINT32(1, completion_count);
    gw_message_t ack = make_ack("dev1", "set_led", request_id, true);
    TEST_ASSERT_TRUE(device_command_service_on_notify("dev1", &ack));
    vTaskDelay(pdMS_TO_TICKS(50));
    TEST_ASSERT_EQUAL_UINT32(1, completion_count);
    device_command_service_deinit();
}

TEST_CASE("shutdown cancels pending exactly once",
          "[device_command_service][phase2]")
{
    reset_test_state();
    device_command_service_set_hooks(&mock_hooks);
    TEST_ASSERT_EQUAL(ESP_OK, device_command_service_init());
    device_command_request_t req = make_control_request("dev1", "set_led");
    req.has_bool_value = true;
    TEST_ASSERT_EQUAL(ESP_OK, device_command_service_submit(
                                  &req, test_completion, NULL));
    vTaskDelay(pdMS_TO_TICKS(30));
    device_command_service_deinit();
    TEST_ASSERT_EQUAL_UINT32(1, completion_count);
    TEST_ASSERT_EQUAL(DEVICE_CMD_STATUS_CANCELLED, last_result.status);
    TEST_ASSERT_EQUAL_UINT32(0, device_command_service_get_pending_count());
}

TEST_CASE("send failure returns TRANSPORT_ERROR", "[device_command_service]")
{
    reset_test_state();
    mock_send_rc = -1;
    device_command_service_set_hooks(&mock_hooks);
    device_command_service_init();

    device_command_request_t req = make_control_request("dev1", "set_led");
    req.has_bool_value = true;
    req.bool_value = true;
    TEST_ASSERT_EQUAL(ESP_OK, device_command_service_submit(&req, test_completion, NULL));

    vTaskDelay(pdMS_TO_TICKS(50));
    TEST_ASSERT_TRUE(completion_called);
    TEST_ASSERT_EQUAL(DEVICE_CMD_STATUS_TRANSPORT_ERROR, last_result.status);
    TEST_ASSERT_EQUAL(0, device_command_service_get_pending_count());

    device_command_service_deinit();
}

TEST_CASE("not connected returns NOT_CONNECTED", "[device_command_service]")
{
    reset_test_state();
    mock_send_rc = 0;
    mock_connected = 0;
    device_command_service_set_hooks(&mock_hooks);
    device_command_service_init();

    device_command_request_t req = make_control_request("dev1", "set_led");
    req.has_bool_value = true;
    req.bool_value = true;
    TEST_ASSERT_EQUAL(ESP_OK, device_command_service_submit(&req, test_completion, NULL));

    vTaskDelay(pdMS_TO_TICKS(50));
    TEST_ASSERT_TRUE(completion_called);
    TEST_ASSERT_EQUAL(DEVICE_CMD_STATUS_NOT_CONNECTED, last_result.status);

    device_command_service_deinit();
}

TEST_CASE("stats tracking", "[device_command_service]")
{
    reset_test_state();
    mock_send_rc = 0;
    device_command_service_set_hooks(&mock_hooks);
    device_command_service_init();

    device_command_request_t req = make_control_request("dev1", "set_led");
    req.has_bool_value = true;
    req.bool_value = true;
    device_command_service_submit(&req, test_completion, NULL);
    vTaskDelay(pdMS_TO_TICKS(30));

    device_command_service_stats_t stats;
    device_command_service_get_stats(&stats);
    TEST_ASSERT_EQUAL(1, stats.submitted);

    device_command_service_deinit();
}
