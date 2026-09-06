#include <string.h>

#include "device_settings.h"
#include "esp_log.h"
#include "memory_policy.h"

static const char *TAG = "ds_memory";

/* ── PSRAM-required allocator ──────────────────────────────────────── */

void *ds_settings_alloc(size_t size)
{
    if (size == 0) return NULL;
    void *ptr = gw_mem_alloc(size, GW_MEM_EXTERNAL_REQUIRED);
    if (ptr == NULL) {
        ESP_LOGE(TAG, "PSRAM alloc failed: %u bytes", (unsigned)size);
        DS_DIAG_INC(psram_alloc_fail);
    } else {
        DS_DIAG_INC(psram_alloc_success);
    }
    return ptr;
}

void ds_settings_free(void *ptr)
{
    if (ptr != NULL) {
        gw_mem_free(ptr);
    }
}

/* ── String pool ───────────────────────────────────────────────────── */

const char *ds_string_pool_get(const ds_string_pool_t *pool, uint16_t off)
{
    if (pool == NULL || pool->pool == NULL || off >= pool->total_size) {
        return "";
    }
    return pool->pool + off;
}

esp_err_t ds_string_pool_add(ds_string_pool_t *pool, const char *str,
                             uint16_t *out_off)
{
    if (pool == NULL || str == NULL || out_off == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t len = strlen(str);
    size_t aligned = (len + 1 + 3) & ~(size_t)3;  /* 4-byte align */

    if (pool->total_size + aligned > pool->capacity) {
        ESP_LOGE(TAG, "string pool overflow: need %u, have %u",
                 (unsigned)(pool->total_size + aligned),
                 (unsigned)(pool->capacity));
        return ESP_ERR_NO_MEM;
    }

    *out_off = pool->total_size;
    memcpy(pool->pool + pool->total_size, str, len + 1);
    pool->total_size += (uint16_t)aligned;
    return ESP_OK;
}

/* ── Schema refcount ─────────────────────────────────────────────────
 * Schema snapshots are refcounted.  The refcount field is stored in
 * the first 4 bytes of the allocation header (before the ds_schema_t).
 * This keeps the schema struct itself compact and avoids adding a
 * refcount field to the public type. */

typedef struct {
    volatile int32_t refcount;
} ds_ref_header_t;

static ds_ref_header_t *ref_header_from_ptr(void *ptr)
{
    return (ds_ref_header_t *)((uint8_t *)ptr - sizeof(ds_ref_header_t));
}

static void *ref_header_to_ptr(ds_ref_header_t *hdr)
{
    return (uint8_t *)hdr + sizeof(ds_ref_header_t);
}

static void *alloc_with_refcount(size_t user_size)
{
    size_t total = sizeof(ds_ref_header_t) + user_size;
    void *raw = ds_settings_alloc(total);
    if (raw == NULL) return NULL;

    ds_ref_header_t *hdr = (ds_ref_header_t *)raw;
    hdr->refcount = 1;
    return ref_header_to_ptr(hdr);
}

static void free_with_refcount(void *ptr)
{
    if (ptr == NULL) return;
    ds_ref_header_t *hdr = ref_header_from_ptr(ptr);
    ds_settings_free(hdr);
}

bool ds_settings_ref_acquire(ds_schema_t *schema)
{
    if (schema == NULL) return false;
    ds_ref_header_t *hdr = ref_header_from_ptr(schema);
    int32_t old = hdr->refcount;
    if (old <= 0) return false;
    hdr->refcount = old + 1;
    return true;
}

void ds_settings_ref_release(ds_schema_t *schema)
{
    if (schema == NULL) return;
    ds_ref_header_t *hdr = ref_header_from_ptr(schema);
    int32_t old = hdr->refcount;
    if (old <= 1) {
        /* Last reference — free string pool, then the schema itself. */
        if (schema->strings.pool != NULL) {
            ds_settings_free(schema->strings.pool);
        }
        hdr->refcount = 0;
        free_with_refcount(schema);
    } else {
        hdr->refcount = old - 1;
    }
}

bool ds_values_ref_acquire(ds_values_t *values)
{
    if (values == NULL) return false;
    ds_ref_header_t *hdr = ref_header_from_ptr(values);
    int32_t old = hdr->refcount;
    if (old <= 0) return false;
    hdr->refcount = old + 1;
    return true;
}

void ds_values_ref_release(ds_values_t *values)
{
    if (values == NULL) return;
    ds_ref_header_t *hdr = ref_header_from_ptr(values);
    int32_t old = hdr->refcount;
    if (old <= 1) {
        if (values->string_pool.pool != NULL) {
            ds_settings_free(values->string_pool.pool);
        }
        hdr->refcount = 0;
        free_with_refcount(values);
    } else {
        hdr->refcount = old - 1;
    }
}

/* ── Schema builder ────────────────────────────────────────────────── */

esp_err_t ds_schema_builder_init(ds_schema_builder_t *builder)
{
    if (builder == NULL) return ESP_ERR_INVALID_ARG;
    memset(builder, 0, sizeof(*builder));

    builder->strings.pool = ds_settings_alloc(DEVICE_SETTINGS_MAX_STRING_POOL);
    if (builder->strings.pool == NULL) return ESP_ERR_NO_MEM;
    builder->strings.capacity = DEVICE_SETTINGS_MAX_STRING_POOL;
    builder->strings.total_size = 0;
    return ESP_OK;
}

void ds_schema_builder_reset(ds_schema_builder_t *builder)
{
    if (builder == NULL) return;
    if (builder->strings.pool != NULL) {
        ds_settings_free(builder->strings.pool);
    }
    memset(builder, 0, sizeof(*builder));
}

esp_err_t ds_schema_builder_add_string(ds_schema_builder_t *builder,
                                       const char *str, uint16_t *out_off)
{
    if (builder == NULL || str == NULL || out_off == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return ds_string_pool_add(&builder->strings, str, out_off);
}

esp_err_t ds_schema_builder_add_setting(ds_schema_builder_t *builder,
                                        const ds_setting_desc_t *desc)
{
    if (builder == NULL || desc == NULL) return ESP_ERR_INVALID_ARG;
    if (builder->setting_count >= DEVICE_SETTINGS_MAX_SETTINGS) {
        return ESP_ERR_NO_MEM;
    }
    builder->descriptors[builder->setting_count] = *desc;
    builder->setting_count++;
    return ESP_OK;
}

esp_err_t ds_schema_builder_add_enum_option(ds_schema_builder_t *builder,
                                            int32_t value,
                                            const char *label,
                                            uint16_t *out_index)
{
    if (builder == NULL || label == NULL || out_index == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint16_t max_opts = DEVICE_SETTINGS_MAX_SETTINGS *
                        DEVICE_SETTINGS_MAX_ENUM_OPTS;
    if (builder->enum_option_count >= max_opts) {
        return ESP_ERR_NO_MEM;
    }

    uint16_t label_off = 0;
    esp_err_t err = ds_string_pool_add(&builder->strings, label, &label_off);
    if (err != ESP_OK) return err;

    *out_index = builder->enum_option_count;
    builder->enum_options[builder->enum_option_count].value = value;
    builder->enum_options[builder->enum_option_count].label_off = label_off;
    builder->enum_option_count++;
    return ESP_OK;
}

ds_schema_t *ds_schema_builder_commit(ds_schema_builder_t *builder)
{
    if (builder == NULL) return NULL;
    if (builder->setting_count == 0) return NULL;

    /* Allocate the schema with refcount header. */
    ds_schema_t *schema = alloc_with_refcount(sizeof(ds_schema_t));
    if (schema == NULL) return NULL;

    memset(schema, 0, sizeof(*schema));
    schema->schema_revision = builder->schema_revision;
    schema->setting_count = builder->setting_count;
    memcpy(schema->descriptors, builder->descriptors,
           builder->setting_count * sizeof(ds_setting_desc_t));
    schema->enum_option_count = builder->enum_option_count;
    memcpy(schema->enum_option_pool, builder->enum_options,
           builder->enum_option_count * sizeof(ds_enum_option_t));

    /* Transfer string pool ownership (builder must not free it). */
    schema->strings = builder->strings;
    builder->strings.pool = NULL;
    builder->strings.total_size = 0;
    builder->strings.capacity = 0;

    return schema;
}

/* ── Values builder ────────────────────────────────────────────────── */

esp_err_t ds_values_builder_init(ds_values_builder_t *builder)
{
    if (builder == NULL) return ESP_ERR_INVALID_ARG;
    memset(builder, 0, sizeof(*builder));

    builder->string_pool.pool =
        ds_settings_alloc(DEVICE_SETTINGS_MAX_STRING_POOL);
    if (builder->string_pool.pool == NULL) return ESP_ERR_NO_MEM;
    builder->string_pool.capacity = DEVICE_SETTINGS_MAX_STRING_POOL;
    builder->string_pool.total_size = 0;
    return ESP_OK;
}

void ds_values_builder_reset(ds_values_builder_t *builder)
{
    if (builder == NULL) return;
    if (builder->string_pool.pool != NULL) {
        ds_settings_free(builder->string_pool.pool);
    }
    memset(builder, 0, sizeof(*builder));
}

esp_err_t ds_values_builder_add(ds_values_builder_t *builder,
                                const ds_value_entry_t *entry)
{
    if (builder == NULL || entry == NULL) return ESP_ERR_INVALID_ARG;
    if (builder->value_count >= DEVICE_SETTINGS_MAX_SETTINGS) {
        return ESP_ERR_NO_MEM;
    }
    builder->entries[builder->value_count] = *entry;
    builder->value_count++;
    return ESP_OK;
}

ds_values_t *ds_values_builder_commit(ds_values_builder_t *builder)
{
    if (builder == NULL) return NULL;

    ds_values_t *values = alloc_with_refcount(sizeof(ds_values_t));
    if (values == NULL) return NULL;

    memset(values, 0, sizeof(*values));
    values->config_revision = builder->config_revision;
    values->value_count = builder->value_count;
    memcpy(values->values, builder->entries,
           builder->value_count * sizeof(ds_value_entry_t));

    values->string_pool = builder->string_pool;
    builder->string_pool.pool = NULL;
    builder->string_pool.total_size = 0;
    builder->string_pool.capacity = 0;

    return values;
}
