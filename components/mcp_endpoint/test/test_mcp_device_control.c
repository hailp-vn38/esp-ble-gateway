/**
 * Comprehensive unit tests for Phase 8: device_control Describe/Read/Set.
 *
 * Covers:
 *   - describe: with/without feature filter, name inclusion
 *   - read: cache lookup (BOOL/INT), missing state
 *   - set: typed value dispatch (BOOL/INT), policy gates, async plan
 *   - format_result / format_completion (OK + error codes)
 *   - error paths: missing device, ambiguous names, type mismatch, etc.
 */

#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "cJSON.h"
#include "cbor_codec.h"
#include "device_schema.h"
#include "device_state.h"
#include "device_store.h"
#include "device_template.h"
#include "mcp_tool_exposure.h"

#include "../mcp_endpoint_internal.h"
#include "../mcp_semantic_control.h"
#include "test_mcp_transport.h"

/* ── Schema helpers ────────────────────────────────────────────────── */

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
    msg.has_feature_schema_version = 1;
    msg.feature_schema_version = 1;
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

/* ── Schema discovery: 2 tools (toggle=NONE, set_level=INT 0..100),
 *    3 features: light_state (BOOL,writable), brightness (INT,writable),
 *                temperature (INT, read-only — no matching tool) ─── */

static void feed_schema_discovery(const char *device_id, uint32_t snap_id)
{
    /* begin: 2 tools, 3 features, revision 1 */
    gw_message_t begin = make_begin(device_id, snap_id, 2, 3, 1);
    TEST_ASSERT_TRUE(device_schema_on_notify(device_id, &begin));
    vTaskDelay(pdMS_TO_TICKS(50));

    /* tool 0: toggle (NONE, idempotent) */
    gw_message_t tool0 = make_tool_item(device_id, snap_id, 0, "toggle",
                                        0 /* NONE */, 0x01, 0, 0, 0);
    TEST_ASSERT_TRUE(device_schema_on_notify(device_id, &tool0));
    vTaskDelay(pdMS_TO_TICKS(50));

    /* tool 1: set_level (INT 0..100 step 1, idempotent) */
    gw_message_t tool1 = make_tool_item(device_id, snap_id, 1, "set_level",
                                        2 /* INT */, 0x01, 0, 100, 1);
    TEST_ASSERT_TRUE(device_schema_on_notify(device_id, &tool1));
    vTaskDelay(pdMS_TO_TICKS(50));

    /* feature 0: light_state (ON_OFF_LIGHT, prop=ON_OFF, tool=toggle) */
    gw_message_t feat0 = make_feature_item(device_id, snap_id, 2,
                                           "light_state",
                                           GW_FEATURE_ON_OFF_LIGHT,
                                           GW_PROP_ON_OFF, "toggle");
    TEST_ASSERT_TRUE(device_schema_on_notify(device_id, &feat0));
    vTaskDelay(pdMS_TO_TICKS(50));

    /* feature 1: brightness (DIMMABLE_LIGHT, prop=LEVEL, tool=set_level) */
    gw_message_t feat1 = make_feature_item(device_id, snap_id, 3,
                                           "brightness",
                                           GW_FEATURE_DIMMABLE_LIGHT,
                                           GW_PROP_LEVEL, "set_level");
    TEST_ASSERT_TRUE(device_schema_on_notify(device_id, &feat1));
    vTaskDelay(pdMS_TO_TICKS(50));

    /* feature 2: temperature (TEMPERATURE_SENSOR, prop=TEMPERATURE, no tool) */
    gw_message_t feat2 = make_feature_item(device_id, snap_id, 4,
                                           "temperature",
                                           GW_FEATURE_TEMPERATURE_SENSOR,
                                           GW_PROP_TEMPERATURE, "");
    TEST_ASSERT_TRUE(device_schema_on_notify(device_id, &feat2));
    vTaskDelay(pdMS_TO_TICKS(50));

    /* end */
    gw_message_t end = make_end(device_id, snap_id, 2);
    TEST_ASSERT_TRUE(device_schema_on_notify(device_id, &end));
    vTaskDelay(pdMS_TO_TICKS(200));
}

/**
 * Run the full discovery protocol and commit the schema for device_id.
 * After this call, device_schema_get() returns a committed snapshot with
 * 2 tools and 3 features.
 */
static void setup_committed_schema(const char *device_id, const char *name,
                                   uint32_t snap_id)
{
    device_store_add(device_id, name);

    device_schema_set_submitter(test_submitter);
    s_submit_called = false;

    TEST_ASSERT_EQUAL_INT(ESP_OK, device_schema_on_ready(device_id));
    vTaskDelay(pdMS_TO_TICKS(200));

    TEST_ASSERT_TRUE(s_submit_called);
    feed_schema_discovery(device_id, snap_id);
    complete_discovery();
    vTaskDelay(pdMS_TO_TICKS(200));

    /* Verify commit */
    device_schema_snapshot_t snap = {0};
    TEST_ASSERT_EQUAL_INT(ESP_OK, device_schema_get(device_id, &snap));
    TEST_ASSERT_TRUE(snap.has_committed);
}

/**
 * Populate device_state for a BOOL feature via on_notify.
 */
static void set_state_bool(const char *device_id, const char *feature_id,
                           uint8_t property_id, bool value)
{
    gw_message_t msg = {0};
    msg.protocol_version = GW_PROTOCOL_VERSION;
    strlcpy(msg.type, "device_event", sizeof(msg.type));
    strlcpy(msg.command, "feature_state", sizeof(msg.command));
    strlcpy(msg.device_id, device_id, sizeof(msg.device_id));
    msg.has_device_id = true;
    strlcpy(msg.feature_id, feature_id, sizeof(msg.feature_id));
    msg.has_feature_id = true;
    msg.property_id = property_id;
    msg.has_property_id = true;
    msg.feature_value_bool = value;
    msg.has_feature_value_bool = true;
    device_state_on_notify(device_id, &msg);
}

/**
 * Populate device_state for an INT feature via on_notify.
 */
static void set_state_int(const char *device_id, const char *feature_id,
                          uint8_t property_id, int32_t value)
{
    gw_message_t msg = {0};
    msg.protocol_version = GW_PROTOCOL_VERSION;
    strlcpy(msg.type, "device_event", sizeof(msg.type));
    strlcpy(msg.command, "feature_state", sizeof(msg.command));
    strlcpy(msg.device_id, device_id, sizeof(msg.device_id));
    msg.has_device_id = true;
    strlcpy(msg.feature_id, feature_id, sizeof(msg.feature_id));
    msg.has_feature_id = true;
    msg.property_id = property_id;
    msg.has_property_id = true;
    msg.feature_value_int = value;
    msg.has_feature_value_int = true;
    device_state_on_notify(device_id, &msg);
}

/**
 * Set up an exposure record for a writable feature.
 * After this call, the feature passes policy checks for SET operations.
 */
static void setup_feature_exposure(const char *device_id,
                                   const char *feature_id,
                                   const char *command)
{
    TEST_ASSERT_EQUAL_INT(ESP_OK, mcp_tool_exposure_init());
    device_schema_snapshot_t snap = {0};
    TEST_ASSERT_EQUAL_INT(ESP_OK, device_schema_get(device_id, &snap));
    TEST_ASSERT_TRUE(snap.has_committed);

    const device_schema_tool_t *tool = NULL;
    for (size_t i = 0; i < snap.tool_count; i++) {
        if (strcmp(snap.tools[i].command, command) == 0) {
            tool = &snap.tools[i];
            break;
        }
    }
    TEST_ASSERT_NOT_NULL(tool);

    TEST_ASSERT_EQUAL_INT(ESP_OK,
        mcp_tool_expose_feature(device_id, feature_id, command,
                                feature_id, tool));
}

/* ── Protocol context helpers ──────────────────────────────────────── */

static mcp_request_context_t make_ctx_2025(void)
{
    mcp_request_context_t ctx = {0};
    ctx.era = MCP_ERA_2025_11_25;
    return ctx;
}

/* ── Params builders ───────────────────────────────────────────────── */

static cJSON *params_describe(const char *device, const char *feature)
{
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "operation", "describe");
    cJSON_AddStringToObject(p, "device", device);
    if (feature != NULL) cJSON_AddStringToObject(p, "feature", feature);
    return p;
}

static cJSON *params_read(const char *device, const char *feature)
{
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "operation", "read");
    cJSON_AddStringToObject(p, "device", device);
    cJSON_AddStringToObject(p, "feature", feature);
    return p;
}

static cJSON *params_set_bool(const char *device, const char *feature,
                              bool value)
{
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "operation", "set");
    cJSON_AddStringToObject(p, "device", device);
    cJSON_AddStringToObject(p, "feature", feature);
    cJSON_AddBoolToObject(p, "bool_value", value);
    return p;
}

static cJSON *params_set_int(const char *device, const char *feature,
                             int32_t value)
{
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "operation", "set");
    cJSON_AddStringToObject(p, "device", device);
    cJSON_AddStringToObject(p, "feature", feature);
    cJSON_AddNumberToObject(p, "int_value", value);
    return p;
}

/* Helper: parse the inner semantic payload from a formatted MCP result */
static cJSON *parse_inner_result(cJSON *plan_result)
{
    cJSON *content = cJSON_GetObjectItem(plan_result, "content");
    TEST_ASSERT_NOT_NULL(content);
    cJSON *text_item = cJSON_GetArrayItem(content, 0);
    TEST_ASSERT_NOT_NULL(text_item);
    cJSON *text = cJSON_GetObjectItem(text_item, "text");
    TEST_ASSERT_NOT_NULL(text);
    return cJSON_Parse(text->valuestring);
}

/* ════════════════════════════════════════════════════════════════════
 *  DESCRIBE tests
 * ════════════════════════════════════════════════════════════════════ */

TEST_CASE("describe: all features (no filter)", "[device_control]")
{
    setup_committed_schema("dc-desc1", "Living Room", 2001);

    mcp_request_context_t ctx = make_ctx_2025();
    mcp_device_control_plan_t plan = {0};
    cJSON *params = params_describe("dc-desc1", NULL);

    esp_err_t err = mcp_device_control_resolve(params, &ctx, &plan);
    cJSON_Delete(params);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_EQUAL(MCP_DEVICE_CONTROL_EXEC_LOCAL, plan.kind);

    cJSON *inner = parse_inner_result(plan.local_result);
    TEST_ASSERT_NOT_NULL(inner);
    cJSON *features = cJSON_GetObjectItem(inner, "features");
    TEST_ASSERT_NOT_NULL(features);
    TEST_ASSERT_EQUAL_INT(3, cJSON_GetArraySize(features));

    /* light_state: ON_OFF_LIGHT → semantic "light", writable (toggle) */
    cJSON *f0 = cJSON_GetArrayItem(features, 0);
    TEST_ASSERT_EQUAL_STRING("light_state",
        cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(f0, "feature_id")));
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(f0, "writable")));

    /* brightness: DIMMABLE_LIGHT → semantic "light", writable (set_level) */
    cJSON *f1 = cJSON_GetArrayItem(features, 1);
    TEST_ASSERT_EQUAL_STRING("brightness",
        cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(f1, "feature_id")));
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(f1, "writable")));
    /* INT feature should have min/max/step */
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(f1, "minimum"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(f1, "maximum"));

    /* temperature: read-only (no matching tool) */
    cJSON *f2 = cJSON_GetArrayItem(features, 2);
    TEST_ASSERT_EQUAL_STRING("temperature",
        cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(f2, "feature_id")));
    TEST_ASSERT_FALSE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(f2, "writable")));

    cJSON_Delete(inner);
    cJSON_Delete(plan.local_result);
}

TEST_CASE("describe: single feature filter", "[device_control]")
{
    setup_committed_schema("dc-desc2", "Kitchen", 2002);

    mcp_request_context_t ctx = make_ctx_2025();
    mcp_device_control_plan_t plan = {0};
    cJSON *params = params_describe("dc-desc2", "brightness");

    esp_err_t err = mcp_device_control_resolve(params, &ctx, &plan);
    cJSON_Delete(params);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_EQUAL(MCP_DEVICE_CONTROL_EXEC_LOCAL, plan.kind);

    cJSON *inner = parse_inner_result(plan.local_result);
    cJSON *features = cJSON_GetObjectItem(inner, "features");
    TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(features));

    cJSON *f0 = cJSON_GetArrayItem(features, 0);
    TEST_ASSERT_EQUAL_STRING("brightness",
        cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(f0, "feature_id")));
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(f0, "writable")));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(f0, "minimum"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(f0, "maximum"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(f0, "step"));

    cJSON_Delete(inner);
    cJSON_Delete(plan.local_result);
}

TEST_CASE("describe: device includes name", "[device_control]")
{
    setup_committed_schema("dc-desc3", "Bedroom", 2003);

    mcp_request_context_t ctx = make_ctx_2025();
    mcp_device_control_plan_t plan = {0};
    cJSON *params = params_describe("dc-desc3", NULL);

    esp_err_t err = mcp_device_control_resolve(params, &ctx, &plan);
    cJSON_Delete(params);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);

    cJSON *inner = parse_inner_result(plan.local_result);
    TEST_ASSERT_EQUAL_STRING("Bedroom",
        cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(inner, "name")));
    TEST_ASSERT_EQUAL_STRING("dc-desc3",
        cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(inner, "device_id")));

    cJSON_Delete(inner);
    cJSON_Delete(plan.local_result);
}

/* ════════════════════════════════════════════════════════════════════
 *  READ tests
 * ════════════════════════════════════════════════════════════════════ */

TEST_CASE("read: BOOL feature returns cached value", "[device_control]")
{
    setup_committed_schema("dc-read1", "Office", 3001);
    set_state_bool("dc-read1", "light_state", GW_PROP_ON_OFF, true);

    mcp_request_context_t ctx = make_ctx_2025();
    mcp_device_control_plan_t plan = {0};
    cJSON *params = params_read("dc-read1", "light_state");

    esp_err_t err = mcp_device_control_resolve(params, &ctx, &plan);
    cJSON_Delete(params);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_EQUAL(MCP_DEVICE_CONTROL_EXEC_LOCAL, plan.kind);

    cJSON *inner = parse_inner_result(plan.local_result);
    TEST_ASSERT_EQUAL_STRING("dc-read1",
        cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(inner, "device_id")));
    TEST_ASSERT_EQUAL_STRING("light_state",
        cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(inner, "feature_id")));
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(inner, "value")));

    cJSON_Delete(inner);
    cJSON_Delete(plan.local_result);
}

TEST_CASE("read: INT feature returns cached value", "[device_control]")
{
    setup_committed_schema("dc-read2", "Garage", 3002);
    set_state_int("dc-read2", "brightness", GW_PROP_LEVEL, 75);

    mcp_request_context_t ctx = make_ctx_2025();
    mcp_device_control_plan_t plan = {0};
    cJSON *params = params_read("dc-read2", "brightness");

    esp_err_t err = mcp_device_control_resolve(params, &ctx, &plan);
    cJSON_Delete(params);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_EQUAL(MCP_DEVICE_CONTROL_EXEC_LOCAL, plan.kind);

    cJSON *inner = parse_inner_result(plan.local_result);
    cJSON *val = cJSON_GetObjectItemCaseSensitive(inner, "value");
    TEST_ASSERT_NOT_NULL(val);
    TEST_ASSERT_EQUAL_INT(75, val->valueint);

    cJSON_Delete(inner);
    cJSON_Delete(plan.local_result);
}

TEST_CASE("read: no state returns error", "[device_control]")
{
    setup_committed_schema("dc-read3", "Patio", 3003);
    /* No set_state call — state not available */

    mcp_request_context_t ctx = make_ctx_2025();
    mcp_device_control_plan_t plan = {0};
    cJSON *params = params_read("dc-read3", "light_state");

    esp_err_t err = mcp_device_control_resolve(params, &ctx, &plan);
    cJSON_Delete(params);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_EQUAL(MCP_DEVICE_CONTROL_EXEC_LOCAL, plan.kind);

    cJSON *is_error = cJSON_GetObjectItem(plan.local_result, "isError");
    TEST_ASSERT_NOT_NULL(is_error);
    TEST_ASSERT_TRUE(cJSON_IsTrue(is_error));

    cJSON *inner = parse_inner_result(plan.local_result);
    cJSON *err_field = cJSON_GetObjectItemCaseSensitive(inner, "error");
    TEST_ASSERT_EQUAL_STRING("state_not_available", err_field->valuestring);

    cJSON_Delete(inner);
    cJSON_Delete(plan.local_result);
}

/* ════════════════════════════════════════════════════════════════════
 *  SET tests
 * ════════════════════════════════════════════════════════════════════ */

TEST_CASE("set: BOOL success — async plan", "[device_control]")
{
    setup_committed_schema("dc-set1", "Switch1", 4001);
    setup_feature_exposure("dc-set1", "light_state", "toggle");

    mcp_request_context_t ctx = make_ctx_2025();
    mcp_device_control_plan_t plan = {0};
    cJSON *params = params_set_bool("dc-set1", "light_state", true);

    esp_err_t err = mcp_device_control_resolve(params, &ctx, &plan);
    cJSON_Delete(params);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_EQUAL(MCP_DEVICE_CONTROL_EXEC_ASYNC_SET, plan.kind);

    /* Verify request fields */
    TEST_ASSERT_EQUAL(DEVICE_CMD_ORIGIN_CONTROL, plan.request.origin);
    TEST_ASSERT_EQUAL_STRING("dc-set1", plan.request.device_id);
    TEST_ASSERT_EQUAL_STRING("toggle", plan.request.command);
    TEST_ASSERT_EQUAL_STRING("light_state", plan.request.feature_id);
    TEST_ASSERT_TRUE(plan.request.has_bool_value);
    TEST_ASSERT_TRUE(plan.request.bool_value);
    TEST_ASSERT_TRUE(plan.request.has_feature_id);
    TEST_ASSERT_TRUE(plan.request.has_property_id);

    /* Plan context fields */
    TEST_ASSERT_EQUAL_STRING("dc-set1", plan.device_id);
    TEST_ASSERT_EQUAL_STRING("light_state", plan.feature_id);
}

TEST_CASE("set: INT success — async plan", "[device_control]")
{
    setup_committed_schema("dc-set2", "Dimmer1", 4002);
    setup_feature_exposure("dc-set2", "brightness", "set_level");

    mcp_request_context_t ctx = make_ctx_2025();
    mcp_device_control_plan_t plan = {0};
    cJSON *params = params_set_int("dc-set2", "brightness", 50);

    esp_err_t err = mcp_device_control_resolve(params, &ctx, &plan);
    cJSON_Delete(params);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_EQUAL(MCP_DEVICE_CONTROL_EXEC_ASYNC_SET, plan.kind);

    TEST_ASSERT_EQUAL(DEVICE_CMD_ORIGIN_CONTROL, plan.request.origin);
    TEST_ASSERT_EQUAL_STRING("set_level", plan.request.command);
    TEST_ASSERT_TRUE(plan.request.has_int_value);
    TEST_ASSERT_EQUAL_INT(50, plan.request.int_value);
    TEST_ASSERT_EQUAL_STRING("brightness", plan.request.feature_id);
}

TEST_CASE("set: read-only feature denied", "[device_control]")
{
    /* temperature feature has no matching tool → writable_tool_index = -1 */
    setup_committed_schema("dc-set-ro", "RODev", 4010);

    mcp_request_context_t ctx = make_ctx_2025();
    mcp_device_control_plan_t plan = {0};
    cJSON *params = params_set_int("dc-set-ro", "temperature", 25);

    esp_err_t err = mcp_device_control_resolve(params, &ctx, &plan);
    cJSON_Delete(params);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_EQUAL(MCP_DEVICE_CONTROL_EXEC_LOCAL, plan.kind);

    cJSON *inner = parse_inner_result(plan.local_result);
    cJSON *err_field = cJSON_GetObjectItemCaseSensitive(inner, "error");
    TEST_ASSERT_NOT_NULL(err_field);
    TEST_ASSERT_EQUAL_STRING("feature_read_only", err_field->valuestring);

    cJSON_Delete(inner);
    cJSON_Delete(plan.local_result);
}

TEST_CASE("set: no exposure — control denied", "[device_control]")
{
    setup_committed_schema("dc-set-noexp", "NoExp", 4011);
    /* Deliberately NOT calling setup_feature_exposure */

    mcp_request_context_t ctx = make_ctx_2025();
    mcp_device_control_plan_t plan = {0};
    cJSON *params = params_set_int("dc-set-noexp", "brightness", 50);

    esp_err_t err = mcp_device_control_resolve(params, &ctx, &plan);
    cJSON_Delete(params);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_EQUAL(MCP_DEVICE_CONTROL_EXEC_LOCAL, plan.kind);

    cJSON *inner = parse_inner_result(plan.local_result);
    cJSON *err_field = cJSON_GetObjectItemCaseSensitive(inner, "error");
    TEST_ASSERT_EQUAL_STRING("control_denied", err_field->valuestring);

    cJSON_Delete(inner);
    cJSON_Delete(plan.local_result);
}

TEST_CASE("set: exposure disabled — control denied", "[device_control]")
{
    setup_committed_schema("dc-set-dis", "DisDev", 4012);
    setup_feature_exposure("dc-set-dis", "brightness", "set_level");

    /* Disable the exposure */
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        mcp_tool_exposure_set_feature_enabled("dc-set-dis", "brightness", false));

    mcp_request_context_t ctx = make_ctx_2025();
    mcp_device_control_plan_t plan = {0};
    cJSON *params = params_set_int("dc-set-dis", "brightness", 50);

    esp_err_t err = mcp_device_control_resolve(params, &ctx, &plan);
    cJSON_Delete(params);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_EQUAL(MCP_DEVICE_CONTROL_EXEC_LOCAL, plan.kind);

    cJSON *inner = parse_inner_result(plan.local_result);
    cJSON *err_field = cJSON_GetObjectItemCaseSensitive(inner, "error");
    TEST_ASSERT_EQUAL_STRING("control_denied", err_field->valuestring);

    cJSON_Delete(inner);
    cJSON_Delete(plan.local_result);
}

/* ════════════════════════════════════════════════════════════════════
 *  ERROR path tests
 * ════════════════════════════════════════════════════════════════════ */

TEST_CASE("missing operation — -32602", "[device_control]")
{
    mcp_request_context_t ctx = make_ctx_2025();
    mcp_device_control_plan_t plan = {0};
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "device", "any");

    esp_err_t err = mcp_device_control_resolve(params, &ctx, &plan);
    cJSON_Delete(params);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_EQUAL_INT(-32602, plan.error.code);
    TEST_ASSERT_EQUAL(MCP_DEVICE_CONTROL_EXEC_ERROR, plan.kind);
}

TEST_CASE("invalid operation — -32602", "[device_control]")
{
    mcp_request_context_t ctx = make_ctx_2025();
    mcp_device_control_plan_t plan = {0};
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "operation", "delete");
    cJSON_AddStringToObject(params, "device", "any");

    esp_err_t err = mcp_device_control_resolve(params, &ctx, &plan);
    cJSON_Delete(params);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_EQUAL_INT(-32602, plan.error.code);
}

TEST_CASE("device not found", "[device_control]")
{
    mcp_request_context_t ctx = make_ctx_2025();
    mcp_device_control_plan_t plan = {0};
    cJSON *params = params_describe("nonexistent", NULL);

    esp_err_t err = mcp_device_control_resolve(params, &ctx, &plan);
    cJSON_Delete(params);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);

    TEST_ASSERT_EQUAL(MCP_DEVICE_CONTROL_EXEC_LOCAL, plan.kind);
    cJSON *is_error = cJSON_GetObjectItem(plan.local_result, "isError");
    TEST_ASSERT_TRUE(cJSON_IsTrue(is_error));

    cJSON *inner = parse_inner_result(plan.local_result);
    cJSON *err_field = cJSON_GetObjectItemCaseSensitive(inner, "error");
    TEST_ASSERT_EQUAL_STRING("device_not_found", err_field->valuestring);

    cJSON_Delete(inner);
    cJSON_Delete(plan.local_result);
}

TEST_CASE("ambiguous device name", "[device_control]")
{
    device_store_add("dc-amb-a", "Same Name");
    device_store_add("dc-amb-b", "Same Name");

    mcp_request_context_t ctx = make_ctx_2025();
    mcp_device_control_plan_t plan = {0};
    cJSON *params = params_describe("Same Name", NULL);

    esp_err_t err = mcp_device_control_resolve(params, &ctx, &plan);
    cJSON_Delete(params);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);

    TEST_ASSERT_EQUAL(MCP_DEVICE_CONTROL_EXEC_LOCAL, plan.kind);
    cJSON *is_error = cJSON_GetObjectItem(plan.local_result, "isError");
    TEST_ASSERT_TRUE(cJSON_IsTrue(is_error));

    cJSON *inner = parse_inner_result(plan.local_result);
    cJSON *err_field = cJSON_GetObjectItemCaseSensitive(inner, "error");
    TEST_ASSERT_EQUAL_STRING("ambiguous_device", err_field->valuestring);

    cJSON_Delete(inner);
    cJSON_Delete(plan.local_result);
}

TEST_CASE("ambiguous feature name", "[device_control]")
{
    /* Two ON_OFF_LIGHT features → both have semantic name "light" */
    device_store_add("dc-amb-feat", "AmbFeat");

    device_schema_set_submitter(test_submitter);
    s_submit_called = false;
    TEST_ASSERT_EQUAL_INT(ESP_OK, device_schema_on_ready("dc-amb-feat"));
    vTaskDelay(pdMS_TO_TICKS(200));

    gw_message_t begin = make_begin("dc-amb-feat", 5001, 1, 2, 1);
    TEST_ASSERT_TRUE(device_schema_on_notify("dc-amb-feat", &begin));
    vTaskDelay(pdMS_TO_TICKS(50));

    gw_message_t tool0 = make_tool_item("dc-amb-feat", 5001, 0, "toggle",
                                        0, 0x01, 0, 0, 0);
    TEST_ASSERT_TRUE(device_schema_on_notify("dc-amb-feat", &tool0));
    vTaskDelay(pdMS_TO_TICKS(50));

    /* feature 0: ON_OFF_LIGHT → semantic "light" */
    gw_message_t feat0 = make_feature_item("dc-amb-feat", 5001, 1,
                                           "light_a",
                                           GW_FEATURE_ON_OFF_LIGHT,
                                           GW_PROP_ON_OFF, "toggle");
    TEST_ASSERT_TRUE(device_schema_on_notify("dc-amb-feat", &feat0));
    vTaskDelay(pdMS_TO_TICKS(50));

    /* feature 1: also ON_OFF_LIGHT → semantic "light" (ambiguous!) */
    gw_message_t feat1 = make_feature_item("dc-amb-feat", 5001, 2,
                                           "light_b",
                                           GW_FEATURE_ON_OFF_LIGHT,
                                           GW_PROP_ON_OFF, "toggle");
    TEST_ASSERT_TRUE(device_schema_on_notify("dc-amb-feat", &feat1));
    vTaskDelay(pdMS_TO_TICKS(50));

    gw_message_t end = make_end("dc-amb-feat", 5001, 1);
    TEST_ASSERT_TRUE(device_schema_on_notify("dc-amb-feat", &end));
    vTaskDelay(pdMS_TO_TICKS(200));
    complete_discovery();
    vTaskDelay(pdMS_TO_TICKS(200));

    mcp_request_context_t ctx = make_ctx_2025();
    mcp_device_control_plan_t plan = {0};
    /* Use semantic name "light" — matches two features */
    cJSON *params = params_read("dc-amb-feat", "light");

    esp_err_t err = mcp_device_control_resolve(params, &ctx, &plan);
    cJSON_Delete(params);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);

    TEST_ASSERT_EQUAL(MCP_DEVICE_CONTROL_EXEC_LOCAL, plan.kind);
    cJSON *inner = parse_inner_result(plan.local_result);
    cJSON *err_field = cJSON_GetObjectItemCaseSensitive(inner, "error");
    TEST_ASSERT_EQUAL_STRING("ambiguous_feature", err_field->valuestring);

    cJSON_Delete(inner);
    cJSON_Delete(plan.local_result);
}

TEST_CASE("feature not found", "[device_control]")
{
    setup_committed_schema("dc-feat-nf", "NotFound", 6001);

    mcp_request_context_t ctx = make_ctx_2025();
    mcp_device_control_plan_t plan = {0};
    cJSON *params = params_read("dc-feat-nf", "nonexistent_feature");

    esp_err_t err = mcp_device_control_resolve(params, &ctx, &plan);
    cJSON_Delete(params);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);

    TEST_ASSERT_EQUAL(MCP_DEVICE_CONTROL_EXEC_LOCAL, plan.kind);
    cJSON *inner = parse_inner_result(plan.local_result);
    cJSON *err_field = cJSON_GetObjectItemCaseSensitive(inner, "error");
    TEST_ASSERT_EQUAL_STRING("feature_not_found", err_field->valuestring);

    cJSON_Delete(inner);
    cJSON_Delete(plan.local_result);
}

TEST_CASE("read without feature — -32602", "[device_control]")
{
    setup_committed_schema("dc-read-nofeat", "NoFeat", 7001);

    mcp_request_context_t ctx = make_ctx_2025();
    mcp_device_control_plan_t plan = {0};
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "operation", "read");
    cJSON_AddStringToObject(p, "device", "dc-read-nofeat");

    esp_err_t err = mcp_device_control_resolve(p, &ctx, &plan);
    cJSON_Delete(p);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_EQUAL_INT(-32602, plan.error.code);
}

TEST_CASE("set without feature — -32602", "[device_control]")
{
    setup_committed_schema("dc-set-nofeat", "NoFeatSet", 7002);

    mcp_request_context_t ctx = make_ctx_2025();
    mcp_device_control_plan_t plan = {0};
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "operation", "set");
    cJSON_AddStringToObject(p, "device", "dc-set-nofeat");
    cJSON_AddBoolToObject(p, "bool_value", true);

    esp_err_t err = mcp_device_control_resolve(p, &ctx, &plan);
    cJSON_Delete(p);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_EQUAL_INT(-32602, plan.error.code);
}

TEST_CASE("set BOOL with int_value — type mismatch", "[device_control]")
{
    setup_committed_schema("dc-set-type", "TypeDev", 7003);
    setup_feature_exposure("dc-set-type", "light_state", "toggle");

    mcp_request_context_t ctx = make_ctx_2025();
    mcp_device_control_plan_t plan = {0};
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "operation", "set");
    cJSON_AddStringToObject(p, "device", "dc-set-type");
    cJSON_AddStringToObject(p, "feature", "light_state");
    cJSON_AddNumberToObject(p, "int_value", 42);

    esp_err_t err = mcp_device_control_resolve(p, &ctx, &plan);
    cJSON_Delete(p);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_EQUAL_INT(-32602, plan.error.code);
}

TEST_CASE("set with both bool_value and int_value — -32602", "[device_control]")
{
    setup_committed_schema("dc-set-both", "BothDev", 7004);
    setup_feature_exposure("dc-set-both", "brightness", "set_level");

    mcp_request_context_t ctx = make_ctx_2025();
    mcp_device_control_plan_t plan = {0};
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "operation", "set");
    cJSON_AddStringToObject(p, "device", "dc-set-both");
    cJSON_AddStringToObject(p, "feature", "brightness");
    cJSON_AddBoolToObject(p, "bool_value", true);
    cJSON_AddNumberToObject(p, "int_value", 50);

    esp_err_t err = mcp_device_control_resolve(p, &ctx, &plan);
    cJSON_Delete(p);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_EQUAL_INT(-32602, plan.error.code);
}

TEST_CASE("set with neither bool_value nor int_value — -32602", "[device_control]")
{
    setup_committed_schema("dc-set-none", "NoneDev", 7005);
    setup_feature_exposure("dc-set-none", "brightness", "set_level");

    mcp_request_context_t ctx = make_ctx_2025();
    mcp_device_control_plan_t plan = {0};
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "operation", "set");
    cJSON_AddStringToObject(p, "device", "dc-set-none");
    cJSON_AddStringToObject(p, "feature", "brightness");

    esp_err_t err = mcp_device_control_resolve(p, &ctx, &plan);
    cJSON_Delete(p);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_EQUAL_INT(-32602, plan.error.code);
}

TEST_CASE("params not an object — -32602", "[device_control]")
{
    mcp_request_context_t ctx = make_ctx_2025();
    mcp_device_control_plan_t plan = {0};
    cJSON *params = cJSON_CreateString("bad");

    esp_err_t err = mcp_device_control_resolve(params, &ctx, &plan);
    cJSON_Delete(params);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_EQUAL_INT(-32602, plan.error.code);
}

TEST_CASE("NULL out returns INVALID_ARG", "[device_control]")
{
    mcp_request_context_t ctx = make_ctx_2025();
    cJSON *params = params_describe("any", NULL);
    esp_err_t err = mcp_device_control_resolve(params, &ctx, NULL);
    cJSON_Delete(params);
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, err);
}

TEST_CASE("NULL protocol returns INVALID_ARG", "[device_control]")
{
    mcp_device_control_plan_t plan = {0};
    cJSON *params = params_describe("any", NULL);
    esp_err_t err = mcp_device_control_resolve(params, NULL, &plan);
    cJSON_Delete(params);
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, err);
}

TEST_CASE("capabilities_not_ready — no committed schema", "[device_control]")
{
    /* Device exists but schema not committed */
    device_store_add("dc-no-schema", "NoSchema");

    mcp_request_context_t ctx = make_ctx_2025();
    mcp_device_control_plan_t plan = {0};
    cJSON *params = params_read("dc-no-schema", "anything");

    esp_err_t err = mcp_device_control_resolve(params, &ctx, &plan);
    cJSON_Delete(params);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);

    TEST_ASSERT_EQUAL(MCP_DEVICE_CONTROL_EXEC_LOCAL, plan.kind);
    cJSON *inner = parse_inner_result(plan.local_result);
    cJSON *err_field = cJSON_GetObjectItemCaseSensitive(inner, "error");
    TEST_ASSERT_EQUAL_STRING("capabilities_not_ready", err_field->valuestring);

    cJSON_Delete(inner);
    cJSON_Delete(plan.local_result);
}

/* ════════════════════════════════════════════════════════════════════
 *  format_result tests
 * ════════════════════════════════════════════════════════════════════ */

TEST_CASE("format_result: success payload", "[device_control]")
{
    mcp_request_context_t ctx = make_ctx_2025();
    mcp_rpc_error_t error = {0};
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "device_id", "x");

    cJSON *result = mcp_device_control_format_result(payload, false, &ctx,
                                                     &error);
    cJSON_Delete(payload);
    TEST_ASSERT_NOT_NULL(result);

    cJSON *is_error = cJSON_GetObjectItem(result, "isError");
    TEST_ASSERT_NOT_NULL(is_error);
    TEST_ASSERT_FALSE(cJSON_IsTrue(is_error));

    cJSON *content = cJSON_GetObjectItem(result, "content");
    TEST_ASSERT_NOT_NULL(content);
    TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(content));

    cJSON *sc = cJSON_GetObjectItem(result, "structuredContent");
    TEST_ASSERT_NOT_NULL(sc);

    cJSON_Delete(result);
}

TEST_CASE("format_result: error payload", "[device_control]")
{
    mcp_request_context_t ctx = make_ctx_2025();
    mcp_rpc_error_t error = {0};
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "error", "device_not_found");

    cJSON *result = mcp_device_control_format_result(payload, true, &ctx,
                                                     &error);
    cJSON_Delete(payload);
    TEST_ASSERT_NOT_NULL(result);

    cJSON *is_error = cJSON_GetObjectItem(result, "isError");
    TEST_ASSERT_TRUE(cJSON_IsTrue(is_error));

    cJSON_Delete(result);
}

/* ════════════════════════════════════════════════════════════════════
 *  format_completion tests
 * ════════════════════════════════════════════════════════════════════ */

TEST_CASE("format_completion: OK status", "[device_control]")
{
    mcp_request_context_t ctx = make_ctx_2025();
    mcp_rpc_error_t error = {0};
    device_command_result_t result = {0};
    result.status = DEVICE_CMD_STATUS_OK;
    result.accepted = true;

    cJSON *out = mcp_device_control_format_completion("dev1", "feat1",
                                                      &result, &ctx, &error);
    TEST_ASSERT_NOT_NULL(out);

    cJSON *inner = parse_inner_result(out);
    TEST_ASSERT_EQUAL_STRING("dev1",
        cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(inner, "device_id")));
    TEST_ASSERT_EQUAL_STRING("feat1",
        cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(inner, "feature_id")));
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(inner, "success")));

    cJSON_Delete(inner);
    cJSON_Delete(out);
}

TEST_CASE("format_completion: timeout error", "[device_control]")
{
    mcp_request_context_t ctx = make_ctx_2025();
    mcp_rpc_error_t error = {0};
    device_command_result_t result = {0};
    result.status = DEVICE_CMD_STATUS_TIMEOUT;

    cJSON *out = mcp_device_control_format_completion("dev1", "feat1",
                                                      &result, &ctx, &error);
    TEST_ASSERT_NOT_NULL(out);

    cJSON *inner = parse_inner_result(out);
    TEST_ASSERT_FALSE(cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(inner, "success")));
    cJSON *err = cJSON_GetObjectItemCaseSensitive(inner, "error");
    TEST_ASSERT_NOT_NULL(err);
    TEST_ASSERT_EQUAL_STRING("timeout", err->valuestring);

    cJSON_Delete(inner);
    cJSON_Delete(out);
}

TEST_CASE("format_completion: device_rejected error", "[device_control]")
{
    mcp_request_context_t ctx = make_ctx_2025();
    mcp_rpc_error_t error = {0};
    device_command_result_t result = {0};
    result.status = DEVICE_CMD_STATUS_DEVICE_REJECTED;

    cJSON *out = mcp_device_control_format_completion("dev1", "feat1",
                                                      &result, &ctx, &error);
    TEST_ASSERT_NOT_NULL(out);

    cJSON *inner = parse_inner_result(out);
    cJSON *err = cJSON_GetObjectItemCaseSensitive(inner, "error");
    TEST_ASSERT_EQUAL_STRING("device_rejected", err->valuestring);

    cJSON_Delete(inner);
    cJSON_Delete(out);
}

TEST_CASE("format_completion: not_connected error", "[device_control]")
{
    mcp_request_context_t ctx = make_ctx_2025();
    mcp_rpc_error_t error = {0};
    device_command_result_t result = {0};
    result.status = DEVICE_CMD_STATUS_NOT_CONNECTED;

    cJSON *out = mcp_device_control_format_completion("dev1", "feat1",
                                                      &result, &ctx, &error);
    TEST_ASSERT_NOT_NULL(out);

    cJSON *inner = parse_inner_result(out);
    cJSON *err = cJSON_GetObjectItemCaseSensitive(inner, "error");
    TEST_ASSERT_EQUAL_STRING("not_connected", err->valuestring);

    cJSON_Delete(inner);
    cJSON_Delete(out);
}

TEST_CASE("format_completion: type_mismatch error", "[device_control]")
{
    mcp_request_context_t ctx = make_ctx_2025();
    mcp_rpc_error_t error = {0};
    device_command_result_t result = {0};
    result.status = DEVICE_CMD_STATUS_TYPE_MISMATCH;

    cJSON *out = mcp_device_control_format_completion("dev1", "feat1",
                                                      &result, &ctx, &error);
    TEST_ASSERT_NOT_NULL(out);

    cJSON *inner = parse_inner_result(out);
    cJSON *err = cJSON_GetObjectItemCaseSensitive(inner, "error");
    TEST_ASSERT_EQUAL_STRING("type_mismatch", err->valuestring);

    cJSON_Delete(inner);
    cJSON_Delete(out);
}

TEST_CASE("format_completion: queue_full error", "[device_control]")
{
    mcp_request_context_t ctx = make_ctx_2025();
    mcp_rpc_error_t error = {0};
    device_command_result_t result = {0};
    result.status = DEVICE_CMD_STATUS_QUEUE_FULL;

    cJSON *out = mcp_device_control_format_completion("dev1", "feat1",
                                                      &result, &ctx, &error);
    TEST_ASSERT_NOT_NULL(out);

    cJSON *inner = parse_inner_result(out);
    cJSON *err = cJSON_GetObjectItemCaseSensitive(inner, "error");
    TEST_ASSERT_EQUAL_STRING("queue_full", err->valuestring);

    cJSON_Delete(inner);
    cJSON_Delete(out);
}
