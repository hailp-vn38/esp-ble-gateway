#include <string.h>

#include "cJSON.h"
#include "cbor_codec.h"
#include "device_schema.h"
#include "device_store.h"
#include "device_template.h"
#include "../mcp_semantic_control.h"
#include "mcp_tool_exposure.h"
#include "unity.h"

/* ── Helpers ────────────────────────────────────────────────────────── */

static device_schema_snapshot_t make_snapshot(const device_schema_feature_t *features,
                                              size_t feature_count,
                                              const device_schema_tool_t *tools,
                                              size_t tool_count)
{
    device_schema_snapshot_t snap = {0};
    snap.has_committed = true;
    snap.feature_count = feature_count;
    snap.tool_count = tool_count;
    if (features != NULL)
        memcpy(snap.features, features, feature_count * sizeof(*features));
    if (tools != NULL)
        memcpy(snap.tools, tools, tool_count * sizeof(*tools));
    return snap;
}

/* ── Device resolution tests ────────────────────────────────────────── */

TEST_CASE("sem_resolve: exact device_id match", "[mcp_semantic]")
{
    device_store_add("dev-aaa", "Living Room");

    cJSON *arg = cJSON_CreateString("dev-aaa");
    char out[32] = {0};
    mcp_sem_resolve_status_t st = mcp_sem_resolve_device(arg, out, sizeof(out));

    TEST_ASSERT_EQUAL(MCP_SEM_OK, st);
    TEST_ASSERT_EQUAL_STRING("dev-aaa", out);
    cJSON_Delete(arg);
}

TEST_CASE("sem_resolve: unique name fallback", "[mcp_semantic]")
{
    device_store_add("dev-bbb", "Kitchen");

    cJSON *arg = cJSON_CreateString("Kitchen");
    char out[32] = {0};
    mcp_sem_resolve_status_t st = mcp_sem_resolve_device(arg, out, sizeof(out));

    TEST_ASSERT_EQUAL(MCP_SEM_OK, st);
    TEST_ASSERT_EQUAL_STRING("dev-bbb", out);
    cJSON_Delete(arg);
}

TEST_CASE("sem_resolve: duplicate name is ambiguous", "[mcp_semantic]")
{
    device_store_add("dev-ccc", "Bedroom");
    device_store_add("dev-ddd", "Bedroom");

    cJSON *arg = cJSON_CreateString("Bedroom");
    char out[32] = {0};
    mcp_sem_resolve_status_t st = mcp_sem_resolve_device(arg, out, sizeof(out));

    TEST_ASSERT_EQUAL(MCP_SEM_AMBIGUOUS, st);
    cJSON_Delete(arg);
}

TEST_CASE("sem_resolve: not found returns NOT_FOUND", "[mcp_semantic]")
{
    cJSON *arg = cJSON_CreateString("nonexistent");
    char out[32] = {0};
    mcp_sem_resolve_status_t st = mcp_sem_resolve_device(arg, out, sizeof(out));

    TEST_ASSERT_EQUAL(MCP_SEM_NOT_FOUND, st);
    cJSON_Delete(arg);
}

TEST_CASE("sem_resolve: empty string is INVALID", "[mcp_semantic]")
{
    cJSON *arg = cJSON_CreateString("");
    char out[32] = {0};
    mcp_sem_resolve_status_t st = mcp_sem_resolve_device(arg, out, sizeof(out));

    TEST_ASSERT_EQUAL(MCP_SEM_INVALID, st);
    cJSON_Delete(arg);
}

TEST_CASE("sem_resolve: non-string cJSON is INVALID", "[mcp_semantic]")
{
    cJSON *arg = cJSON_CreateNumber(42);
    char out[32] = {0};
    mcp_sem_resolve_status_t st = mcp_sem_resolve_device(arg, out, sizeof(out));

    TEST_ASSERT_EQUAL(MCP_SEM_INVALID, st);
    cJSON_Delete(arg);
}

/* ── Feature resolution tests ───────────────────────────────────────── */

TEST_CASE("sem_resolve_feature: exact feature_id match", "[mcp_semantic]")
{
    device_schema_feature_t feat = {0};
    strlcpy(feat.feature_id, "feat-onoff", sizeof(feat.feature_id));
    feat.feature_type = GW_FEATURE_ON_OFF_LIGHT;
    feat.feature_schema_version = 1;
    feat.property_id = GW_PROP_ON_OFF;

    device_schema_snapshot_t snap = make_snapshot(&feat, 1, NULL, 0);
    cJSON *arg = cJSON_CreateString("feat-onoff");
    device_schema_feature_t out = {0};
    mcp_sem_resolve_status_t st = mcp_sem_resolve_feature(&snap, arg, &out);

    TEST_ASSERT_EQUAL(MCP_SEM_OK, st);
    TEST_ASSERT_EQUAL_STRING("feat-onoff", out.feature_id);
    cJSON_Delete(arg);
}

TEST_CASE("sem_resolve_feature: unique semantic name fallback", "[mcp_semantic]")
{
    device_schema_feature_t feat = {0};
    strlcpy(feat.feature_id, "feat-light1", sizeof(feat.feature_id));
    feat.feature_type = GW_FEATURE_ON_OFF_LIGHT;
    feat.feature_schema_version = 1;
    feat.property_id = GW_PROP_ON_OFF;

    device_schema_snapshot_t snap = make_snapshot(&feat, 1, NULL, 0);
    cJSON *arg = cJSON_CreateString("light");
    device_schema_feature_t out = {0};
    mcp_sem_resolve_status_t st = mcp_sem_resolve_feature(&snap, arg, &out);

    TEST_ASSERT_EQUAL(MCP_SEM_OK, st);
    TEST_ASSERT_EQUAL_STRING("feat-light1", out.feature_id);
    cJSON_Delete(arg);
}

TEST_CASE("sem_resolve_feature: duplicate semantic is ambiguous", "[mcp_semantic]")
{
    device_schema_feature_t feats[2] = {0};
    strlcpy(feats[0].feature_id, "feat-a", sizeof(feats[0].feature_id));
    feats[0].feature_type = GW_FEATURE_ON_OFF_LIGHT;
    feats[0].feature_schema_version = 1;
    feats[0].property_id = GW_PROP_ON_OFF;

    strlcpy(feats[1].feature_id, "feat-b", sizeof(feats[1].feature_id));
    feats[1].feature_type = GW_FEATURE_DIMMABLE_LIGHT;
    feats[1].feature_schema_version = 1;
    feats[1].property_id = GW_PROP_ON_OFF;

    device_schema_snapshot_t snap = make_snapshot(feats, 2, NULL, 0);
    cJSON *arg = cJSON_CreateString("light");
    device_schema_feature_t out = {0};
    mcp_sem_resolve_status_t st = mcp_sem_resolve_feature(&snap, arg, &out);

    TEST_ASSERT_EQUAL(MCP_SEM_AMBIGUOUS, st);
    cJSON_Delete(arg);
}

TEST_CASE("sem_resolve_feature: not found", "[mcp_semantic]")
{
    device_schema_snapshot_t snap = make_snapshot(NULL, 0, NULL, 0);
    cJSON *arg = cJSON_CreateString("no-such-feature");
    device_schema_feature_t out = {0};
    mcp_sem_resolve_status_t st = mcp_sem_resolve_feature(&snap, arg, &out);

    TEST_ASSERT_EQUAL(MCP_SEM_NOT_FOUND, st);
    cJSON_Delete(arg);
}

TEST_CASE("sem_resolve_feature: empty string is INVALID", "[mcp_semantic]")
{
    device_schema_snapshot_t snap = make_snapshot(NULL, 0, NULL, 0);
    cJSON *arg = cJSON_CreateString("");
    device_schema_feature_t out = {0};
    mcp_sem_resolve_status_t st = mcp_sem_resolve_feature(&snap, arg, &out);

    TEST_ASSERT_EQUAL(MCP_SEM_INVALID, st);
    cJSON_Delete(arg);
}

TEST_CASE("sem_resolve_feature: non-string cJSON is INVALID", "[mcp_semantic]")
{
    device_schema_snapshot_t snap = make_snapshot(NULL, 0, NULL, 0);
    cJSON *arg = cJSON_CreateBool(true);
    device_schema_feature_t out = {0};
    mcp_sem_resolve_status_t st = mcp_sem_resolve_feature(&snap, arg, &out);

    TEST_ASSERT_EQUAL(MCP_SEM_INVALID, st);
    cJSON_Delete(arg);
}

/* ── Feature serialization tests ────────────────────────────────────── */

TEST_CASE("sem_serialize_feature: BOOL feature", "[mcp_semantic]")
{
    device_schema_feature_t feat = {0};
    strlcpy(feat.feature_id, "feat-switch", sizeof(feat.feature_id));
    feat.feature_type = GW_FEATURE_ON_OFF_LIGHT;
    feat.feature_schema_version = 1;
    feat.property_id = GW_PROP_ON_OFF;
    feat.writable_tool_index = -1; /* read-only */

    device_schema_snapshot_t snap = make_snapshot(&feat, 1, NULL, 0);
    cJSON *array = cJSON_CreateArray();
    TEST_ASSERT_NOT_NULL(array);

    bool ok = mcp_sem_serialize_feature(array, &snap, &feat);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(array));

    cJSON *item = cJSON_GetArrayItem(array, 0);
    TEST_ASSERT_EQUAL_STRING("feat-switch",
                             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "feature_id")));
    TEST_ASSERT_EQUAL_STRING("light",
                             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "semantic_name")));
    TEST_ASSERT_EQUAL_STRING("on_off",
                             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "property")));
    TEST_ASSERT_EQUAL_STRING("bool",
                             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "value_type")));
    TEST_ASSERT_FALSE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "writable")));
    /* BOOL features should NOT have min/max/step */
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(item, "minimum"));
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(item, "maximum"));
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(item, "step"));

    cJSON_Delete(array);
}

TEST_CASE("sem_serialize_feature: writable INT feature with range", "[mcp_semantic]")
{
    device_schema_tool_t tool = {0};
    strlcpy(tool.command, "set_value", sizeof(tool.command));
    tool.min_value = 0;
    tool.max_value = 100;
    tool.step = 5;

    device_schema_feature_t feat = {0};
    strlcpy(feat.feature_id, "feat-dimmer", sizeof(feat.feature_id));
    feat.feature_type = GW_FEATURE_DIMMABLE_LIGHT;
    feat.feature_schema_version = 1;
    feat.property_id = GW_PROP_LEVEL;
    feat.writable_tool_index = 0; /* points to tool[0] */

    device_schema_snapshot_t snap = make_snapshot(&feat, 1, &tool, 1);
    cJSON *array = cJSON_CreateArray();
    TEST_ASSERT_NOT_NULL(array);

    bool ok = mcp_sem_serialize_feature(array, &snap, &feat);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(array));

    cJSON *item = cJSON_GetArrayItem(array, 0);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "writable")));
    cJSON *min = cJSON_GetObjectItemCaseSensitive(item, "minimum");
    TEST_ASSERT_NOT_NULL(min);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, min->valuedouble);
    cJSON *max = cJSON_GetObjectItemCaseSensitive(item, "maximum");
    TEST_ASSERT_NOT_NULL(max);
    TEST_ASSERT_EQUAL_DOUBLE(100.0, max->valuedouble);
    cJSON *step = cJSON_GetObjectItemCaseSensitive(item, "step");
    TEST_ASSERT_NOT_NULL(step);
    TEST_ASSERT_EQUAL_DOUBLE(5.0, step->valuedouble);

    cJSON_Delete(array);
}

/* ── Hint serialization tests ───────────────────────────────────────── */

TEST_CASE("sem_serialize_hints: BOOL hint", "[mcp_semantic]")
{
    mcp_control_hint_t hint = {0};
    strlcpy(hint.feature_id, "sw-1", sizeof(hint.feature_id));
    strlcpy(hint.semantic_name, "switch", sizeof(hint.semantic_name));
    strlcpy(hint.property_name, "on_off", sizeof(hint.property_name));
    hint.value_type = DEVICE_TEMPLATE_VALUE_BOOL;

    cJSON *array = cJSON_CreateArray();
    TEST_ASSERT_NOT_NULL(array);

    esp_err_t err = mcp_semantic_control_serialize_hints(array, &hint, 1);
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(array));

    cJSON *item = cJSON_GetArrayItem(array, 0);
    TEST_ASSERT_EQUAL_STRING("sw-1",
                             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "feature_id")));
    TEST_ASSERT_EQUAL_STRING("bool",
                             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "value_type")));
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "writable")));
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(item, "minimum"));

    cJSON_Delete(array);
}

TEST_CASE("sem_serialize_hints: INT hint with range", "[mcp_semantic]")
{
    mcp_control_hint_t hint = {0};
    strlcpy(hint.feature_id, "dim-1", sizeof(hint.feature_id));
    strlcpy(hint.semantic_name, "brightness", sizeof(hint.semantic_name));
    strlcpy(hint.property_name, "level", sizeof(hint.property_name));
    hint.value_type = DEVICE_TEMPLATE_VALUE_INT;
    hint.has_min = true;
    hint.min_value = 10;
    hint.has_max = true;
    hint.max_value = 90;
    hint.has_step = true;
    hint.step = 5;

    cJSON *array = cJSON_CreateArray();
    TEST_ASSERT_NOT_NULL(array);

    esp_err_t err = mcp_semantic_control_serialize_hints(array, &hint, 1);
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(array));

    cJSON *item = cJSON_GetArrayItem(array, 0);
    TEST_ASSERT_EQUAL_STRING("int",
                             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "value_type")));
    TEST_ASSERT_EQUAL_DOUBLE(10.0,
                             cJSON_GetObjectItemCaseSensitive(item, "minimum")->valuedouble);
    TEST_ASSERT_EQUAL_DOUBLE(90.0,
                             cJSON_GetObjectItemCaseSensitive(item, "maximum")->valuedouble);
    TEST_ASSERT_EQUAL_DOUBLE(5.0,
                             cJSON_GetObjectItemCaseSensitive(item, "step")->valuedouble);

    cJSON_Delete(array);
}

TEST_CASE("sem_serialize_hints: empty array is ok", "[mcp_semantic]")
{
    cJSON *array = cJSON_CreateArray();
    TEST_ASSERT_NOT_NULL(array);

    esp_err_t err = mcp_semantic_control_serialize_hints(array, NULL, 0);
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_EQUAL_INT(0, cJSON_GetArraySize(array));

    cJSON_Delete(array);
}

TEST_CASE("sem_serialize_hints: NULL array returns INVALID_ARG", "[mcp_semantic]")
{
    mcp_control_hint_t hint = {0};
    esp_err_t err = mcp_semantic_control_serialize_hints(NULL, &hint, 1);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, err);
}
