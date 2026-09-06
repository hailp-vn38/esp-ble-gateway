#include <string.h>

#include "device_settings.h"
#include "device_command_service.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "gw_settings_view.h"
#include "memory_policy.h"

static const char *TAG = "ds_tx";

/* ── Transaction registry (internal SRAM) ───────────────────────────── */

static ds_transaction_t s_txns[DEVICE_SETTINGS_MAX_DEVICES];

/* ── Forward declarations ───────────────────────────────────────────── */

static void cancel_reconciliation_timer(ds_transaction_t *tx);

/* ── Allocation / lookup ────────────────────────────────────────────── */

ds_transaction_t *ds_tx_find(const char *device_id)
{
    if (device_id == NULL || device_id[0] == '\0') return NULL;
    for (int i = 0; i < DEVICE_SETTINGS_MAX_DEVICES; i++) {
        if (s_txns[i].active &&
            strcmp(s_txns[i].device_id, device_id) == 0) {
            return &s_txns[i];
        }
    }
    return NULL;
}

ds_transaction_t *ds_tx_alloc(void)
{
    for (int i = 0; i < DEVICE_SETTINGS_MAX_DEVICES; i++) {
        if (!s_txns[i].active) {
            memset(&s_txns[i], 0, sizeof(s_txns[i]));
            s_txns[i].active = true;
            return &s_txns[i];
        }
    }
    return NULL;
}

void ds_tx_free(ds_transaction_t *tx)
{
    if (tx == NULL) return;
    cancel_reconciliation_timer(tx);
    if (tx->changes != NULL) {
        ds_settings_free(tx->changes);
        tx->changes = NULL;
    }
    tx->active = false;
}

void ds_tx_reset_for_test(void)
{
    for (int i = 0; i < DEVICE_SETTINGS_MAX_DEVICES; i++) {
        ds_tx_free(&s_txns[i]);
    }
}

/* ── Command completion callback (runs from command service task) ────── */

static void on_cmd_complete(const device_command_result_t *result,
                            void *context);

/* ── Reconciliation timer ─────────────────────────────────────────────
 * Fires if device does not reconnect within DS_RECONCILIATION_TIMEOUT_MS
 * after COMMIT ACK.  Resolves the transaction as OUTCOME_UNKNOWN.
 * Timer handle is stored on the transaction for direct cleanup. */

static void reconciliation_timeout_cb(TimerHandle_t timer)
{
    /* The timer ID is the transaction pointer. */
    ds_transaction_t *tx = (ds_transaction_t *)pvTimerGetTimerID(timer);
    if (tx == NULL || !tx->active) return;

    tx->recon_timer = NULL;

    if (tx->state != DS_TX_WAITING_REBOOT) return;

    ESP_LOGW(TAG, "[%s] reconciliation timeout — OUTCOME_UNKNOWN",
             tx->device_id);

    ds_device_record_t *rec = device_settings_find_record(tx->device_id);
    if (rec != NULL) {
        rec->pending_reconciliation = false;
    }

    tx->state = DS_TX_FAILED;
    ds_tx_completion_fn cb = tx->completion;
    void *ctx = tx->context;
    ds_tx_free(tx);
    if (cb != NULL) cb(DS_TX_RESULT_OUTCOME_UNKNOWN, ctx);
}

static void cancel_reconciliation_timer(ds_transaction_t *tx)
{
    if (tx == NULL || tx->recon_timer == NULL) return;
    xTimerDelete((TimerHandle_t)tx->recon_timer, 0);
    tx->recon_timer = NULL;
}

static void start_reconciliation_timer(ds_transaction_t *tx)
{
    if (tx == NULL) return;

    TimerHandle_t t = xTimerCreate(
        "ds_recon",
        pdMS_TO_TICKS(DS_RECONCILIATION_TIMEOUT_MS),
        pdFALSE,              /* one-shot */
        (void *)tx,
        reconciliation_timeout_cb);

    if (t != NULL) {
        tx->recon_timer = t;
        if (xTimerStart(t, 0) != pdPASS) {
            ESP_LOGW(TAG, "[%s] timer start failed", tx->device_id);
            tx->recon_timer = NULL;
            xTimerDelete(t, 0);
        }
    }
}

/* ── Prevalidate one change against schema ──────────────────────────── */

static bool prevalidate_change(const ds_schema_t *schema,
                               const ds_change_request_t *change)
{
    if (schema == NULL || change == NULL) return false;
    if (change->setting_id[0] == '\0') return false;

    for (uint16_t i = 0; i < schema->setting_count; i++) {
        const char *id = ds_string_pool_get(&schema->strings,
                                            schema->descriptors[i].id_off);
        if (id != NULL && strcmp(id, change->setting_id) == 0) {
            const ds_setting_desc_t *desc = &schema->descriptors[i];

            /* Must be writable. */
            if (!(desc->flags & DS_FLAG_WRITABLE)) return false;

            /* Type must match. */
            if (desc->type != change->type) return false;

            /* Range check for INT. */
            if (desc->type == DS_TYPE_INT) {
                if (change->int_val < desc->min_value ||
                    change->int_val > desc->max_value) {
                    return false;
                }
            }
            return true;
        }
    }
    /* Setting ID not found in schema. */
    return false;
}

/* ── Submit next SET or COMMIT command ──────────────────────────────── */

static void submit_next_command(ds_transaction_t *tx)
{
    if (tx == NULL || !tx->active) return;

    /* Look up device record for schema/values. */
    ds_device_record_t *rec = device_settings_find_record(tx->device_id);
    if (rec == NULL || rec->schema == NULL) {
        ESP_LOGE(TAG, "[%s] no schema for command dispatch", tx->device_id);
        tx->state = DS_TX_FAILED;
        ds_tx_result_t r = DS_TX_RESULT_INTERNAL;
        ds_tx_completion_fn cb = tx->completion;
        void *ctx = tx->context;
        ds_tx_free(tx);
        if (cb != NULL) cb(r, ctx);
        return;
    }

    /* Check if we have more SETs to send. */
    if (tx->next_change_index < tx->change_count) {
        /* Build set_settings command with the current change. */
        const ds_change_request_t *ch = &tx->changes[tx->next_change_index];

        device_command_request_t req = {0};
        req.origin = DEVICE_CMD_ORIGIN_SETTINGS;
        strlcpy(req.device_id, tx->device_id, sizeof(req.device_id));
        strlcpy(req.command, GW_SETTINGS_CMD_SET_SETTINGS, sizeof(req.command));

        /* Encode setting_id into feature_id field (repurposed for settings). */
        strlcpy(req.feature_id, ch->setting_id, sizeof(req.feature_id));
        req.has_feature_id = true;

        /* Encode value into int_value (bool/int/enum share this). */
        switch (ch->type) {
        case DS_TYPE_BOOL:
            req.has_bool_value = true;
            req.bool_value = ch->bool_val;
            break;
        case DS_TYPE_INT:
            req.has_int_value = true;
            req.int_value = ch->int_val;
            break;
        case DS_TYPE_FLOAT:
            req.has_int_value = true;
            req.int_value = (int32_t)ch->float_val;
            break;
        case DS_TYPE_ENUM:
            req.has_int_value = true;
            req.int_value = ch->enum_val;
            break;
        default:
            break;
        }

        tx->state = DS_TX_SET_SENT;
        ESP_LOGI(TAG, "[%s] SET %u/%u id='%s'",
                 tx->device_id,
                 (unsigned)(tx->next_change_index + 1),
                 (unsigned)tx->change_count,
                 ch->setting_id);

        esp_err_t err = device_command_service_submit(
            &req, on_cmd_complete, tx);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "[%s] SET submit failed: %s",
                     tx->device_id, esp_err_to_name(err));
            tx->state = DS_TX_FAILED;
            ds_tx_result_t r = DS_TX_RESULT_INTERNAL;
            ds_tx_completion_fn cb = tx->completion;
            void *ctx = tx->context;
            ds_tx_free(tx);
            if (cb != NULL) cb(r, ctx);
        }
        return;
    }

    /* All SETs sent — send COMMIT. */
    device_command_request_t req = {0};
    req.origin = DEVICE_CMD_ORIGIN_SETTINGS;
    strlcpy(req.device_id, tx->device_id, sizeof(req.device_id));
    strlcpy(req.command, GW_SETTINGS_CMD_COMMIT_SETTINGS, sizeof(req.command));

    tx->state = DS_TX_COMMIT_SENT;
    ESP_LOGI(TAG, "[%s] COMMIT", tx->device_id);

    esp_err_t err = device_command_service_submit(
        &req, on_cmd_complete, tx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[%s] COMMIT submit failed: %s",
                 tx->device_id, esp_err_to_name(err));
        tx->state = DS_TX_FAILED;
        ds_tx_result_t r = DS_TX_RESULT_INTERNAL;
        ds_tx_completion_fn cb = tx->completion;
        void *ctx = tx->context;
        ds_tx_free(tx);
        if (cb != NULL) cb(r, ctx);
    }
}

/* ── Command completion callback ────────────────────────────────────── */

static void on_cmd_complete(const device_command_result_t *result,
                            void *context)
{
    ds_transaction_t *tx = context;
    if (tx == NULL || !tx->active) return;

    if (result == NULL) {
        ESP_LOGE(TAG, "[%s] cmd callback with NULL result", tx->device_id);
        tx->state = DS_TX_FAILED;
        ds_tx_completion_fn cb = tx->completion;
        void *ctx = tx->context;
        ds_tx_free(tx);
        if (cb != NULL) cb(DS_TX_RESULT_INTERNAL, ctx);
        return;
    }

    switch (result->status) {
    case DEVICE_CMD_STATUS_OK:
        /* Success — advance state machine. */
        if (tx->state == DS_TX_SET_SENT) {
            tx->next_change_index++;
            submit_next_command(tx);
        } else if (tx->state == DS_TX_COMMIT_SENT) {
            /* COMMIT ACK received — transition to WAITING_REBOOT.
             * Do not fire completion yet; verify after reconnect. */
            if (result->has_int_value) {
                tx->new_config_rev = (uint32_t)result->int_value;
            }
            tx->state = DS_TX_WAITING_REBOOT;
            ESP_LOGI(TAG, "[%s] COMMIT_ACK new_config_rev=%lu — waiting reboot",
                     tx->device_id,
                     (unsigned long)tx->new_config_rev);

            /* Set reconciliation state on device record. */
            ds_device_record_t *rec =
                device_settings_find_record(tx->device_id);
            if (rec != NULL) {
                rec->pending_reconciliation = true;
                rec->reconciliation_expected_rev = tx->new_config_rev;
                rec->reconciliation_old_rev = tx->expected_config_rev;
            }

            /* Start timeout timer. */
            start_reconciliation_timer(tx);
        }
        break;

    case DEVICE_CMD_STATUS_REJECTED:
        ESP_LOGW(TAG, "[%s] REJECTED in state %d", tx->device_id, tx->state);
        tx->state = DS_TX_FAILED;
        {
            ds_tx_result_t r = DS_TX_RESULT_DEVICE_REJECTED;
            ds_tx_completion_fn cb = tx->completion;
            void *ctx = tx->context;
            ds_tx_free(tx);
            if (cb != NULL) cb(r, ctx);
        }
        break;

    case DEVICE_CMD_STATUS_BUSY:
        /* Device was busy — could retry, but for now fail. */
        ESP_LOGW(TAG, "[%s] BUSY in state %d", tx->device_id, tx->state);
        tx->state = DS_TX_FAILED;
        {
            ds_tx_result_t r = DS_TX_RESULT_DEVICE_REJECTED;
            ds_tx_completion_fn cb = tx->completion;
            void *ctx = tx->context;
            ds_tx_free(tx);
            if (cb != NULL) cb(r, ctx);
        }
        break;

    case DEVICE_CMD_STATUS_TIMEOUT:
        ESP_LOGW(TAG, "[%s] TIMEOUT in state %d", tx->device_id, tx->state);
        if (tx->state == DS_TX_COMMIT_SENT) {
            /* Ambiguous — commit may have persisted.  Set up reconciliation. */
            ESP_LOGW(TAG, "[%s] TIMEOUT during COMMIT — OUTCOME_UNKNOWN",
                     tx->device_id);
            tx->state = DS_TX_WAITING_REBOOT;
            ds_device_record_t *rec =
                device_settings_find_record(tx->device_id);
            if (rec != NULL) {
                rec->pending_reconciliation = true;
                rec->reconciliation_expected_rev = tx->expected_config_rev + 1;
                rec->reconciliation_old_rev = tx->expected_config_rev;
            }
            start_reconciliation_timer(tx);
        } else {
            tx->state = DS_TX_FAILED;
            {
                ds_tx_result_t r = DS_TX_RESULT_TIMEOUT;
                ds_tx_completion_fn cb = tx->completion;
                void *ctx = tx->context;
                ds_tx_free(tx);
                if (cb != NULL) cb(r, ctx);
            }
        }
        break;

    case DEVICE_CMD_STATUS_NOT_CONNECTED:
        ESP_LOGW(TAG, "[%s] DISCONNECTED in state %d",
                 tx->device_id, tx->state);
        if (tx->state == DS_TX_COMMIT_SENT) {
            /* Ambiguous — commit may have persisted.  Set up reconciliation. */
            ESP_LOGW(TAG, "[%s] DISCONNECT during COMMIT — OUTCOME_UNKNOWN",
                     tx->device_id);
            tx->state = DS_TX_WAITING_REBOOT;
            ds_device_record_t *rec =
                device_settings_find_record(tx->device_id);
            if (rec != NULL) {
                rec->pending_reconciliation = true;
                rec->reconciliation_expected_rev = tx->expected_config_rev + 1;
                rec->reconciliation_old_rev = tx->expected_config_rev;
            }
            start_reconciliation_timer(tx);
        } else {
            tx->state = DS_TX_FAILED;
            {
                ds_tx_result_t r = DS_TX_RESULT_DISCONNECTED;
                ds_tx_completion_fn cb = tx->completion;
                void *ctx = tx->context;
                ds_tx_free(tx);
                if (cb != NULL) cb(r, ctx);
            }
        }
        break;

    case DEVICE_CMD_STATUS_CANCELLED:
        ESP_LOGI(TAG, "[%s] CANCELLED", tx->device_id);
        tx->state = DS_TX_CANCELLED;
        {
            ds_tx_completion_fn cb = tx->completion;
            void *ctx = tx->context;
            ds_tx_free(tx);
            if (cb != NULL) cb(DS_TX_RESULT_CANCELLED, ctx);
        }
        break;

    default:
        ESP_LOGW(TAG, "[%s] UNEXPECTED status=%d in state %d",
                 tx->device_id, result->status, tx->state);
        tx->state = DS_TX_FAILED;
        {
            ds_tx_result_t r = DS_TX_RESULT_INTERNAL;
            ds_tx_completion_fn cb = tx->completion;
            void *ctx = tx->context;
            ds_tx_free(tx);
            if (cb != NULL) cb(r, ctx);
        }
        break;
    }
}

/* ── Public API: save ───────────────────────────────────────────────── */

esp_err_t device_settings_save(const char *device_id,
                               const ds_change_request_t *changes,
                               uint16_t change_count,
                               uint32_t expected_config_rev,
                               ds_tx_completion_fn completion,
                               void *context)
{
    if (device_id == NULL || device_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (changes == NULL || change_count == 0 ||
        change_count > DS_TX_MAX_CHANGES) {
        return ESP_ERR_INVALID_ARG;
    }

    /* One active transaction per device. */
    if (ds_tx_find(device_id) != NULL) {
        ESP_LOGW(TAG, "[%s] save: already active", device_id);
        return ESP_ERR_INVALID_STATE;
    }

    /* Must have committed schema. */
    ds_device_record_t *rec = device_settings_find_record(device_id);
    if (rec == NULL || rec->schema_state != DS_SCHEMA_READY ||
        rec->schema == NULL) {
        ESP_LOGW(TAG, "[%s] save: schema not ready", device_id);
        return ESP_ERR_INVALID_STATE;
    }

    /* Allocate transaction. */
    ds_transaction_t *tx = ds_tx_alloc();
    if (tx == NULL) {
        ESP_LOGE(TAG, "[%s] save: no free transaction slot", device_id);
        return ESP_ERR_NO_MEM;
    }

    strlcpy(tx->device_id, device_id, sizeof(tx->device_id));
    tx->expected_config_rev = expected_config_rev;

    /* Deep-copy changes into PSRAM. */
    tx->changes = ds_settings_alloc(change_count * sizeof(ds_change_request_t));
    if (tx->changes == NULL) {
        ds_tx_free(tx);
        return ESP_ERR_NO_MEM;
    }
    memcpy(tx->changes, changes, change_count * sizeof(ds_change_request_t));
    tx->change_count = change_count;
    tx->next_change_index = 0;

    tx->completion = completion;
    tx->context = context;

    /* Prevalidate all changes against schema. */
    tx->state = DS_TX_PREVALIDATING;
    for (uint16_t i = 0; i < change_count; i++) {
        if (!prevalidate_change(rec->schema, &tx->changes[i])) {
            ESP_LOGW(TAG, "[%s] prevalidate FAIL at change %u id='%s'",
                     device_id, (unsigned)i, tx->changes[i].setting_id);
            ds_tx_free(tx);
            return ESP_ERR_INVALID_ARG;
        }
    }

    ESP_LOGI(TAG, "[%s] SAVE %u changes, config_rev=%lu",
             device_id, (unsigned)change_count,
             (unsigned long)expected_config_rev);

    /* Send BEGIN command. */
    device_command_request_t req = {0};
    req.origin = DEVICE_CMD_ORIGIN_SETTINGS;
    strlcpy(req.device_id, device_id, sizeof(req.device_id));
    strlcpy(req.command, GW_SETTINGS_CMD_SET_SETTINGS, sizeof(req.command));
    req.has_int_value = true;
    req.int_value = (int32_t)expected_config_rev;

    tx->state = DS_TX_BEGIN_SENT;
    esp_err_t err = device_command_service_submit(
        &req, on_cmd_complete, tx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[%s] BEGIN submit failed: %s",
                 device_id, esp_err_to_name(err));
        ds_tx_free(tx);
        return err;
    }

    return ESP_OK;
}

/* ── Public API: cancel ─────────────────────────────────────────────── */

esp_err_t device_settings_tx_cancel(const char *device_id)
{
    if (device_id == NULL || device_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    ds_transaction_t *tx = ds_tx_find(device_id);
    if (tx == NULL) return ESP_ERR_NOT_FOUND;

    ESP_LOGI(TAG, "[%s] TX cancel requested", device_id);

    /* Cancel any pending command in the command service. */
    device_command_service_cancel_device(device_id);

    /* The command service will call our callback with CANCELLED,
     * which will free the transaction. */
    return ESP_OK;
}
