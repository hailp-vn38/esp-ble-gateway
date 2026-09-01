#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "../device_schema_internal.h"
#include "cbor_codec.h"
#include "device_schema.h"
#include "device_store.h"
#include "nvs.h"

/* ── Helpers ────────────────────────────────────────────────────────── */

static void reset_and_init(void)
{
    device_schema_reset_for_test();
    TEST_ASSERT_EQUAL_INT(ESP_OK, device_schema_init());
}

static gw_message_t make_begin(const char *device_id, uint32_t snapshot_id,
                               uint16_t tool_total, uint16_t feature_total,
                               uint32_t revision)
{
    gw_message_t msg = {0};
    msg.protocol_version = GW_PROTOCOL_VERSION;
    msg.has_device_id = 1;
    strlcpy(msg.device_id, device_id, sizeof(msg.device_id));
    strlcpy(msg.type, "capabilities_begin", sizeof(msg.type));
    msg.has_snapshot_id = 1;
    msg.snapshot_id = snapshot_id;
    msg.has_total = 1;
    msg.total = tool_total;
    msg.has_feature_total = 1;
    msg.feature_total = feature_total;
    msg.has_capability_revision = 1;
    msg.capability_revision = revision;
    return msg;
}

static gw_message_t make_tool_item(const char *device_id, uint32_t snapshot_id,
                                   uint16_t sequence, const char *command,
                                   uint8_t value_type, uint8_t flags,
                                   int32_t min_val, int32_t max_val,
                                   uint32_t step_val)
{
    gw_message_t msg = {0};
    msg.protocol_version = GW_PROTOCOL_VERSION;
    msg.has_device_id = 1;
    strlcpy(msg.device_id, device_id, sizeof(msg.device_id));
    strlcpy(msg.type, "capability_item", sizeof(msg.type));
    msg.has_snapshot_id = 1;
    msg.snapshot_id = snapshot_id;
    msg.has_sequence = 1;
    msg.sequence = sequence;
    msg.has_value_type = 1;
    msg.value_type = value_type;
    msg.has_capability_flags = 1;
    msg.capability_flags = flags;
    strlcpy(msg.command, command, sizeof(msg.command));
    strlcpy(msg.capability_label, command, sizeof(msg.capability_label));
    if (value_type == 2 /* INT */) {
        msg.has_min_value = 1;
        msg.min_value = min_val;
        msg.has_max_value = 1;
        msg.max_value = max_val;
        msg.has_step = 1;
        msg.step = step_val;
    }
    return msg;
}

static gw_message_t make_feature_item(const char *device_id,
                                      uint32_t snapshot_id, uint16_t sequence,
                                      const char *feature_id,
                                      uint8_t feature_type,
                                      uint8_t property_id,
                                      const char *feature_tool)
{
    gw_message_t msg = {0};
    msg.protocol_version = GW_PROTOCOL_VERSION;
    msg.has_device_id = 1;
    strlcpy(msg.device_id, device_id, sizeof(msg.device_id));
    strlcpy(msg.type, "feature_item", sizeof(msg.type));
    msg.has_snapshot_id = 1;
    msg.snapshot_id = snapshot_id;
    msg.has_sequence = 1;
    msg.sequence = sequence;
    msg.has_feature_id = 1;
    strlcpy(msg.feature_id, feature_id, sizeof(msg.feature_id));
    msg.has_feature_type = 1;
    msg.feature_type = feature_type;
    msg.has_property_id = 1;
    msg.property_id = property_id;
    if (feature_tool != NULL && feature_tool[0] != '\0') {
        msg.has_feature_tool = 1;
        strlcpy(msg.feature_tool, feature_tool, sizeof(msg.feature_tool));
    }
    return msg;
}

static gw_message_t make_end(const char *device_id, uint32_t snapshot_id,
                             uint16_t tool_total)
{
    gw_message_t msg = {0};
    msg.protocol_version = GW_PROTOCOL_VERSION;
    msg.has_device_id = 1;
    strlcpy(msg.device_id, device_id, sizeof(msg.device_id));
    strlcpy(msg.type, "capabilities_end", sizeof(msg.type));
    msg.has_snapshot_id = 1;
    msg.snapshot_id = snapshot_id;
    msg.has_total = 1;
    msg.total = tool_total;
    return msg;
}

/* ── Validation helper tests ────────────────────────────────────────── */

TEST_CASE("valid_command_name accepts valid names", "[device_schema]")
{
    TEST_ASSERT_TRUE(schema_valid_command_name("toggle"));
    TEST_ASSERT_TRUE(schema_valid_command_name("set_level"));
    TEST_ASSERT_TRUE(schema_valid_command_name("led.on"));
    TEST_ASSERT_TRUE(schema_valid_command_name("fan-speed"));
    TEST_ASSERT_TRUE(schema_valid_command_name("a1"));
}

TEST_CASE("valid_command_name rejects invalid names", "[device_schema]")
{
    TEST_ASSERT_FALSE(schema_valid_command_name(""));
    TEST_ASSERT_FALSE(schema_valid_command_name("has space"));
    TEST_ASSERT_FALSE(schema_valid_command_name("special!"));
    TEST_ASSERT_FALSE(schema_valid_command_name("no@at"));
}

TEST_CASE("valid_tool validates NONE type", "[device_schema]")
{
    device_schema_tool_t tool = {0};
    strlcpy(tool.command, "toggle", sizeof(tool.command));
    tool.value_type = 0; /* NONE */
    TEST_ASSERT_TRUE(schema_valid_tool(&tool));
}

TEST_CASE("valid_tool validates INT type with range", "[device_schema]")
{
    device_schema_tool_t tool = {0};
    strlcpy(tool.command, "set_level", sizeof(tool.command));
    tool.value_type = 2; /* INT */
    tool.min_value = 0;
    tool.max_value = 100;
    tool.step = 1;
    TEST_ASSERT_TRUE(schema_valid_tool(&tool));
}

TEST_CASE("valid_tool rejects INT with min > max", "[device_schema]")
{
    device_schema_tool_t tool = {0};
    strlcpy(tool.command, "bad_range", sizeof(tool.command));
    tool.value_type = 2;
    tool.min_value = 100;
    tool.max_value = 0;
    tool.step = 1;
    TEST_ASSERT_FALSE(schema_valid_tool(&tool));
}

TEST_CASE("valid_tool rejects INT with step 0", "[device_schema]")
{
    device_schema_tool_t tool = {0};
    strlcpy(tool.command, "bad_step", sizeof(tool.command));
    tool.value_type = 2;
    tool.min_value = 0;
    tool.max_value = 100;
    tool.step = 0;
    TEST_ASSERT_FALSE(schema_valid_tool(&tool));
}

TEST_CASE("tool_equal compares field by field", "[device_schema]")
{
    device_schema_tool_t a = {0};
    strlcpy(a.command, "toggle", sizeof(a.command));
    a.value_type = 1;
    a.flags = DEVICE_SCHEMA_FLAG_IDEMPOTENT;

    device_schema_tool_t b = a;
    TEST_ASSERT_TRUE(schema_tool_equal(&a, &b));

    b.flags = DEVICE_SCHEMA_FLAG_DESTRUCTIVE;
    TEST_ASSERT_FALSE(schema_tool_equal(&a, &b));
}

TEST_CASE("valid_feature_id validates", "[device_schema]")
{
    TEST_ASSERT_TRUE(schema_valid_feature_id("light_state"));
    TEST_ASSERT_FALSE(schema_valid_feature_id(""));
}

TEST_CASE("resolve_writable_tool finds matching command", "[device_schema]")
{
    device_schema_tool_t tools[3] = {0};
    strlcpy(tools[0].command, "toggle", sizeof(tools[0].command));
    strlcpy(tools[1].command, "set_level", sizeof(tools[1].command));
    strlcpy(tools[2].command, "get_status", sizeof(tools[2].command));

    TEST_ASSERT_EQUAL_INT(1, schema_resolve_writable_tool(tools, 3, "set_level"));
    TEST_ASSERT_EQUAL_INT(0, schema_resolve_writable_tool(tools, 3, "toggle"));
    TEST_ASSERT_EQUAL_INT(-1, schema_resolve_writable_tool(tools, 3, "missing"));
    TEST_ASSERT_EQUAL_INT(-1, schema_resolve_writable_tool(tools, 3, NULL));
    TEST_ASSERT_EQUAL_INT(-1, schema_resolve_writable_tool(tools, 3, ""));
}

/* ── Public API lifecycle tests ─────────────────────────────────────── */

TEST_CASE("init is single shot", "[device_schema]")
{
    reset_and_init();
    /* Second init must be rejected. */
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE, device_schema_init());
}

TEST_CASE("get returns unknown state for unknown device", "[device_schema]")
{
    reset_and_init();
    device_schema_snapshot_t snap = {0};
    TEST_ASSERT_EQUAL_INT(ESP_ERR_NOT_FOUND,
                          device_schema_get("nonexistent", &snap));
}

TEST_CASE("validate_command returns unknown for unregistered device",
          "[device_schema]")
{
    reset_and_init();
    gw_message_t msg = {0};
    msg.protocol_version = GW_PROTOCOL_VERSION;
    msg.has_device_id = 1;
    strlcpy(msg.device_id, "unknown_dev", sizeof(msg.device_id));
    strlcpy(msg.command, "toggle", sizeof(msg.command));
    TEST_ASSERT_EQUAL_INT(DEVICE_SCHEMA_VALID_UNKNOWN,
                          device_schema_validate_command(&msg, NULL));
}

/* ── Discovery flow tests ──────────────────────────────────────────── */

static bool s_submit_called;
static gw_message_t s_submitted_msg;
static device_schema_submit_done_fn s_submit_done;
static void *s_submit_done_ctx;

static esp_err_t test_submitter(const gw_message_t *message,
                                device_schema_submit_done_fn done,
                                void *context)
{
    s_submit_called = true;
    s_submitted_msg = *message;
    /* Save the completion callback — call it after discovery messages. */
    s_submit_done = done;
    s_submit_done_ctx = context;
    return ESP_OK;
}

static void complete_discovery(void)
{
    if (s_submit_done != NULL) {
        s_submit_done(DEVICE_SCHEMA_SUBMIT_OK, s_submit_done_ctx);
        s_submit_done = NULL;
        s_submit_done_ctx = NULL;
    }
}

TEST_CASE("discovery: begin → tool_item → feature_item → end commits",
          "[device_schema]")
{
    reset_and_init();

    /* Register device in device_store so schema_get can find it. */
    device_store_add("dev1", "Test Device", "switch");

    device_schema_set_submitter(test_submitter);
    s_submit_called = false;

    /* Trigger on_ready to start discovery. */
    TEST_ASSERT_EQUAL_INT(ESP_OK, device_schema_on_ready("dev1"));
    vTaskDelay(pdMS_TO_TICKS(200));

    /* Submitter should have been called with describe_capabilities. */
    TEST_ASSERT_TRUE(s_submit_called);
    TEST_ASSERT_EQUAL_INT(GW_PROTOCOL_VERSION,
                          s_submitted_msg.protocol_version);
    TEST_ASSERT_EQUAL_STRING(DEVICE_SCHEMA_RESERVED_COMMAND,
                             s_submitted_msg.command);

    /* Simulate device sending discovery messages via on_notify. */
    gw_message_t begin = make_begin("dev1", 100, 2, 1, 1);
    TEST_ASSERT_TRUE(device_schema_on_notify("dev1", &begin));
    vTaskDelay(pdMS_TO_TICKS(50));

    gw_message_t tool0 = make_tool_item("dev1", 100, 0, "toggle", 0, 0x01,
                                        0, 0, 0);
    TEST_ASSERT_TRUE(device_schema_on_notify("dev1", &tool0));
    vTaskDelay(pdMS_TO_TICKS(50));

    gw_message_t tool1 = make_tool_item("dev1", 100, 1, "set_level", 2, 0x01,
                                        0, 100, 1);
    TEST_ASSERT_TRUE(device_schema_on_notify("dev1", &tool1));
    vTaskDelay(pdMS_TO_TICKS(50));

    gw_message_t feat0 = make_feature_item("dev1", 100, 2,
                                           "light_state",
                                           GW_FEATURE_ON_OFF_LIGHT,
                                           GW_PROP_ON_OFF, "toggle");
    TEST_ASSERT_TRUE(device_schema_on_notify("dev1", &feat0));
    vTaskDelay(pdMS_TO_TICKS(50));

    gw_message_t end = make_end("dev1", 100, 2);
    TEST_ASSERT_TRUE(device_schema_on_notify("dev1", &end));
    vTaskDelay(pdMS_TO_TICKS(200));

    /* Complete the discovery operation. */
    complete_discovery();
    vTaskDelay(pdMS_TO_TICKS(200));

    /* Verify committed snapshot. */
    device_schema_snapshot_t snap = {0};
    TEST_ASSERT_EQUAL_INT(ESP_OK, device_schema_get("dev1", &snap));
    TEST_ASSERT_TRUE(snap.has_committed);
    TEST_ASSERT_EQUAL_INT(DEVICE_SCHEMA_STATE_READY, snap.state);
    TEST_ASSERT_EQUAL_INT(2, (int)snap.tool_count);
    TEST_ASSERT_EQUAL_INT(1, (int)snap.feature_count);

    /* Verify tool content. */
    TEST_ASSERT_EQUAL_STRING("toggle", snap.tools[0].command);
    TEST_ASSERT_EQUAL_INT(0, snap.tools[0].value_type);
    TEST_ASSERT_EQUAL_STRING("set_level", snap.tools[1].command);
    TEST_ASSERT_EQUAL_INT(2, snap.tools[1].value_type);
    TEST_ASSERT_EQUAL_INT(0, snap.tools[1].min_value);
    TEST_ASSERT_EQUAL_INT(100, snap.tools[1].max_value);

    /* Verify feature content. */
    TEST_ASSERT_EQUAL_STRING("light_state", snap.features[0].feature_id);
    TEST_ASSERT_EQUAL_INT(GW_FEATURE_ON_OFF_LIGHT,
                          snap.features[0].feature_type);
    TEST_ASSERT_EQUAL_INT(0, snap.features[0].writable_tool_index);

    /* Validate command after commit. */
    gw_message_t cmd = {0};
    cmd.protocol_version = GW_PROTOCOL_VERSION;
    cmd.has_device_id = 1;
    strlcpy(cmd.device_id, "dev1", sizeof(cmd.device_id));
    strlcpy(cmd.command, "toggle", sizeof(cmd.command));
    TEST_ASSERT_EQUAL_INT(DEVICE_SCHEMA_VALID,
                          device_schema_validate_command(&cmd, NULL));
}

TEST_CASE("discovery: missing tool_item breaks staging", "[device_schema]")
{
    reset_and_init();
    device_store_add("dev2", "Test2", "switch");
    device_schema_set_submitter(test_submitter);
    s_submit_called = false;

    TEST_ASSERT_EQUAL_INT(ESP_OK, device_schema_on_ready("dev2"));
    vTaskDelay(pdMS_TO_TICKS(200));

    /* begin says 2 tools, but only send 1 tool_item. */
    gw_message_t begin = make_begin("dev2", 200, 2, 0, 1);
    TEST_ASSERT_TRUE(device_schema_on_notify("dev2", &begin));
    vTaskDelay(pdMS_TO_TICKS(50));

    gw_message_t tool0 = make_tool_item("dev2", 200, 0, "toggle", 0, 0,
                                        0, 0, 0);
    TEST_ASSERT_TRUE(device_schema_on_notify("dev2", &tool0));
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Send end expecting 2 tools — incomplete. */
    gw_message_t end = make_end("dev2", 200, 2);
    TEST_ASSERT_TRUE(device_schema_on_notify("dev2", &end));
    vTaskDelay(pdMS_TO_TICKS(200));

    /* Should NOT be committed. */
    device_schema_snapshot_t snap = {0};
    TEST_ASSERT_EQUAL_INT(ESP_OK, device_schema_get("dev2", &snap));
    TEST_ASSERT_FALSE(snap.has_committed);
}

TEST_CASE("discovery: duplicate tool command breaks staging", "[device_schema]")
{
    reset_and_init();
    device_store_add("dev3", "Test3", "switch");
    device_schema_set_submitter(test_submitter);
    s_submit_called = false;

    TEST_ASSERT_EQUAL_INT(ESP_OK, device_schema_on_ready("dev3"));
    vTaskDelay(pdMS_TO_TICKS(200));

    gw_message_t begin = make_begin("dev3", 300, 2, 0, 1);
    TEST_ASSERT_TRUE(device_schema_on_notify("dev3", &begin));
    vTaskDelay(pdMS_TO_TICKS(50));

    gw_message_t tool0 = make_tool_item("dev3", 300, 0, "toggle", 0, 0,
                                        0, 0, 0);
    TEST_ASSERT_TRUE(device_schema_on_notify("dev3", &tool0));
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Duplicate command name — should fail. */
    gw_message_t tool1 = make_tool_item("dev3", 300, 1, "toggle", 0, 0,
                                        0, 0, 0);
    TEST_ASSERT_TRUE(device_schema_on_notify("dev3", &tool1));
    vTaskDelay(pdMS_TO_TICKS(50));

    gw_message_t end = make_end("dev3", 300, 2);
    TEST_ASSERT_TRUE(device_schema_on_notify("dev3", &end));
    vTaskDelay(pdMS_TO_TICKS(200));

    device_schema_snapshot_t snap = {0};
    TEST_ASSERT_EQUAL_INT(ESP_OK, device_schema_get("dev3", &snap));
    TEST_ASSERT_FALSE(snap.has_committed);
}

TEST_CASE("discovery: duplicate feature_id breaks staging", "[device_schema]")
{
    reset_and_init();
    device_store_add("dev4", "Test4", "switch");
    device_schema_set_submitter(test_submitter);
    s_submit_called = false;

    TEST_ASSERT_EQUAL_INT(ESP_OK, device_schema_on_ready("dev4"));
    vTaskDelay(pdMS_TO_TICKS(200));

    gw_message_t begin = make_begin("dev4", 400, 1, 2, 1);
    TEST_ASSERT_TRUE(device_schema_on_notify("dev4", &begin));
    vTaskDelay(pdMS_TO_TICKS(50));

    gw_message_t tool0 = make_tool_item("dev4", 400, 0, "toggle", 0, 0,
                                        0, 0, 0);
    TEST_ASSERT_TRUE(device_schema_on_notify("dev4", &tool0));
    vTaskDelay(pdMS_TO_TICKS(50));

    gw_message_t feat0 = make_feature_item("dev4", 400, 1, "light_state",
                                           GW_FEATURE_ON_OFF_LIGHT,
                                           GW_PROP_ON_OFF, NULL);
    TEST_ASSERT_TRUE(device_schema_on_notify("dev4", &feat0));
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Duplicate feature_id — should fail. */
    gw_message_t feat1 = make_feature_item("dev4", 400, 2, "light_state",
                                           GW_FEATURE_ON_OFF_LIGHT,
                                           GW_PROP_ON_OFF, NULL);
    TEST_ASSERT_TRUE(device_schema_on_notify("dev4", &feat1));
    vTaskDelay(pdMS_TO_TICKS(50));

    gw_message_t end = make_end("dev4", 400, 1);
    TEST_ASSERT_TRUE(device_schema_on_notify("dev4", &end));
    vTaskDelay(pdMS_TO_TICKS(200));

    device_schema_snapshot_t snap = {0};
    TEST_ASSERT_EQUAL_INT(ESP_OK, device_schema_get("dev4", &snap));
    TEST_ASSERT_FALSE(snap.has_committed);
}

TEST_CASE("discovery: feature with non-existent tool breaks staging",
          "[device_schema]")
{
    reset_and_init();
    device_store_add("dev5", "Test5", "switch");
    device_schema_set_submitter(test_submitter);
    s_submit_called = false;

    TEST_ASSERT_EQUAL_INT(ESP_OK, device_schema_on_ready("dev5"));
    vTaskDelay(pdMS_TO_TICKS(200));

    gw_message_t begin = make_begin("dev5", 500, 1, 1, 1);
    TEST_ASSERT_TRUE(device_schema_on_notify("dev5", &begin));
    vTaskDelay(pdMS_TO_TICKS(50));

    gw_message_t tool0 = make_tool_item("dev5", 500, 0, "toggle", 0, 0,
                                        0, 0, 0);
    TEST_ASSERT_TRUE(device_schema_on_notify("dev5", &tool0));
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Feature references a tool that doesn't exist. */
    gw_message_t feat0 = make_feature_item("dev5", 500, 1, "light_state",
                                           GW_FEATURE_ON_OFF_LIGHT,
                                           GW_PROP_ON_OFF, "nonexistent_tool");
    TEST_ASSERT_TRUE(device_schema_on_notify("dev5", &feat0));
    vTaskDelay(pdMS_TO_TICKS(50));

    gw_message_t end = make_end("dev5", 500, 1);
    TEST_ASSERT_TRUE(device_schema_on_notify("dev5", &end));
    vTaskDelay(pdMS_TO_TICKS(200));

    device_schema_snapshot_t snap = {0};
    TEST_ASSERT_EQUAL_INT(ESP_OK, device_schema_get("dev5", &snap));
    TEST_ASSERT_FALSE(snap.has_committed);
}

/* ── Forget test ────────────────────────────────────────────────────── */

TEST_CASE("forget clears committed schema", "[device_schema]")
{
    reset_and_init();
    device_store_add("dev6", "Test6", "switch");
    device_schema_set_submitter(test_submitter);
    s_submit_called = false;

    TEST_ASSERT_EQUAL_INT(ESP_OK, device_schema_on_ready("dev6"));
    vTaskDelay(pdMS_TO_TICKS(200));

    /* Commit a simple schema. */
    gw_message_t begin = make_begin("dev6", 600, 1, 0, 1);
    TEST_ASSERT_TRUE(device_schema_on_notify("dev6", &begin));
    vTaskDelay(pdMS_TO_TICKS(50));

    gw_message_t tool0 = make_tool_item("dev6", 600, 0, "led", 1, 0,
                                        0, 0, 0);
    TEST_ASSERT_TRUE(device_schema_on_notify("dev6", &tool0));
    vTaskDelay(pdMS_TO_TICKS(50));

    gw_message_t end = make_end("dev6", 600, 1);
    TEST_ASSERT_TRUE(device_schema_on_notify("dev6", &end));
    vTaskDelay(pdMS_TO_TICKS(200));

    device_schema_snapshot_t snap = {0};
    TEST_ASSERT_EQUAL_INT(ESP_OK, device_schema_get("dev6", &snap));
    TEST_ASSERT_TRUE(snap.has_committed);

    /* Forget. */
    TEST_ASSERT_EQUAL_INT(ESP_OK, device_schema_forget("dev6"));

    TEST_ASSERT_EQUAL_INT(ESP_OK, device_schema_get("dev6", &snap));
    TEST_ASSERT_FALSE(snap.has_committed);
}

/* ── Persistence tests (V4-04) ──────────────────────────────────────── */

/* Helper: commit a minimal schema for "dev7", then tear down in-memory
   state.  On the next init, NVS should restore the committed schema. */
static void commit_schema_for_dev7(void)
{
    reset_and_init();
    device_store_add("dev7", "Persist7", "switch");
    device_schema_set_submitter(test_submitter);
    s_submit_called = false;

    TEST_ASSERT_EQUAL_INT(ESP_OK, device_schema_on_ready("dev7"));
    vTaskDelay(pdMS_TO_TICKS(200));

    gw_message_t begin = make_begin("dev7", 700, 1, 1, 5);
    TEST_ASSERT_TRUE(device_schema_on_notify("dev7", &begin));
    vTaskDelay(pdMS_TO_TICKS(50));

    gw_message_t tool0 = make_tool_item("dev7", 700, 0, "toggle", 0, 0,
                                        0, 0, 0);
    TEST_ASSERT_TRUE(device_schema_on_notify("dev7", &tool0));
    vTaskDelay(pdMS_TO_TICKS(50));

    gw_message_t feat0 = make_feature_item("dev7", 700, 1, "led_state",
                                           GW_FEATURE_ON_OFF_LIGHT,
                                           GW_PROP_ON_OFF, "toggle");
    TEST_ASSERT_TRUE(device_schema_on_notify("dev7", &feat0));
    vTaskDelay(pdMS_TO_TICKS(50));

    gw_message_t end = make_end("dev7", 700, 1);
    TEST_ASSERT_TRUE(device_schema_on_notify("dev7", &end));
    vTaskDelay(pdMS_TO_TICKS(200));

    complete_discovery();
    vTaskDelay(pdMS_TO_TICKS(200));

    device_schema_snapshot_t snap = {0};
    TEST_ASSERT_EQUAL_INT(ESP_OK, device_schema_get("dev7", &snap));
    TEST_ASSERT_TRUE(snap.has_committed);
}

TEST_CASE("reboot: valid dev_schema loaded from NVS", "[device_schema]")
{
    /* Commit schema, then reset in-memory only (simulates reboot). */
    commit_schema_for_dev7();
    device_schema_reset_for_test();

    /* Re-init — should load from NVS. */
    TEST_ASSERT_EQUAL_INT(ESP_OK, device_schema_init());

    /* Device must be re-registered in device_store for load to succeed. */
    device_store_add("dev7", "Persist7", "switch");

    /* Re-init again so schema_load_persisted runs with device registered. */
    device_schema_reset_for_test();
    TEST_ASSERT_EQUAL_INT(ESP_OK, device_schema_init());

    device_schema_snapshot_t snap = {0};
    TEST_ASSERT_EQUAL_INT(ESP_OK, device_schema_get("dev7", &snap));
    TEST_ASSERT_TRUE(snap.has_committed);
    TEST_ASSERT_EQUAL_INT(DEVICE_SCHEMA_STATE_READY, snap.state);
    TEST_ASSERT_EQUAL_INT(1, (int)snap.tool_count);
    TEST_ASSERT_EQUAL_INT(1, (int)snap.feature_count);
    TEST_ASSERT_EQUAL_INT(5, (int)snap.revision);
    TEST_ASSERT_EQUAL_STRING("toggle", snap.tools[0].command);
    TEST_ASSERT_EQUAL_STRING("led_state", snap.features[0].feature_id);
}

TEST_CASE("corrupt dev_schema blob ignored safely", "[device_schema]")
{
    /* Write garbage blob into dev_schema NVS. */
    nvs_handle_t handle;
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        nvs_open("dev_schema", NVS_READWRITE, &handle));
    uint8_t garbage[64];
    memset(garbage, 0xAA, sizeof(garbage));
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        nvs_set_blob(handle, "sch00", garbage, sizeof(garbage)));
    TEST_ASSERT_EQUAL_INT(ESP_OK, nvs_commit(handle));
    nvs_close(handle);

    /* Init must not crash — corrupt record is skipped. */
    device_schema_reset_for_test();
    TEST_ASSERT_EQUAL_INT(ESP_OK, device_schema_init());

    device_schema_snapshot_t snap = {0};
    TEST_ASSERT_EQUAL_INT(ESP_ERR_NOT_FOUND,
                          device_schema_get("nonexistent", &snap));
}

TEST_CASE("legacy dev_caps not loaded into schema", "[device_schema]")
{
    /* Write an old-style cap00 blob. */
    nvs_handle_t handle;
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        nvs_open("dev_caps", NVS_READWRITE, &handle));
    uint8_t old_blob[32];
    memset(old_blob, 0xBB, sizeof(old_blob));
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        nvs_set_blob(handle, "cap00", old_blob, sizeof(old_blob)));
    TEST_ASSERT_EQUAL_INT(ESP_OK, nvs_commit(handle));
    nvs_close(handle);

    /* Init — legacy namespace is not read by dev_schema. */
    device_schema_reset_for_test();
    TEST_ASSERT_EQUAL_INT(ESP_OK, device_schema_init());

    /* No schema record should be loaded from old keys. */
    device_schema_snapshot_t snap = {0};
    TEST_ASSERT_EQUAL_INT(ESP_ERR_NOT_FOUND,
                          device_schema_get("nonexistent", &snap));
}

TEST_CASE("legacy dev_caps erased after init", "[device_schema]")
{
    /* Plant old cap00 + cap01 keys. */
    nvs_handle_t handle;
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        nvs_open("dev_caps", NVS_READWRITE, &handle));
    uint8_t old_blob[16];
    memset(old_blob, 0xCC, sizeof(old_blob));
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        nvs_set_blob(handle, "cap00", old_blob, sizeof(old_blob)));
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        nvs_set_blob(handle, "cap01", old_blob, sizeof(old_blob)));
    TEST_ASSERT_EQUAL_INT(ESP_OK, nvs_commit(handle));
    nvs_close(handle);

    /* Init triggers cleanup. */
    device_schema_reset_for_test();
    TEST_ASSERT_EQUAL_INT(ESP_OK, device_schema_init());
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Verify old keys are gone. */
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        nvs_open("dev_caps", NVS_READONLY, &handle));
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_NVS_NOT_FOUND,
        nvs_get_blob(handle, "cap00", NULL, &len));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_NVS_NOT_FOUND,
        nvs_get_blob(handle, "cap01", NULL, &len));
    nvs_close(handle);
}
