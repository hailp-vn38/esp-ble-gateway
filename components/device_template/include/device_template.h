#ifndef DEVICE_TEMPLATE_H
#define DEVICE_TEMPLATE_H

#include <stdint.h>

/* ── Template definition ────────────────────────────────────────────── */

typedef struct {
    uint8_t feature_type;       /* gw_feature_type_t value */
    uint16_t schema_version;    /* feature_schema_version */
    const char *semantic_name;  /* e.g. "light" */
    uint8_t primary_property;   /* gw_feature_property_t value */
} device_template_t;

/* ── Public API ─────────────────────────────────────────────────────── */

/**
 * Lookup template by (feature_type, schema_version).
 * Returns NULL if no template matches (unknown/unsupported).
 */
const device_template_t *device_template_resolve(uint8_t feature_type,
                                                  uint16_t schema_version);

/**
 * Get semantic name from resolved template (NULL-safe).
 * Returns "" if tpl is NULL.
 */
const char *device_template_semantic_name(const device_template_t *tpl);

/**
 * Get primary property from resolved template (0 if NULL).
 */
uint8_t device_template_primary_property(const device_template_t *tpl);

#endif /* DEVICE_TEMPLATE_H */
