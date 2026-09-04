#include "device_command_service.h"

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

static int mock_send_command(const char *device_id, const gw_message_t *msg)
{
    send_called = true;
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

static void seed_schema(const char *device_id, const char *command,
                        uint32_t snapshot_id)
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
    begin.feature_total = 0;
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
    item.value_type = 1; /* BOOL */
    TEST_ASSERT_TRUE(device_schema_on_notify(device_id, &item));

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

/* ── Completion tracking ─────────────────────────────────────────────── */

static device_command_result_t last_result;
static bool completion_called = false;
static void *last_completion_context = NULL;

static void test_completion(const device_command_result_t *result, void *ctx)
{
    last_result = *result;
    completion_called = true;
    last_completion_context = ctx;
}

/* ── Helpers ─────────────────────────────────────────────────────────── */

static void reset_test_state(void)
{
    mock_send_rc = 0;
    mock_connected = 1;
    send_called = false;
    completion_called = false;
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
    TEST_ASSERT_EQUAL(DEVICE_CMD_STATUS_NOT_CONNECTED, last_result.status);
    TEST_ASSERT_EQUAL(0, device_command_service_get_pending_count());

    device_command_service_deinit();
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
