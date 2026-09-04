#include "device_template.h"

#include "cbor_codec.h"

/* ── Static template registry ───────────────────────────────────────── */

static const device_template_t s_templates[] = {
    {
        .feature_type    = GW_FEATURE_GENERIC_RELAY,
        .schema_version  = 1,
        .semantic_name   = "relay",
        .primary_property = GW_PROP_ON_OFF,
    },
    {
        .feature_type    = GW_FEATURE_ON_OFF_PLUGIN_UNIT,
        .schema_version  = 1,
        .semantic_name   = "outlet",
        .primary_property = GW_PROP_ON_OFF,
    },
    {
        .feature_type    = GW_FEATURE_ON_OFF_LIGHT,
        .schema_version  = 1,
        .semantic_name   = "light",
        .primary_property = GW_PROP_ON_OFF,
    },
    {
        .feature_type    = GW_FEATURE_DIMMABLE_LIGHT,
        .schema_version  = 1,
        .semantic_name   = "light",
        .primary_property = GW_PROP_ON_OFF,
    },
    {
        .feature_type    = GW_FEATURE_FAN,
        .schema_version  = 1,
        .semantic_name   = "fan",
        .primary_property = GW_PROP_ON_OFF,
    },
    {
        .feature_type    = GW_FEATURE_TEMPERATURE_SENSOR,
        .schema_version  = 1,
        .semantic_name   = "temperature",
        .primary_property = GW_PROP_TEMPERATURE,
    },
    {
        .feature_type    = GW_FEATURE_HUMIDITY_SENSOR,
        .schema_version  = 1,
        .semantic_name   = "humidity",
        .primary_property = GW_PROP_HUMIDITY,
    },
    {
        .feature_type    = GW_FEATURE_CONTACT_SENSOR,
        .schema_version  = 1,
        .semantic_name   = "contact",
        .primary_property = GW_PROP_CONTACT,
    },
};

static const size_t s_template_count =
    sizeof(s_templates) / sizeof(s_templates[0]);

/* ── Feature name mapping ───────────────────────────────────────────── */

static const struct {
    uint8_t type;
    const char *name;
} s_feature_names[] = {
    { GW_FEATURE_NONE,                  "none" },
    { GW_FEATURE_GENERIC_RELAY,         "relay" },
    { GW_FEATURE_ON_OFF_PLUGIN_UNIT,    "outlet" },
    { GW_FEATURE_ON_OFF_LIGHT,          "light" },
    { GW_FEATURE_DIMMABLE_LIGHT,        "light" },
    { GW_FEATURE_FAN,                   "fan" },
    { GW_FEATURE_TEMPERATURE_SENSOR,    "temperature" },
    { GW_FEATURE_HUMIDITY_SENSOR,       "humidity" },
    { GW_FEATURE_CONTACT_SENSOR,        "contact" },
};

/* ── Property name mapping ──────────────────────────────────────────── */

static const struct {
    uint8_t id;
    const char *name;
    device_template_value_type_t value_type;
} s_property_info[] = {
    { GW_PROP_NONE,             "none",              DEVICE_TEMPLATE_VALUE_NONE },
    { GW_PROP_ON_OFF,           "on_off",            DEVICE_TEMPLATE_VALUE_BOOL },
    { GW_PROP_LEVEL,            "level",             DEVICE_TEMPLATE_VALUE_INT },
    { GW_PROP_PERCENT_SETTING,  "percent_setting",   DEVICE_TEMPLATE_VALUE_INT },
    { GW_PROP_PERCENT_CURRENT,  "percent_current",   DEVICE_TEMPLATE_VALUE_INT },
    { GW_PROP_TEMPERATURE,      "temperature",       DEVICE_TEMPLATE_VALUE_INT },
    { GW_PROP_HUMIDITY,         "humidity",          DEVICE_TEMPLATE_VALUE_INT },
    { GW_PROP_CONTACT,          "contact",           DEVICE_TEMPLATE_VALUE_BOOL },
};

/* ── Public API ─────────────────────────────────────────────────────── */

const device_template_t *device_template_resolve(uint8_t feature_type,
                                                  uint16_t schema_version)
{
    for (size_t i = 0; i < s_template_count; i++) {
        if (s_templates[i].feature_type == feature_type &&
            s_templates[i].schema_version == schema_version) {
            return &s_templates[i];
        }
    }
    return NULL;
}

const char *device_template_semantic_name(const device_template_t *tpl)
{
    if (tpl == NULL) return "";
    return tpl->semantic_name;
}

uint8_t device_template_primary_property(const device_template_t *tpl)
{
    if (tpl == NULL) return 0;
    return tpl->primary_property;
}

const char *device_template_feature_name(uint8_t feature_type)
{
    for (size_t i = 0; i < sizeof(s_feature_names) / sizeof(s_feature_names[0]); i++) {
        if (s_feature_names[i].type == feature_type) {
            return s_feature_names[i].name;
        }
    }
    return "unknown";
}

const char *device_template_property_name(uint8_t property_id)
{
    for (size_t i = 0; i < sizeof(s_property_info) / sizeof(s_property_info[0]); i++) {
        if (s_property_info[i].id == property_id) {
            return s_property_info[i].name;
        }
    }
    return "unknown";
}

device_template_value_type_t device_template_property_value_type(
    uint8_t property_id)
{
    for (size_t i = 0; i < sizeof(s_property_info) / sizeof(s_property_info[0]); i++) {
        if (s_property_info[i].id == property_id) {
            return s_property_info[i].value_type;
        }
    }
    return DEVICE_TEMPLATE_VALUE_NONE;
}
