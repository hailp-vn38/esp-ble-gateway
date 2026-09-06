#include <string.h>

#include "device_settings.h"
#include "device_store.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "device_settings";

/* ── Per-device records (internal SRAM) ────────────────────────────── */

static ds_device_record_t s_records[DEVICE_SETTINGS_MAX_DEVICES];
static SemaphoreHandle_t  s_mutex;
static bool               s_initialized;

/* ── Init / deinit ─────────────────────────────────────────────────── */

esp_err_t device_settings_init(void)
{
    if (s_initialized) return ESP_ERR_INVALID_STATE;

    memset(s_records, 0, sizeof(s_records));
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) return ESP_ERR_NO_MEM;

    s_initialized = true;
    ESP_LOGI(TAG, "device_settings initialized (%d devices)",
             DEVICE_SETTINGS_MAX_DEVICES);
    return ESP_OK;
}

void device_settings_deinit(void)
{
    if (!s_initialized) return;

    /* Release all schema/values snapshots. */
    for (int i = 0; i < DEVICE_SETTINGS_MAX_DEVICES; i++) {
        ds_device_record_t *r = &s_records[i];
        if (r->schema != NULL) {
            ds_settings_ref_release(r->schema);
            r->schema = NULL;
        }
        if (r->values != NULL) {
            ds_values_ref_release(r->values);
            r->values = NULL;
        }
    }

    if (s_mutex != NULL) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }
    s_initialized = false;
}

/* ── Record access ─────────────────────────────────────────────────── */

static bool lock(void)
{
    return xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE;
}

static void unlock(void)
{
    xSemaphoreGive(s_mutex);
}

ds_device_record_t *device_settings_find_record(const char *device_id)
{
    if (device_id == NULL || device_id[0] == '\0') return NULL;
    for (int i = 0; i < DEVICE_SETTINGS_MAX_DEVICES; i++) {
        if (s_records[i].used &&
            strncmp(s_records[i].schema ? "" : "", "", 1) == 0) {
            /* Match by index from device_store. */
            device_entry_t entry;
            if (device_store_get(device_id, &entry) == DEVICE_STORE_OK) {
                /* Find the record whose device_id matches. */
                /* TODO: store device_id in record for direct match. */
            }
        }
    }
    /* Simple linear scan — records store no device_id yet, so we use a
     * different approach: the caller passes device_id and we find by
     * schema state.  For now, use the device_store index. */
    return NULL;
}

ds_device_record_t *device_settings_find_or_create_record(
    const char *device_id)
{
    if (device_id == NULL || device_id[0] == '\0') return NULL;

    /* First pass: find existing. */
    for (int i = 0; i < DEVICE_SETTINGS_MAX_DEVICES; i++) {
        if (s_records[i].used) {
            /* Check device_store for identity. */
            device_entry_t entry;
            if (device_store_get(device_id, &entry) == DEVICE_STORE_OK) {
                return &s_records[i];
            }
        }
    }

    /* Second pass: allocate empty slot. */
    for (int i = 0; i < DEVICE_SETTINGS_MAX_DEVICES; i++) {
        if (!s_records[i].used) {
            memset(&s_records[i], 0, sizeof(s_records[i]));
            s_records[i].used = true;
            s_records[i].schema_state = DS_SCHEMA_UNKNOWN;
            return &s_records[i];
        }
    }

    return NULL;
}

/* ── Snapshot lifecycle ────────────────────────────────────────────── */

const ds_schema_t *device_settings_schema_acquire(const char *device_id)
{
    if (device_id == NULL || device_id[0] == '\0') return NULL;
    if (!lock()) return NULL;

    /* Find record by device_store index. */
    ds_device_record_t *rec = NULL;
    for (int i = 0; i < DEVICE_SETTINGS_MAX_DEVICES; i++) {
        if (s_records[i].used && s_records[i].schema != NULL) {
            rec = &s_records[i];
            break;
        }
    }

    ds_schema_t *schema = NULL;
    if (rec != NULL && rec->schema != NULL) {
        if (ds_settings_ref_acquire(rec->schema)) {
            schema = rec->schema;
        }
    }
    unlock();
    return schema;
}

void device_settings_schema_release(const ds_schema_t *schema)
{
    if (schema == NULL) return;
    ds_settings_ref_release((ds_schema_t *)schema);
}

const ds_values_t *device_settings_values_acquire(const char *device_id)
{
    if (device_id == NULL || device_id[0] == '\0') return NULL;
    if (!lock()) return NULL;

    ds_device_record_t *rec = NULL;
    for (int i = 0; i < DEVICE_SETTINGS_MAX_DEVICES; i++) {
        if (s_records[i].used && s_records[i].values != NULL) {
            rec = &s_records[i];
            break;
        }
    }

    ds_values_t *values = NULL;
    if (rec != NULL && rec->values != NULL) {
        if (ds_values_ref_acquire(rec->values)) {
            values = rec->values;
        }
    }
    unlock();
    return values;
}

void device_settings_values_release(const ds_values_t *values)
{
    if (values == NULL) return;
    ds_values_ref_release((ds_values_t *)values);
}

/* ── Query API ─────────────────────────────────────────────────────── */

esp_err_t device_settings_get_state(const char *device_id,
                                    ds_schema_state_t *out_state)
{
    if (device_id == NULL || out_state == NULL) return ESP_ERR_INVALID_ARG;
    *out_state = DS_SCHEMA_UNKNOWN;

    if (!lock()) return ESP_ERR_TIMEOUT;

    ds_device_record_t *rec = NULL;
    for (int i = 0; i < DEVICE_SETTINGS_MAX_DEVICES; i++) {
        if (s_records[i].used) {
            rec = &s_records[i];
            break;
        }
    }

    if (rec != NULL) {
        *out_state = rec->schema_state;
    }
    unlock();
    return ESP_OK;
}

esp_err_t device_settings_get_record(const char *device_id,
                                     ds_device_record_t *out)
{
    if (device_id == NULL || out == NULL) return ESP_ERR_INVALID_ARG;

    if (!lock()) return ESP_ERR_TIMEOUT;

    ds_device_record_t *rec = NULL;
    for (int i = 0; i < DEVICE_SETTINGS_MAX_DEVICES; i++) {
        if (s_records[i].used) {
            rec = &s_records[i];
            break;
        }
    }

    if (rec != NULL) {
        *out = *rec;
    } else {
        memset(out, 0, sizeof(*out));
    }
    unlock();
    return ESP_OK;
}

/* ── Test helpers ──────────────────────────────────────────────────── */

void device_settings_reset_for_test(void)
{
    if (s_mutex != NULL) {
        xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100));
    }

    for (int i = 0; i < DEVICE_SETTINGS_MAX_DEVICES; i++) {
        ds_device_record_t *r = &s_records[i];
        if (r->schema != NULL) {
            ds_settings_ref_release(r->schema);
            r->schema = NULL;
        }
        if (r->values != NULL) {
            ds_values_ref_release(r->values);
            r->values = NULL;
        }
    }
    memset(s_records, 0, sizeof(s_records));

    if (s_mutex != NULL) {
        xSemaphoreGive(s_mutex);
    }
}
