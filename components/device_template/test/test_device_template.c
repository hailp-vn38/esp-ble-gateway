#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "cbor_codec.h"
#include "device_template.h"

/* ── Tests ──────────────────────────────────────────────────────────── */

TEST_CASE("ON_OFF_LIGHT + schema 1 resolves to light template",
          "[device_template]")
{
    const device_template_t *tpl = device_template_resolve(
        GW_FEATURE_ON_OFF_LIGHT, 1);
    TEST_ASSERT_NOT_NULL(tpl);
    TEST_ASSERT_EQUAL_STRING("light", tpl->semantic_name);
    TEST_ASSERT_EQUAL_UINT8(GW_PROP_ON_OFF, tpl->primary_property);
    TEST_ASSERT_EQUAL_UINT8(GW_FEATURE_ON_OFF_LIGHT, tpl->feature_type);
    TEST_ASSERT_EQUAL_UINT16(1, tpl->schema_version);
}

TEST_CASE("template does not hardcode tool name", "[device_template]")
{
    /* The template is independent of the tool name (set_led, power, etc.).
     * Write commands use the feature's writable_tool_index from the schema. */
    const device_template_t *tpl = device_template_resolve(
        GW_FEATURE_ON_OFF_LIGHT, 1);
    TEST_ASSERT_NOT_NULL(tpl);
    /* Template only defines semantic_name and primary_property */
    TEST_ASSERT_EQUAL_STRING("light", tpl->semantic_name);
    TEST_ASSERT_EQUAL_UINT8(GW_PROP_ON_OFF, tpl->primary_property);
}

TEST_CASE("different tool names use same template", "[device_template]")
{
    /* Whether the device calls its tool "set_led" or "power",
     * the template resolves the same way. */
    const device_template_t *tpl1 = device_template_resolve(
        GW_FEATURE_ON_OFF_LIGHT, 1);
    const device_template_t *tpl2 = device_template_resolve(
        GW_FEATURE_ON_OFF_LIGHT, 1);
    TEST_ASSERT_NOT_NULL(tpl1);
    TEST_ASSERT_NOT_NULL(tpl2);
    TEST_ASSERT_EQUAL_PTR(tpl1, tpl2);
}

TEST_CASE("unknown schema version returns NULL", "[device_template]")
{
    const device_template_t *tpl = device_template_resolve(
        GW_FEATURE_ON_OFF_LIGHT, 999);
    TEST_ASSERT_NULL(tpl);
}

TEST_CASE("unknown feature type returns NULL", "[device_template]")
{
    const device_template_t *tpl = device_template_resolve(255, 1);
    TEST_ASSERT_NULL(tpl);
}

TEST_CASE("semantic_name helper is NULL-safe", "[device_template]")
{
    TEST_ASSERT_EQUAL_STRING("", device_template_semantic_name(NULL));
}

TEST_CASE("primary_property helper is NULL-safe", "[device_template]")
{
    TEST_ASSERT_EQUAL_UINT8(0, device_template_primary_property(NULL));
}
