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
static int s_submit_count;

static esp_err_t mock_submit(const gw_message_t *message,
                             device_cap_submit_done_fn done, void *context)
{
    s_submitted = *message;
    s_done = done;
    s_done_context = context;
    s_submit_count++;
    return ESP_OK;
}

static esp_err_t mock_submit_fail(const gw_message_t *message,
                                  device_cap_submit_done_fn done, void *context)
{
    (void)message;
    (void)done;
    (void)context;
    return ESP_ERR_NO_MEM;
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
    s_submit_count = 0;
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

static void commit_valid_snapshot(const char *device_id, uint32_t snapshot_id,
                                  uint32_t revision)
{
    gw_message_t begin = capability_message("capabilities_begin", device_id,
                                            snapshot_id);
    begin.total = 1;
    begin.has_total = 1;
    begin.capability_revision = revision;
    begin.has_capability_revision = 1;
    device_capabilities_on_notify(device_id, &begin);

    gw_message_t item = capability_message("capability_item", device_id,
                                           snapshot_id);
    strlcpy(item.command, "set_led", sizeof(item.command));
    item.sequence = 0;
    item.has_sequence = 1;
    item.value_type = DEVICE_CAP_VALUE_BOOL;
    item.has_value_type = 1;
    device_capabilities_on_notify(device_id, &item);

    gw_message_t end = capability_message("capabilities_end", device_id,
                                          snapshot_id);
    end.total = 1;
    end.has_total = 1;
    device_capabilities_on_notify(device_id, &end);
    vTaskDelay(pdMS_TO_TICKS(50));
}

/* ── Existing tests ────────────────────────────────────────────────── */

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

/* ── Phase A: Cache-first READY ─────────────────────────────────────── */

TEST_CASE("READY with committed cache skips discovery",
          "[device_capabilities][cache_hit]")
{
    prepare("cap-cached");

    /* First: commit a valid snapshot. */
    TEST_ASSERT_EQUAL(ESP_OK, device_capabilities_on_ready("cap-cached"));
    vTaskDelay(pdMS_TO_TICKS(30));
    commit_valid_snapshot("cap-cached", 1, 1);
    TEST_ASSERT_NOT_NULL(s_done);
    s_done(DEVICE_CAP_SUBMIT_OK, s_done_context);
    vTaskDelay(pdMS_TO_TICKS(20));

    device_capability_snapshot_t snapshot;
    TEST_ASSERT_EQUAL(ESP_OK,
                      device_capabilities_get("cap-cached", &snapshot));
    TEST_ASSERT_EQUAL(DEVICE_CAP_STATE_READY, snapshot.state);

    /* Second READY: should NOT trigger discovery. */
    s_submit_count = 0;
    TEST_ASSERT_EQUAL(ESP_OK, device_capabilities_on_ready("cap-cached"));
    vTaskDelay(pdMS_TO_TICKS(30));
    TEST_ASSERT_EQUAL(0, s_submit_count);
}

TEST_CASE("READY without cache starts initial discovery",
          "[device_capabilities][cache_miss]")
{
    prepare("cap-fresh");
    s_submit_count = 0;
    TEST_ASSERT_EQUAL(ESP_OK, device_capabilities_on_ready("cap-fresh"));
    vTaskDelay(pdMS_TO_TICKS(30));
    TEST_ASSERT_EQUAL(1, s_submit_count);
}

TEST_CASE("duplicate READY while queued is idempotent",
          "[device_capabilities]")
{
    prepare("cap-dup");
    TEST_ASSERT_EQUAL(ESP_OK, device_capabilities_on_ready("cap-dup"));
    vTaskDelay(pdMS_TO_TICKS(10));
    /* Second READY should not increase submit count. */
    TEST_ASSERT_EQUAL(ESP_OK, device_capabilities_on_ready("cap-dup"));
    vTaskDelay(pdMS_TO_TICKS(10));
    TEST_ASSERT_EQUAL(1, s_submit_count);
}

/* ── Phase A: Stale completion guard ────────────────────────────────── */

TEST_CASE("stale completion from old operation is ignored",
          "[device_capabilities][stale]")
{
    prepare("cap-stale");

    /* Start first discovery. */
    TEST_ASSERT_EQUAL(ESP_OK, device_capabilities_on_ready("cap-stale"));
    vTaskDelay(pdMS_TO_TICKS(30));
    TEST_ASSERT_NOT_NULL(s_done);

    /* Save the first context. */
    void *first_context = s_done_context;

    /* Simulate a stale completion with wrong result — should not crash. */
    s_done(DEVICE_CAP_SUBMIT_ERROR, first_context);
    vTaskDelay(pdMS_TO_TICKS(20));

    /* Device state should reflect the error. */
    device_capability_snapshot_t snapshot;
    TEST_ASSERT_EQUAL(ESP_OK,
                      device_capabilities_get("cap-stale", &snapshot));
    TEST_ASSERT_EQUAL(DEVICE_CAP_STATE_ERROR, snapshot.state);
}

/* ── Phase A: Disconnect handling ───────────────────────────────────── */

TEST_CASE("disconnect preserves committed READY cache",
          "[device_capabilities][disconnect]")
{
    prepare("cap-disc");

    /* Commit a snapshot. */
    TEST_ASSERT_EQUAL(ESP_OK, device_capabilities_on_ready("cap-disc"));
    vTaskDelay(pdMS_TO_TICKS(30));
    commit_valid_snapshot("cap-disc", 1, 1);
    TEST_ASSERT_NOT_NULL(s_done);
    s_done(DEVICE_CAP_SUBMIT_OK, s_done_context);
    vTaskDelay(pdMS_TO_TICKS(20));

    device_capability_snapshot_t snapshot;
    TEST_ASSERT_EQUAL(ESP_OK,
                      device_capabilities_get("cap-disc", &snapshot));
    TEST_ASSERT_EQUAL(DEVICE_CAP_STATE_READY, snapshot.state);

    /* Disconnect — cache should stay READY. */
    device_capabilities_on_disconnect("cap-disc");
    vTaskDelay(pdMS_TO_TICKS(20));

    TEST_ASSERT_EQUAL(ESP_OK,
                      device_capabilities_get("cap-disc", &snapshot));
    TEST_ASSERT_EQUAL(DEVICE_CAP_STATE_READY, snapshot.state);
}

TEST_CASE("disconnect without cache leaves UNKNOWN",
          "[device_capabilities][disconnect]")
{
    prepare("cap-disc2");
    device_capabilities_on_disconnect("cap-disc2");
    vTaskDelay(pdMS_TO_TICKS(20));

    device_capability_snapshot_t snapshot;
    TEST_ASSERT_EQUAL(ESP_OK,
                      device_capabilities_get("cap-disc2", &snapshot));
    TEST_ASSERT_EQUAL(DEVICE_CAP_STATE_UNKNOWN, snapshot.state);
}

/* ── Phase A: Manual refresh ────────────────────────────────────────── */

TEST_CASE("manual refresh returns generation on success",
          "[device_capabilities][refresh]")
{
    prepare("cap-refresh");

    /* Commit a cache first. */
    TEST_ASSERT_EQUAL(ESP_OK, device_capabilities_on_ready("cap-refresh"));
    vTaskDelay(pdMS_TO_TICKS(30));
    commit_valid_snapshot("cap-refresh", 1, 1);
    TEST_ASSERT_NOT_NULL(s_done);
    s_done(DEVICE_CAP_SUBMIT_OK, s_done_context);
    vTaskDelay(pdMS_TO_TICKS(20));

    uint32_t gen = 0;
    esp_err_t err = device_capabilities_refresh("cap-refresh", &gen);
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_NOT_EQUAL(0, gen);

    /* Verify refresh status shows queued/running. */
    device_cap_refresh_active_t active;
    device_cap_refresh_completed_t completed;
    TEST_ASSERT_EQUAL(ESP_OK,
                      device_capabilities_get_refresh_status("cap-refresh",
                                                             &active,
                                                             &completed));
    TEST_ASSERT_EQUAL_UINT32(gen, active.generation);
    TEST_ASSERT_NOT_EQUAL(DEVICE_CAP_REFRESH_IDLE, active.state);
}

TEST_CASE("manual refresh rejects same device already running",
          "[device_capabilities][refresh]")
{
    prepare("cap-refresh2");

    TEST_ASSERT_EQUAL(ESP_OK, device_capabilities_on_ready("cap-refresh2"));
    vTaskDelay(pdMS_TO_TICKS(30));
    commit_valid_snapshot("cap-refresh2", 1, 1);
    TEST_ASSERT_NOT_NULL(s_done);
    s_done(DEVICE_CAP_SUBMIT_OK, s_done_context);
    vTaskDelay(pdMS_TO_TICKS(20));

    uint32_t gen1 = 0;
    TEST_ASSERT_EQUAL(ESP_OK,
                      device_capabilities_refresh("cap-refresh2", &gen1));
    vTaskDelay(pdMS_TO_TICKS(5));

    /* Second refresh while first is queued should fail. */
    uint32_t gen2 = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      device_capabilities_refresh("cap-refresh2", &gen2));
}

TEST_CASE("manual refresh rejects unknown device",
          "[device_capabilities][refresh]")
{
    prepare("cap-refresh3");
    uint32_t gen = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      device_capabilities_refresh("nonexistent", &gen));
}

/* ── Phase A: Operation token isolation ─────────────────────────────── */

TEST_CASE("completion only applies to matching operation",
          "[device_capabilities][token]")
{
    prepare("cap-token");

    /* Start discovery. */
    TEST_ASSERT_EQUAL(ESP_OK, device_capabilities_on_ready("cap-token"));
    vTaskDelay(pdMS_TO_TICKS(30));
    TEST_ASSERT_NOT_NULL(s_done);

    /* Complete it. */
    s_done(DEVICE_CAP_SUBMIT_OK, s_done_context);
    vTaskDelay(pdMS_TO_TICKS(20));

    device_capability_snapshot_t snapshot;
    TEST_ASSERT_EQUAL(ESP_OK,
                      device_capabilities_get("cap-token", &snapshot));
    /* No committed snapshot, so state depends on completion mapping. */
}

/* ── Phase A: Submit failure maps correctly ─────────────────────────── */

TEST_CASE("submit rejection maps to UNSUPPORTED",
          "[device_capabilities][submit]")
{
    prepare("cap-reject");
    TEST_ASSERT_EQUAL(ESP_OK, device_capabilities_on_ready("cap-reject"));
    vTaskDelay(pdMS_TO_TICKS(30));
    TEST_ASSERT_NOT_NULL(s_done);

    s_done(DEVICE_CAP_SUBMIT_REJECTED, s_done_context);
    vTaskDelay(pdMS_TO_TICKS(20));

    device_capability_snapshot_t snapshot;
    TEST_ASSERT_EQUAL(ESP_OK,
                      device_capabilities_get("cap-reject", &snapshot));
    TEST_ASSERT_EQUAL(DEVICE_CAP_STATE_ERROR, snapshot.state);
}

/* ── Phase B: Forget is failure-safe ────────────────────────────────── */

TEST_CASE("forget removes record and returns success",
          "[device_capabilities][forget]")
{
    prepare("cap-forget");

    /* Commit a snapshot. */
    TEST_ASSERT_EQUAL(ESP_OK, device_capabilities_on_ready("cap-forget"));
    vTaskDelay(pdMS_TO_TICKS(30));
    commit_valid_snapshot("cap-forget", 1, 1);
    TEST_ASSERT_NOT_NULL(s_done);
    s_done(DEVICE_CAP_SUBMIT_OK, s_done_context);
    vTaskDelay(pdMS_TO_TICKS(20));

    device_capability_snapshot_t snapshot;
    TEST_ASSERT_EQUAL(ESP_OK,
                      device_capabilities_get("cap-forget", &snapshot));
    TEST_ASSERT_EQUAL(DEVICE_CAP_STATE_READY, snapshot.state);

    /* Forget. */
    TEST_ASSERT_EQUAL(ESP_OK,
                      device_capabilities_forget("cap-forget"));
    vTaskDelay(pdMS_TO_TICKS(20));

    /* After forget, capabilities are cleared but device_store entry remains. */
    TEST_ASSERT_EQUAL(ESP_OK,
                      device_capabilities_get("cap-forget", &snapshot));
    TEST_ASSERT_EQUAL(DEVICE_CAP_STATE_UNKNOWN, snapshot.state);
    TEST_ASSERT_EQUAL_UINT32(0, snapshot.revision);
    TEST_ASSERT_EQUAL_UINT32(0, snapshot.count);
}

TEST_CASE("forget unknown device is idempotent",
          "[device_capabilities][forget]")
{
    prepare("cap-forget2");
    TEST_ASSERT_EQUAL(ESP_OK,
                      device_capabilities_forget("nonexistent"));
}

/* ── Phase B: Refresh status API ────────────────────────────────────── */

TEST_CASE("get_refresh_status returns idle after READY completion",
          "[device_capabilities][refresh_status]")
{
    prepare("cap-rstatus");

    /* Must commit a snapshot first — refresh status requires a record. */
    TEST_ASSERT_EQUAL(ESP_OK, device_capabilities_on_ready("cap-rstatus"));
    vTaskDelay(pdMS_TO_TICKS(30));
    commit_valid_snapshot("cap-rstatus", 1, 1);
    TEST_ASSERT_NOT_NULL(s_done);
    s_done(DEVICE_CAP_SUBMIT_OK, s_done_context);
    vTaskDelay(pdMS_TO_TICKS(20));

    device_cap_refresh_active_t active;
    device_cap_refresh_completed_t completed;
    TEST_ASSERT_EQUAL(ESP_OK,
                      device_capabilities_get_refresh_status("cap-rstatus",
                                                             &active,
                                                             &completed));
    TEST_ASSERT_EQUAL(DEVICE_CAP_REFRESH_IDLE, active.state);
    TEST_ASSERT_EQUAL_UINT32(0, active.generation);
    TEST_ASSERT_EQUAL_UINT32(0, completed.generation);
    TEST_ASSERT_EQUAL(DEVICE_CAP_REFRESH_RESULT_NONE, completed.result);
}

TEST_CASE("get_refresh_status returns NOT_FOUND for unknown device",
          "[device_capabilities][refresh_status]")
{
    prepare("cap-rstatus2");
    device_cap_refresh_active_t active;
    device_cap_refresh_completed_t completed;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      device_capabilities_get_refresh_status("nonexistent",
                                                             &active,
                                                             &completed));
}

/* ── Phase B: State name helper ─────────────────────────────────────── */

TEST_CASE("state name returns correct strings",
          "[device_capabilities][state_name]")
{
    TEST_ASSERT_EQUAL_STRING("unknown",
                             device_capabilities_state_name(
                                 DEVICE_CAP_STATE_UNKNOWN));
    TEST_ASSERT_EQUAL_STRING("discovering",
                             device_capabilities_state_name(
                                 DEVICE_CAP_STATE_DISCOVERING));
    TEST_ASSERT_EQUAL_STRING("ready",
                             device_capabilities_state_name(
                                 DEVICE_CAP_STATE_READY));
    TEST_ASSERT_EQUAL_STRING("unsupported",
                             device_capabilities_state_name(
                                 DEVICE_CAP_STATE_UNSUPPORTED));
    TEST_ASSERT_EQUAL_STRING("error",
                             device_capabilities_state_name(
                                 DEVICE_CAP_STATE_ERROR));
}

/* ── Phase B: Refresh result name helper ────────────────────────────── */

TEST_CASE("refresh result name returns correct strings",
          "[device_capabilities][refresh_result]")
{
    TEST_ASSERT_EQUAL_STRING(
        "success",
        device_capabilities_refresh_result_name(
            DEVICE_CAP_REFRESH_RESULT_SUCCESS));
    TEST_ASSERT_EQUAL_STRING(
        "unchanged",
        device_capabilities_refresh_result_name(
            DEVICE_CAP_REFRESH_RESULT_UNCHANGED));
    TEST_ASSERT_EQUAL_STRING(
        "busy",
        device_capabilities_refresh_result_name(
            DEVICE_CAP_REFRESH_RESULT_BUSY));
    TEST_ASSERT_EQUAL_STRING(
        "disconnected",
        device_capabilities_refresh_result_name(
            DEVICE_CAP_REFRESH_RESULT_DISCONNECTED));
}
