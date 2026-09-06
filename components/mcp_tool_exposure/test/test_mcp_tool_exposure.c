#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "../mcp_tool_exposure_internal.h"
#include "cbor_codec.h"
#include "device_schema.h"
#include "device_store.h"
#include "device_template.h"
#include "mcp_tool_exposure.h"
#include "unity.h"

TEST_CASE("exposure init succeeds and snapshot is empty after boot",
          "[mcp_tool_exposure]")
{
    TEST_ASSERT_EQUAL(ESP_OK, mcp_tool_exposure_init());

    mcp_tool_exposure_t buf[4];
    size_t count = 0;
    TEST_ASSERT_EQUAL(ESP_OK,
        mcp_tool_exposure_snapshot(buf, 4, &count));
    TEST_ASSERT_EQUAL_UINT(0, count);
}

TEST_CASE("exposure capacity reports correct limits",
          "[mcp_tool_exposure]")
{
    TEST_ASSERT_EQUAL(ESP_OK, mcp_tool_exposure_init());

    mcp_exposure_capacity_t cap = {0};
    TEST_ASSERT_EQUAL(ESP_OK, mcp_tool_exposure_get_capacity(&cap));
    TEST_ASSERT_EQUAL_UINT(0, cap.enabled);
}

/* ── Schema discovery helpers (mirrors test_mcp_device_control.c) ────── */

static bool s_submit_called;
static device_schema_submit_done_fn s_submit_done;
static void *s_submit_done_ctx;

static esp_err_t test_submitter(const gw_message_t *message,
                                device_schema_submit_done_fn done,
                                void *context)
{
    s_submit_called = true;
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
    msg.has_value_type = 1;
    msg.value_type = device_template_property_value_type(property_id);
    msg.has_feature_schema_version = 1;
    msg.feature_schema_version = 1;
    strlcpy(msg.capability_label, feature_id, sizeof(msg.capability_label));
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

/**
 * Set up an exposure record for a writable feature.
 * After this call the feature passes all MCP hint eligibility checks.
 */
static void setup_feature_exposure(const char *device_id,
                                   const char *feature_id,
                                   const char *command,
                                   const device_schema_tool_t *tool)
{
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        mcp_tool_expose_feature(device_id, feature_id, command,
                                feature_id, tool));
}

/* ──────────────────────────────────────────────────────────────────────────
 *  Regression: MCP_SEMANTIC_CONTROL_HINT_MAX must cover 6 controls
 * ────────────────────────────────────────────────────────────────────────── */

/**
 * Build a schema with 6 writable features that mirror a real device:
 *   plug_main       → ON_OFF_PLUGIN_UNIT (BOOL)
 *   light_main      → ON_OFF_LIGHT (BOOL)
 *   dimmer_main     → DIMMABLE_LIGHT (INT 0..100)
 *   fan_main        → FAN (INT 0..100)
 *   dryer_temperature → GENERIC_VALUE (INT 300..1000)
 *   drying_time     → GENERIC_VALUE (INT 0..180)
 *
 * Plus 3 read-only features (no matching tool):
 *   temperature_main → TEMPERATURE_SENSOR
 *   humidity_main    → HUMIDITY_SENSOR
 *   contact_main     → CONTACT_SENSOR
 *
 * Total: 6 tools, 9 features.
 */
static const char *SIX_CTRL_DEV = "sc-hint6";
#define SIX_CTRL_SNAP_ID 9001

static void setup_six_control_schema(void)
{
    device_store_add(SIX_CTRL_DEV, "Six Controls");

    device_schema_set_submitter(test_submitter);
    s_submit_called = false;
    TEST_ASSERT_EQUAL_INT(ESP_OK, device_schema_on_ready(SIX_CTRL_DEV));
    vTaskDelay(pdMS_TO_TICKS(200));
    TEST_ASSERT_TRUE(s_submit_called);

    /* begin: 6 tools, 9 features */
    gw_message_t begin = make_begin(SIX_CTRL_DEV, SIX_CTRL_SNAP_ID, 6, 9, 1);
    TEST_ASSERT_TRUE(device_schema_on_notify(SIX_CTRL_DEV, &begin));
    vTaskDelay(pdMS_TO_TICKS(50));

    /* tools: one per writable feature */
    gw_message_t t0 = make_tool_item(SIX_CTRL_DEV, SIX_CTRL_SNAP_ID, 0,
                                     "set_plug", 1 /* BOOL */, 0x01, 0, 0, 0);
    TEST_ASSERT_TRUE(device_schema_on_notify(SIX_CTRL_DEV, &t0));
    vTaskDelay(pdMS_TO_TICKS(50));

    gw_message_t t1 = make_tool_item(SIX_CTRL_DEV, SIX_CTRL_SNAP_ID, 1,
                                     "set_light", 1 /* BOOL */, 0x01, 0, 0, 0);
    TEST_ASSERT_TRUE(device_schema_on_notify(SIX_CTRL_DEV, &t1));
    vTaskDelay(pdMS_TO_TICKS(50));

    gw_message_t t2 = make_tool_item(SIX_CTRL_DEV, SIX_CTRL_SNAP_ID, 2,
                                     "set_dimmer", 2 /* INT */, 0x01,
                                     0, 100, 1);
    TEST_ASSERT_TRUE(device_schema_on_notify(SIX_CTRL_DEV, &t2));
    vTaskDelay(pdMS_TO_TICKS(50));

    gw_message_t t3 = make_tool_item(SIX_CTRL_DEV, SIX_CTRL_SNAP_ID, 3,
                                     "set_fan", 2 /* INT */, 0x01,
                                     0, 100, 1);
    TEST_ASSERT_TRUE(device_schema_on_notify(SIX_CTRL_DEV, &t3));
    vTaskDelay(pdMS_TO_TICKS(50));

    gw_message_t t4 = make_tool_item(SIX_CTRL_DEV, SIX_CTRL_SNAP_ID, 4,
                                     "set_dryer_temp", 2 /* INT */, 0x01,
                                     300, 1000, 5);
    TEST_ASSERT_TRUE(device_schema_on_notify(SIX_CTRL_DEV, &t4));
    vTaskDelay(pdMS_TO_TICKS(50));

    gw_message_t t5 = make_tool_item(SIX_CTRL_DEV, SIX_CTRL_SNAP_ID, 5,
                                     "set_dry_time", 2 /* INT */, 0x01,
                                     0, 180, 1);
    TEST_ASSERT_TRUE(device_schema_on_notify(SIX_CTRL_DEV, &t5));
    vTaskDelay(pdMS_TO_TICKS(50));

    /* features: 6 writable */
    gw_message_t f0 = make_feature_item(SIX_CTRL_DEV, SIX_CTRL_SNAP_ID, 0,
                                        "plug_main",
                                        GW_FEATURE_ON_OFF_PLUGIN_UNIT,
                                        GW_PROP_ON_OFF, "set_plug");
    TEST_ASSERT_TRUE(device_schema_on_notify(SIX_CTRL_DEV, &f0));
    vTaskDelay(pdMS_TO_TICKS(50));

    gw_message_t f1 = make_feature_item(SIX_CTRL_DEV, SIX_CTRL_SNAP_ID, 1,
                                        "light_main",
                                        GW_FEATURE_ON_OFF_LIGHT,
                                        GW_PROP_ON_OFF, "set_light");
    TEST_ASSERT_TRUE(device_schema_on_notify(SIX_CTRL_DEV, &f1));
    vTaskDelay(pdMS_TO_TICKS(50));

    gw_message_t f2 = make_feature_item(SIX_CTRL_DEV, SIX_CTRL_SNAP_ID, 2,
                                        "dimmer_main",
                                        GW_FEATURE_DIMMABLE_LIGHT,
                                        GW_PROP_LEVEL, "set_dimmer");
    TEST_ASSERT_TRUE(device_schema_on_notify(SIX_CTRL_DEV, &f2));
    vTaskDelay(pdMS_TO_TICKS(50));

    gw_message_t f3 = make_feature_item(SIX_CTRL_DEV, SIX_CTRL_SNAP_ID, 3,
                                        "fan_main",
                                        GW_FEATURE_FAN,
                                        GW_PROP_PERCENT_SETTING, "set_fan");
    TEST_ASSERT_TRUE(device_schema_on_notify(SIX_CTRL_DEV, &f3));
    vTaskDelay(pdMS_TO_TICKS(50));

    gw_message_t f4 = make_feature_item(SIX_CTRL_DEV, SIX_CTRL_SNAP_ID, 4,
                                        "dryer_temperature",
                                        GW_FEATURE_GENERIC_VALUE,
                                        GW_PROP_VALUE, "set_dryer_temp");
    TEST_ASSERT_TRUE(device_schema_on_notify(SIX_CTRL_DEV, &f4));
    vTaskDelay(pdMS_TO_TICKS(50));

    gw_message_t f5 = make_feature_item(SIX_CTRL_DEV, SIX_CTRL_SNAP_ID, 5,
                                        "drying_time",
                                        GW_FEATURE_GENERIC_VALUE,
                                        GW_PROP_VALUE, "set_dry_time");
    TEST_ASSERT_TRUE(device_schema_on_notify(SIX_CTRL_DEV, &f5));
    vTaskDelay(pdMS_TO_TICKS(50));

    /* features: 3 read-only (no tool) */
    gw_message_t f6 = make_feature_item(SIX_CTRL_DEV, SIX_CTRL_SNAP_ID, 6,
                                        "temperature_main",
                                        GW_FEATURE_TEMPERATURE_SENSOR,
                                        GW_PROP_TEMPERATURE, "");
    TEST_ASSERT_TRUE(device_schema_on_notify(SIX_CTRL_DEV, &f6));
    vTaskDelay(pdMS_TO_TICKS(50));

    gw_message_t f7 = make_feature_item(SIX_CTRL_DEV, SIX_CTRL_SNAP_ID, 7,
                                        "humidity_main",
                                        GW_FEATURE_HUMIDITY_SENSOR,
                                        GW_PROP_HUMIDITY, "");
    TEST_ASSERT_TRUE(device_schema_on_notify(SIX_CTRL_DEV, &f7));
    vTaskDelay(pdMS_TO_TICKS(50));

    gw_message_t f8 = make_feature_item(SIX_CTRL_DEV, SIX_CTRL_SNAP_ID, 8,
                                        "contact_main",
                                        GW_FEATURE_CONTACT_SENSOR,
                                        GW_PROP_CONTACT, "");
    TEST_ASSERT_TRUE(device_schema_on_notify(SIX_CTRL_DEV, &f8));
    vTaskDelay(pdMS_TO_TICKS(50));

    /* end */
    gw_message_t end = make_end(SIX_CTRL_DEV, SIX_CTRL_SNAP_ID, 6);
    TEST_ASSERT_TRUE(device_schema_on_notify(SIX_CTRL_DEV, &end));
    vTaskDelay(pdMS_TO_TICKS(200));
    complete_discovery();
    vTaskDelay(pdMS_TO_TICKS(200));

    /* Verify commit */
    device_schema_snapshot_t snap = {0};
    TEST_ASSERT_EQUAL_INT(ESP_OK, device_schema_get(SIX_CTRL_DEV, &snap));
    TEST_ASSERT_TRUE(snap.has_committed);
    TEST_ASSERT_EQUAL_UINT(6, snap.tool_count);
    TEST_ASSERT_EQUAL_UINT(9, snap.feature_count);
}

static void enable_all_six_exposures(void)
{
    device_schema_snapshot_t snap = {0};
    TEST_ASSERT_EQUAL_INT(ESP_OK, device_schema_get(SIX_CTRL_DEV, &snap));

    const char *feature_tool_map[][2] = {
        { "plug_main",          "set_plug" },
        { "light_main",         "set_light" },
        { "dimmer_main",        "set_dimmer" },
        { "fan_main",           "set_fan" },
        { "dryer_temperature",  "set_dryer_temp" },
        { "drying_time",        "set_dry_time" },
    };

    for (size_t i = 0; i < 6; i++) {
        const device_schema_tool_t *tool = NULL;
        for (size_t t = 0; t < snap.tool_count; t++) {
            if (strcmp(snap.tools[t].command, feature_tool_map[i][1]) == 0) {
                tool = &snap.tools[t];
                break;
            }
        }
        TEST_ASSERT_NOT_NULL(tool);
        setup_feature_exposure(SIX_CTRL_DEV, feature_tool_map[i][0],
                               feature_tool_map[i][1], tool);
    }
}

/* ── Regression test: 6 controls returned (the core bug) ─────────────── */

TEST_CASE("semantic hints return all 6 controls without truncation",
          "[mcp_exposure][regression]")
{
    TEST_ASSERT_EQUAL(ESP_OK, mcp_tool_exposure_init());
    setup_six_control_schema();
    enable_all_six_exposures();

    mcp_control_hint_t hints[MCP_SEMANTIC_CONTROL_HINT_MAX] = {0};
    size_t count = 0;
    bool truncated = false;

    TEST_ASSERT_EQUAL(ESP_OK,
        mcp_semantic_control_get_hints(
            SIX_CTRL_DEV,
            hints,
            MCP_SEMANTIC_CONTROL_HINT_MAX,
            &count,
            &truncated));

    TEST_ASSERT_EQUAL_UINT(6, count);
    TEST_ASSERT_FALSE(truncated);

    /* Verify every expected feature_id is present */
    TEST_ASSERT_EQUAL_STRING("plug_main", hints[0].feature_id);
    TEST_ASSERT_EQUAL_STRING("light_main", hints[1].feature_id);
    TEST_ASSERT_EQUAL_STRING("dimmer_main", hints[2].feature_id);
    TEST_ASSERT_EQUAL_STRING("fan_main", hints[3].feature_id);
    TEST_ASSERT_EQUAL_STRING("dryer_temperature", hints[4].feature_id);
    TEST_ASSERT_EQUAL_STRING("drying_time", hints[5].feature_id);

    /* Read-only features must NOT appear */
    for (size_t i = 0; i < count; i++) {
        TEST_ASSERT_TRUE(strcmp("temperature_main", hints[i].feature_id) != 0);
        TEST_ASSERT_TRUE(strcmp("humidity_main", hints[i].feature_id) != 0);
        TEST_ASSERT_TRUE(strcmp("contact_main", hints[i].feature_id) != 0);
    }
}

/* ── Boundary test: MCP_SEMANTIC_CONTROL_HINT_MAX == DEVICE_SCHEMA_MAX_FEATURES */

TEST_CASE("semantic hints support schema maximum feature count",
          "[mcp_exposure][boundary]")
{
    /* This is a compile-time invariant:
     * MCP_SEMANTIC_CONTROL_HINT_MAX must be >= DEVICE_SCHEMA_MAX_FEATURES.
     * We verify it at runtime too. */
    TEST_ASSERT_GREATER_OR_EQUAL_UINT(
        DEVICE_SCHEMA_MAX_FEATURES,
        MCP_SEMANTIC_CONTROL_HINT_MAX);

    /*
     * Build a device with DEVICE_SCHEMA_MAX_FEATURES writable features.
     * Each uses GENERIC_VALUE (INT) with a unique tool.
     */
    const char *dev_id = "sc-maxfeat";
    uint32_t snap_id = 9100;
    size_t n = DEVICE_SCHEMA_MAX_FEATURES;

    device_store_add(dev_id, "Max Features");
    device_schema_set_submitter(test_submitter);
    s_submit_called = false;
    TEST_ASSERT_EQUAL_INT(ESP_OK, device_schema_on_ready(dev_id));
    vTaskDelay(pdMS_TO_TICKS(200));
    TEST_ASSERT_TRUE(s_submit_called);

    gw_message_t begin = make_begin(dev_id, snap_id, n, n, 1);
    TEST_ASSERT_TRUE(device_schema_on_notify(dev_id, &begin));
    vTaskDelay(pdMS_TO_TICKS(50));

    for (size_t i = 0; i < n; i++) {
        char cmd[32];
        snprintf(cmd, sizeof(cmd), "set_val_%zu", i);

        gw_message_t tool = make_tool_item(dev_id, snap_id, i, cmd,
                                           2 /* INT */, 0x01, 0, 100, 1);
        TEST_ASSERT_TRUE(device_schema_on_notify(dev_id, &tool));
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    for (size_t i = 0; i < n; i++) {
        char fid[32];
        snprintf(fid, sizeof(fid), "feat_%zu", i);

        gw_message_t feat = make_feature_item(dev_id, snap_id, i, fid,
                                              GW_FEATURE_GENERIC_VALUE,
                                              GW_PROP_VALUE,
                                              NULL /* auto-link */);
        /* Set the feature_tool manually since make_feature_item sets it
         * from the parameter but we need to match the tool command. */
        char tool_cmd[32];
        snprintf(tool_cmd, sizeof(tool_cmd), "set_val_%zu", i);
        strlcpy(feat.feature_tool, tool_cmd, sizeof(feat.feature_tool));
        feat.has_feature_tool = 1;

        TEST_ASSERT_TRUE(device_schema_on_notify(dev_id, &feat));
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    gw_message_t end = make_end(dev_id, snap_id, n);
    TEST_ASSERT_TRUE(device_schema_on_notify(dev_id, &end));
    vTaskDelay(pdMS_TO_TICKS(200));
    complete_discovery();
    vTaskDelay(pdMS_TO_TICKS(200));

    /* Enable exposure for every feature */
    device_schema_snapshot_t snap = {0};
    TEST_ASSERT_EQUAL_INT(ESP_OK, device_schema_get(dev_id, &snap));
    TEST_ASSERT_TRUE(snap.has_committed);

    for (size_t i = 0; i < n; i++) {
        char fid[32];
        snprintf(fid, sizeof(fid), "feat_%zu", i);
        char cmd[32];
        snprintf(cmd, sizeof(cmd), "set_val_%zu", i);

        const device_schema_tool_t *tool = &snap.tools[i];
        TEST_ASSERT_EQUAL_INT(ESP_OK,
            mcp_tool_expose_feature(dev_id, fid, cmd, fid, tool));
    }

    /* Now call get_hints with full capacity — must return all N */
    mcp_control_hint_t hints[MCP_SEMANTIC_CONTROL_HINT_MAX] = {0};
    size_t count = 0;
    bool truncated = false;

    TEST_ASSERT_EQUAL(ESP_OK,
        mcp_semantic_control_get_hints(
            dev_id, hints, MCP_SEMANTIC_CONTROL_HINT_MAX,
            &count, &truncated));

    TEST_ASSERT_EQUAL_UINT(n, count);
    TEST_ASSERT_FALSE(truncated);
}

/* ── Truncation test: caller capacity < eligible controls ────────────── */

TEST_CASE("semantic hints report truncation when caller capacity is smaller",
          "[mcp_exposure][regression]")
{
    TEST_ASSERT_EQUAL(ESP_OK, mcp_tool_exposure_init());
    setup_six_control_schema();
    enable_all_six_exposures();

    /* Call with capacity = 4 — should truncate 2 */
    mcp_control_hint_t hints[4] = {0};
    size_t count = 0;
    bool truncated = false;

    TEST_ASSERT_EQUAL(ESP_OK,
        mcp_semantic_control_get_hints(
            SIX_CTRL_DEV,
            hints,
            4,
            &count,
            &truncated));

    TEST_ASSERT_EQUAL_UINT(4, count);
    TEST_ASSERT_TRUE(truncated);
}

/* ── Disable / re-enable round-trip ──────────────────────────────────── */

TEST_CASE("semantic hints reflect feature disable/enable toggle",
          "[mcp_exposure][regression]")
{
    TEST_ASSERT_EQUAL(ESP_OK, mcp_tool_exposure_init());
    setup_six_control_schema();
    enable_all_six_exposures();

    /* All 6 present */
    mcp_control_hint_t hints[MCP_SEMANTIC_CONTROL_HINT_MAX] = {0};
    size_t count = 0;
    bool truncated = false;

    TEST_ASSERT_EQUAL(ESP_OK,
        mcp_semantic_control_get_hints(
            SIX_CTRL_DEV, hints, MCP_SEMANTIC_CONTROL_HINT_MAX,
            &count, &truncated));
    TEST_ASSERT_EQUAL_UINT(6, count);

    /* Disable drying_time */
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        mcp_tool_exposure_set_feature_enabled(
            SIX_CTRL_DEV, "drying_time", false));

    /* Now only 5 */
    TEST_ASSERT_EQUAL(ESP_OK,
        mcp_semantic_control_get_hints(
            SIX_CTRL_DEV, hints, MCP_SEMANTIC_CONTROL_HINT_MAX,
            &count, &truncated));
    TEST_ASSERT_EQUAL_UINT(5, count);
    TEST_ASSERT_FALSE(truncated);

    bool found = false;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(hints[i].feature_id, "drying_time") == 0) {
            found = true;
            break;
        }
    }
    TEST_ASSERT_FALSE(found);

    /* Re-enable */
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        mcp_tool_exposure_set_feature_enabled(
            SIX_CTRL_DEV, "drying_time", true));

    TEST_ASSERT_EQUAL(ESP_OK,
        mcp_semantic_control_get_hints(
            SIX_CTRL_DEV, hints, MCP_SEMANTIC_CONTROL_HINT_MAX,
            &count, &truncated));
    TEST_ASSERT_EQUAL_UINT(6, count);

    found = false;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(hints[i].feature_id, "drying_time") == 0) {
            found = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(found);
}

/* ── No schema committed → NOT_FOUND ─────────────────────────────────── */

TEST_CASE("semantic hints return NOT_FOUND for unknown device",
          "[mcp_exposure]")
{
    TEST_ASSERT_EQUAL(ESP_OK, mcp_tool_exposure_init());

    mcp_control_hint_t hints[4] = {0};
    size_t count = 0;
    bool truncated = false;

    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
        mcp_semantic_control_get_hints(
            "nonexistent-device", hints, 4, &count, &truncated));

    TEST_ASSERT_EQUAL_UINT(0, count);
    TEST_ASSERT_FALSE(truncated);
}

/* ── Invalid arguments ───────────────────────────────────────────────── */

TEST_CASE("semantic hints return INVALID_ARG for NULL device_id",
          "[mcp_exposure]")
{
    mcp_control_hint_t hints[4] = {0};
    size_t count = 0;
    bool truncated = false;

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
        mcp_semantic_control_get_hints(NULL, hints, 4, &count, &truncated));
}

TEST_CASE("semantic hints return INVALID_ARG for NULL out_count",
          "[mcp_exposure]")
{
    mcp_control_hint_t hints[4] = {0};
    bool truncated = false;

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
        mcp_semantic_control_get_hints("dev", hints, 4, NULL, &truncated));
}

TEST_CASE("semantic hints return INVALID_ARG for NULL out_truncated",
          "[mcp_exposure]")
{
    mcp_control_hint_t hints[4] = {0};
    size_t count = 0;

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
        mcp_semantic_control_get_hints("dev", hints, 4, &count, NULL));
}

TEST_CASE("semantic hints return INVALID_ARG for non-NULL capacity with NULL out",
          "[mcp_exposure]")
{
    size_t count = 0;
    bool truncated = false;

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
        mcp_semantic_control_get_hints("dev", NULL, 4, &count, &truncated));
}

/* ── Zero capacity is valid (count only) ─────────────────────────────── */

TEST_CASE("semantic hints with zero capacity returns zero count",
          "[mcp_exposure]")
{
    TEST_ASSERT_EQUAL(ESP_OK, mcp_tool_exposure_init());
    setup_six_control_schema();
    enable_all_six_exposures();

    size_t count = 0;
    bool truncated = false;

    TEST_ASSERT_EQUAL(ESP_OK,
        mcp_semantic_control_get_hints(
            SIX_CTRL_DEV, NULL, 0, &count, &truncated));

    TEST_ASSERT_EQUAL_UINT(0, count);
    TEST_ASSERT_FALSE(truncated);
}
