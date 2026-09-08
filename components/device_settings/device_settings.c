#include <string.h>

#include "device_settings.h"
#include "device_store.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "gateway_events.h"

static const char *TAG = "device_settings";

static bool reconciliation_values_match(const ds_device_record_t *rec,
                                        const ds_transaction_t *tx)
{
    if (rec->schema == NULL || rec->values == NULL) return false;
    for (uint16_t change_index = 0; change_index < tx->change_count; change_index++) {
        const ds_change_request_t *change = &tx->changes[change_index];
        const ds_value_entry_t *value = NULL;
        for (uint16_t value_index = 0; value_index < rec->values->value_count;
             value_index++) {
            const ds_value_entry_t *candidate = &rec->values->values[value_index];
            const char *id = ds_string_pool_get(&rec->schema->strings,
                                                candidate->id_off);
            if (id != NULL && strcmp(id, change->setting_id) == 0) {
                value = candidate;
                break;
            }
        }
        if (value == NULL || !value->has_value || value->type != change->type) {
            return false;
        }
        switch (change->type) {
        case DS_TYPE_BOOL:
            if (value->bool_val != change->bool_val) return false;
            break;
        case DS_TYPE_INT:
            if (value->int_val != change->int_val) return false;
            break;
        case DS_TYPE_ENUM:
            if (value->enum_val != change->enum_val) return false;
            break;
        case DS_TYPE_STRING: {
            const char *actual = ds_string_pool_get(&rec->values->string_pool,
                                                    value->string_off);
            if (actual == NULL || strcmp(actual, change->string_val.str) != 0) {
                return false;
            }
            break;
        }
        default:
            return false;
        }
    }
    return true;
}

/* ── Per-device records (internal SRAM) ────────────────────────────── */

static ds_device_record_t s_records[DEVICE_SETTINGS_MAX_DEVICES];
static SemaphoreHandle_t  s_mutex;
static bool               s_initialized;

/* ── Observability counters (internal SRAM, no lock needed) ──────── */

ds_diag_t s_diag;

void device_settings_diag_snapshot(ds_diag_t *out)
{
    if (out != NULL) *out = s_diag;
}

void device_settings_diag_reset(void)
{
    memset(&s_diag, 0, sizeof(s_diag));
}

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

    device_settings_protocol_on_disconnect(NULL);

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

esp_err_t device_settings_commit_schema(const char *device_id,
                                        ds_schema_t *schema)
{
    if (device_id == NULL || device_id[0] == '\0' || schema == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!lock()) return ESP_ERR_TIMEOUT;

    ds_device_record_t *rec = device_settings_find_record(device_id);
    if (rec == NULL) {
        unlock();
        return ESP_ERR_NOT_FOUND;
    }

    ds_schema_t *old_schema = rec->schema;
    rec->schema = schema;
    rec->schema_rev = schema->schema_revision;
    rec->schema_state = DS_SCHEMA_READY;
    unlock();

    if (old_schema != NULL) ds_settings_ref_release(old_schema);
    return ESP_OK;
}

esp_err_t device_settings_commit_values(const char *device_id,
                                        ds_values_t *values)
{
    if (device_id == NULL || device_id[0] == '\0' || values == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!lock()) return ESP_ERR_TIMEOUT;

    ds_device_record_t *rec = device_settings_find_record(device_id);
    if (rec == NULL) {
        unlock();
        return ESP_ERR_NOT_FOUND;
    }

    ds_values_t *old_values = rec->values;
    rec->values = values;
    rec->config_rev = values->config_revision;
    unlock();

    if (old_values != NULL) ds_values_ref_release(old_values);
    return ESP_OK;
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

/* ── Capability bridge ─────────────────────────────────────────────── */

void device_settings_on_capability(const char *device_id,
                                   bool supported,
                                   uint16_t schema_revision)
{
    if (device_id == NULL || device_id[0] == '\0' || !s_initialized) return;
    if (!lock()) return;

    ds_device_record_t *rec = device_settings_find_or_create_record(device_id);
    if (rec == NULL) {
        unlock();
        return;
    }

    if (!supported) {
        rec->schema_state = DS_SCHEMA_UNSUPPORTED;
        ESP_LOGI(TAG, "[%s] [CAPABILITY] supported=0", device_id);
        unlock();
        return;
    }

    const bool needs_describe = rec->schema == NULL ||
                                rec->schema_rev != schema_revision;
    rec->advertised_schema_rev = schema_revision;
    rec->schema_state = needs_describe ? DS_SCHEMA_DISCOVERING
                                       : DS_SCHEMA_READY;
    ESP_LOGI(TAG, "[%s] [CAPABILITY] supported=1 schema_rev=%u cached_rev=%lu action=%s",
             device_id, (unsigned)schema_revision,
             (unsigned long)rec->schema_rev,
             needs_describe ? "DESCRIBE" : "READ");
    unlock();

    esp_err_t err = needs_describe
                        ? device_settings_describe(device_id, NULL, NULL)
                        : device_settings_get(device_id, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[%s] capability action failed: %s", device_id,
                 esp_err_to_name(err));
    }
}

/* ── Disconnect handler ────────────────────────────────────────────── */

void device_settings_on_disconnect(const char *device_id)
{
    if (device_id == NULL || device_id[0] == '\0') return;

    device_settings_protocol_on_disconnect(device_id);

    if (!lock()) return;

    ds_device_record_t *rec = device_settings_find_record(device_id);
    if (rec != NULL) {
        /* Reset stream state. Builders are globally serialized and were
         * already discarded by device_settings_protocol_on_disconnect(). */
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

    /* Notify worker to cancel active commands and invalidate pending ops. */
    device_settings_worker_on_disconnect(device_id);
}

/* ── Reconciliation ───────────────────────────────────────────────────
 * Called after values are refreshed following a reconnect when a
 * device has a pending reconciliation.  Compares config_rev against
 * the expected new revision and verifies changed values. */

void device_settings_reconcile(const char *device_id)
{
    if (device_id == NULL || device_id[0] == '\0') return;

    if (!lock()) return;

    ds_device_record_t *rec = device_settings_find_record(device_id);
    if (rec == NULL || !rec->pending_reconciliation) {
        unlock();
        return;
    }

    /* Find the WAITING_REBOOT transaction. */
    extern ds_transaction_t *ds_tx_find(const char *device_id);
    ds_transaction_t *tx = ds_tx_find(device_id);
    if (tx == NULL || tx->state != DS_TX_WAITING_REBOOT) {
        ESP_LOGW(TAG, "[%s] reconcile: no WAITING_REBOOT transaction",
                 device_id);
        rec->pending_reconciliation = false;
        unlock();
        return;
    }

    uint32_t current_rev = rec->config_rev;
    uint32_t expected_rev = rec->reconciliation_expected_rev;
    uint32_t old_rev = rec->reconciliation_old_rev;

    ESP_LOGI(TAG, "[%s] reconcile: current=%lu expected=%lu old=%lu",
             device_id,
             (unsigned long)current_rev,
             (unsigned long)expected_rev,
             (unsigned long)old_rev);

    ds_tx_result_t outcome;

    if (current_rev == expected_rev) {
        tx->state = DS_TX_VERIFYING;
        ESP_LOGI(TAG, "[VERIFY_BEGIN] device=%s revision=%lu",
                 device_id, (unsigned long)current_rev);
        if (reconciliation_values_match(rec, tx)) {
            ESP_LOGI(TAG, "[VERIFY_OK] device=%s revision=%lu",
                     device_id, (unsigned long)current_rev);
            DS_DIAG_INC(reconcile_success);
            outcome = DS_TX_RESULT_OK;
        } else {
            ESP_LOGW(TAG, "[VERIFY_FAIL] device=%s reason=value_mismatch", device_id);
            DS_DIAG_INC(reconcile_fail);
            outcome = DS_TX_RESULT_FAILED;
        }

    } else if (current_rev == old_rev) {
        /* Commit did not persist. */
        ESP_LOGW(TAG, "[%s] reconcile: FAILED (rev still old)",
                 device_id);
        DS_DIAG_INC(reconcile_fail);
        outcome = DS_TX_RESULT_FAILED;

    } else {
        /* Different revision — external change occurred. */
        ESP_LOGW(TAG, "[%s] reconcile: CONFLICT (rev=%lu != expected=%lu)",
                 device_id,
                 (unsigned long)current_rev,
                 (unsigned long)expected_rev);
        DS_DIAG_INC(reconcile_conflict);
        outcome = DS_TX_RESULT_DEVICE_CONFLICT;
    }

    /* Clear reconciliation state. */
    rec->pending_reconciliation = false;

    /* Resolve the transaction. */
    tx->state = (outcome == DS_TX_RESULT_OK) ? DS_TX_SUCCEEDED
                                              : DS_TX_FAILED;
    ds_tx_completion_fn cb = tx->completion;
    void *ctx = tx->context;
    uint32_t rev = rec->config_rev;

    /* Release lock before freeing tx (tx_free may access record). */
    unlock();

    /* Store result in registry and free. */
    extern void ds_tx_free(ds_transaction_t *tx);
    extern void device_settings_tx_store_result(const char *device_id,
                                                ds_tx_result_t result,
                                                uint32_t config_revision);
    device_settings_tx_store_result(tx->device_id, outcome, rev);
    ds_tx_free(tx);
    if (cb != NULL) cb(outcome, ctx);

    /* Publish settings changed event after reconciliation. */
    {
        gateway_event_t ev = {
            .type = GW_EVENT_SETTINGS_CHANGED,
            .config_revision = rev,
        };
        strlcpy(ev.device_id, device_id, sizeof(ev.device_id));
        gateway_events_publish(&ev);
    }
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
    device_settings_protocol_on_disconnect(NULL);
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

    /* Also reset transaction state. */
    extern void ds_tx_reset_for_test(void);
    ds_tx_reset_for_test();
}

/* ── State name helpers ────────────────────────────────────────────── */

const char *device_settings_schema_state_name(ds_schema_state_t state)
{
    switch (state) {
    case DS_SCHEMA_UNKNOWN:     return "unknown";
    case DS_SCHEMA_DISCOVERING: return "discovering";
    case DS_SCHEMA_READY:       return "ready";
    case DS_SCHEMA_UNSUPPORTED: return "unsupported";
    case DS_SCHEMA_ERROR:       return "error";
    }
    return "unknown";
}

const char *device_settings_tx_state_name(ds_tx_state_t state)
{
    switch (state) {
    case DS_TX_IDLE:            return "idle";
    case DS_TX_PREVALIDATING:   return "running";
    case DS_TX_BEGIN_SENT:      return "running";
    case DS_TX_SET_SENT:        return "running";
    case DS_TX_COMMIT_SENT:     return "running";
    case DS_TX_CONFIRM_SENT:    return "running";
    case DS_TX_WAITING_REBOOT:  return "waiting_reboot";
    case DS_TX_VERIFYING:       return "verifying";
    case DS_TX_SUCCEEDED:       return "succeeded";
    case DS_TX_FAILED:          return "failed";
    case DS_TX_CONFLICT:        return "conflict";
    case DS_TX_CANCELLED:       return "cancelled";
    case DS_TX_OUTCOME_UNKNOWN: return "outcome_unknown";
    }
    return "unknown";
}

const char *device_settings_tx_result_name(ds_tx_result_t result)
{
    switch (result) {
    case DS_TX_RESULT_OK:              return NULL;  /* no error */
    case DS_TX_RESULT_BUSY:            return "busy";
    case DS_TX_RESULT_VALIDATION_FAILED: return "validation_failed";
    case DS_TX_RESULT_MEMORY_ERROR:    return "memory_error";
    case DS_TX_RESULT_DEVICE_CONFLICT: return "conflict";
    case DS_TX_RESULT_DEVICE_REJECTED: return "rejected";
    case DS_TX_RESULT_TIMEOUT:         return "timeout";
    case DS_TX_RESULT_DISCONNECTED:    return "disconnected";
    case DS_TX_RESULT_CANCELLED:       return "cancelled";
    case DS_TX_RESULT_FAILED:          return "failed";
    case DS_TX_RESULT_OUTCOME_UNKNOWN: return "outcome_unknown";
    case DS_TX_RESULT_INTERNAL:        return "internal_error";
    }
    return "internal_error";
}
