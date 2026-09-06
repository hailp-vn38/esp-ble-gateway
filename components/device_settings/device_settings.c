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

    for (int i = 0; i < DEVICE_SETTINGS_MAX_DEVICES; i++) {
        ds_device_record_t *r = &s_records[i];
        if (r->staging_schema != NULL) {
            ds_settings_ref_release(r->staging_schema);
            r->staging_schema = NULL;
        }
        if (r->staging_values != NULL) {
            ds_values_ref_release(r->staging_values);
            r->staging_values = NULL;
        }
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
            strcmp(s_records[i].device_id, device_id) == 0) {
            return &s_records[i];
        }
    }
    return NULL;
}

ds_device_record_t *device_settings_find_or_create_record(
    const char *device_id)
{
    if (device_id == NULL || device_id[0] == '\0') return NULL;

    /* First pass: find existing. */
    for (int i = 0; i < DEVICE_SETTINGS_MAX_DEVICES; i++) {
        if (s_records[i].used &&
            strcmp(s_records[i].device_id, device_id) == 0) {
            return &s_records[i];
        }
    }

    /* Second pass: allocate empty slot. */
    for (int i = 0; i < DEVICE_SETTINGS_MAX_DEVICES; i++) {
        if (!s_records[i].used) {
            memset(&s_records[i], 0, sizeof(s_records[i]));
            s_records[i].used = true;
            s_records[i].schema_state = DS_SCHEMA_UNKNOWN;
            strlcpy(s_records[i].device_id, device_id,
                    sizeof(s_records[i].device_id));
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

    ds_device_record_t *rec = device_settings_find_record(device_id);

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

    ds_device_record_t *rec = device_settings_find_record(device_id);

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

/* ── Disconnect handler ────────────────────────────────────────────── */

void device_settings_on_disconnect(const char *device_id)
{
    if (device_id == NULL || device_id[0] == '\0') return;

    if (!lock()) return;

    ds_device_record_t *rec = device_settings_find_record(device_id);
    if (rec != NULL) {
        /* Free staging snapshots — they were being built but not committed. */
        if (rec->staging_schema != NULL) {
            ds_settings_ref_release(rec->staging_schema);
            rec->staging_schema = NULL;
        }
        if (rec->staging_values != NULL) {
            ds_values_ref_release(rec->staging_values);
            rec->staging_values = NULL;
        }
        /* Reset stream state. */
        rec->schema_stream_active = false;
        rec->values_stream_active = false;
        rec->staging_expected_count = 0;
        rec->staging_received_count = 0;
        rec->staging_snapshot_id = 0;

        /* Committed snapshots are preserved — they may be stale but remain
         * valid for the UI until rediscovery on reconnect.  Schema state
         * remains READY (not UNKNOWN) so the gateway knows this device
         * previously had settings support. */
        ESP_LOGI(TAG, "[%s] disconnected: staging freed, committed preserved",
                 device_id);
    }

    unlock();
}

/* ── Query API ─────────────────────────────────────────────────────── */

esp_err_t device_settings_get_state(const char *device_id,
                                    ds_schema_state_t *out_state)
{
    if (device_id == NULL || out_state == NULL) return ESP_ERR_INVALID_ARG;
    *out_state = DS_SCHEMA_UNKNOWN;

    if (!lock()) return ESP_ERR_TIMEOUT;

    ds_device_record_t *rec = device_settings_find_record(device_id);
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

    ds_device_record_t *rec = device_settings_find_record(device_id);
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
        if (r->staging_schema != NULL) {
            ds_settings_ref_release(r->staging_schema);
            r->staging_schema = NULL;
        }
        if (r->staging_values != NULL) {
            ds_values_ref_release(r->staging_values);
            r->staging_values = NULL;
        }
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

    /* Also reset transaction state. */
    extern void ds_tx_reset_for_test(void);
    ds_tx_reset_for_test();
}
