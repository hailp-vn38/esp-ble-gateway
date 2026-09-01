#include "device_template.h"

#include "cbor_codec.h"

static const char *TAG = "device_template";

/* ── Static template registry ───────────────────────────────────────── */

static const device_template_t s_templates[] = {
    {
        .feature_type    = GW_FEATURE_ON_OFF_LIGHT,
        .schema_version  = 1,
        .semantic_name   = "light",
        .primary_property = GW_PROP_ON_OFF,
    },
    /* Future templates added here */
};

static const size_t s_template_count =
    sizeof(s_templates) / sizeof(s_templates[0]);

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
