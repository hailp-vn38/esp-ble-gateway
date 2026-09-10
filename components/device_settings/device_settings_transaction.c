#include <string.h>

#include "device_settings.h"
#include "device_control_scheduler.h"
#include "device_settings_internal.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "gw_settings_view.h"
#include "gateway_events.h"
#include "memory_policy.h"

static const char *TAG = "ds_tx";

/* ── Transaction registry (internal SRAM) ───────────────────────────── */

static ds_transaction_t s_txns[DEVICE_SETTINGS_MAX_DEVICES];
static uint64_t s_next_transaction_id;

static uint8_t ws_tx_state(ds_tx_state_t state)
{
    switch (state) {
    case DS_TX_IDLE: return GW_SETTINGS_TX_QUEUED;
    case DS_TX_PREVALIDATING: return GW_SETTINGS_TX_VALIDATING;
    case DS_TX_BEGIN_SENT: return GW_SETTINGS_TX_STARTING;
    case DS_TX_SET_SENT: return GW_SETTINGS_TX_APPLYING;
    case DS_TX_COMMIT_SENT: return GW_SETTINGS_TX_COMMITTING;
    case DS_TX_CONFIRM_SENT: return GW_SETTINGS_TX_CONFIRMING;
    case DS_TX_WAITING_REBOOT: return GW_SETTINGS_TX_WAITING_REBOOT;
    case DS_TX_VERIFYING: return GW_SETTINGS_TX_VERIFYING;
    case DS_TX_SUCCEEDED: return GW_SETTINGS_TX_SUCCEEDED;
    case DS_TX_CONFLICT: return GW_SETTINGS_TX_CONFLICT;
    case DS_TX_CANCELLED: return GW_SETTINGS_TX_CANCELLED;
    case DS_TX_OUTCOME_UNKNOWN: return GW_SETTINGS_TX_OUTCOME_UNKNOWN;
    default: return GW_SETTINGS_TX_FAILED;
    }
}

static void publish_tx(const ds_transaction_t *tx)
{
    if (tx == NULL || !tx->active) return;
    gateway_event_t ev = { .type = GW_EVENT_SETTINGS_TRANSACTION };
    strlcpy(ev.device_id, tx->device_id, sizeof(ev.device_id));
    ev.transaction_id = tx->transaction_id;
    ev.expected_revision = tx->expected_config_rev;
    ev.new_revision = tx->new_config_rev;
    ev.progress_current = tx->next_change_index;
    ev.progress_total = tx->change_count;
    ev.settings_tx_state = ws_tx_state(tx->state);
    gateway_events_publish(&ev);
}

static void transition_tx(ds_transaction_t *tx, ds_tx_state_t state)
{
    if (tx == NULL) return;
    if (tx->state == state) return;
    tx->state = state;
    publish_tx(tx);
}

/* ── Result registry — last completed result per device ────────────────
 * Stores the outcome of the most recent completed transaction so the
 * web layer can query it after the transaction object is freed. */

typedef struct {
    bool           used;
    char           device_id[32];
    ds_tx_result_t result;
    uint32_t       config_revision;
} ds_tx_result_record_t;

static ds_tx_result_record_t s_tx_results[DEVICE_SETTINGS_MAX_DEVICES];

void device_settings_tx_store_result(const char *device_id,
                                     ds_tx_result_t result,
                                     uint32_t config_revision)
{
    if (device_id == NULL || device_id[0] == '\0') return;

    /* Find existing or allocate new slot. */
    int free_slot = -1;
    for (int i = 0; i < DEVICE_SETTINGS_MAX_DEVICES; i++) {
        if (s_tx_results[i].used &&
            strcmp(s_tx_results[i].device_id, device_id) == 0) {
            s_tx_results[i].result = result;
            s_tx_results[i].config_revision = config_revision;
            return;
        }
        if (!s_tx_results[i].used && free_slot < 0) {
            free_slot = i;
        }
    }

    if (free_slot >= 0) {
        s_tx_results[free_slot].used = true;
        strlcpy(s_tx_results[free_slot].device_id, device_id,
                sizeof(s_tx_results[free_slot].device_id));
        s_tx_results[free_slot].result = result;
        s_tx_results[free_slot].config_revision = config_revision;
    }
}

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

/* Store result in registry, free transaction, and invoke callback. */
static void ds_tx_complete(ds_transaction_t *tx, ds_tx_result_t result,
                           uint32_t config_revision)
{
    if (tx == NULL) return;
    if (result == DS_TX_RESULT_OK) transition_tx(tx, DS_TX_SUCCEEDED);
    else if (result == DS_TX_RESULT_DEVICE_CONFLICT) transition_tx(tx, DS_TX_CONFLICT);
    else if (result == DS_TX_RESULT_CANCELLED) transition_tx(tx, DS_TX_CANCELLED);
    else if (result == DS_TX_RESULT_OUTCOME_UNKNOWN) transition_tx(tx, DS_TX_OUTCOME_UNKNOWN);
    else transition_tx(tx, DS_TX_FAILED);
    if (tx->lease_id != 0) {
        (void)device_control_scheduler_release_lease(tx->lease_id);
        tx->lease_id = 0;
    }
    device_settings_tx_store_result(tx->device_id, result, config_revision);
    ds_tx_completion_fn cb = tx->completion;
    void *ctx = tx->context;
    ds_tx_free(tx);
    if (cb != NULL) cb(result, ctx);
}

void ds_tx_reset_for_test(void)
{
    for (int i = 0; i < DEVICE_SETTINGS_MAX_DEVICES; i++) {
        ds_tx_free(&s_txns[i]);
    }
    memset(s_tx_results, 0, sizeof(s_tx_results));
    s_next_transaction_id = 0;
}

static uint64_t next_transaction_id(void)
{
    /* Monotonic IDs are unique among active transactions. Zero is reserved
     * as absent on the Protocol v4 wire. */
    do {
        s_next_transaction_id++;
        if (s_next_transaction_id == 0) s_next_transaction_id++;
    } while (s_next_transaction_id == 0);
    return s_next_transaction_id;
}

/* ── Command completion callback (runs from command service task) ────── */

static void on_scheduler_complete(const device_control_result_t *result,
                                  void *context)
{
    const ds_transaction_t *tx = context;
    if (tx == NULL) return;
    ds_event_t ev = { .type = DS_EVENT_COMMAND_COMPLETE, .slot = -1,
        .generation = (uint32_t)tx->transaction_id, .owner_token = tx->owner_token };
    strlcpy(ev.device_id, tx->device_id, sizeof(ev.device_id));
    if (result == NULL) ev.data.command.status = DEVICE_CMD_STATUS_INTERNAL;
    else if (result->terminal == DEVICE_CTRL_TERMINAL_COMMAND) ev.data.command = result->command_result;
    else ev.data.command.status = DEVICE_CMD_STATUS_CANCELLED;
    (void)ds_events_post(&ev);
}

static esp_err_t submit_tx_command(ds_transaction_t *tx,
                                   const device_command_request_t *request)
{
    device_control_job_t job = {
        .request = *request,
        .priority = DEVICE_CTRL_PRIORITY_HIGH,
        .source = DEVICE_CTRL_SOURCE_SETTINGS,
        .owner_token = tx->owner_token,
        .lease_id = tx->lease_id,
    };
    return device_control_scheduler_submit(&job, on_scheduler_complete, tx, NULL);
}

/* ── Reconciliation timer ─────────────────────────────────────────────
 * Fires if device does not reconnect within DS_RECONCILIATION_TIMEOUT_MS
 * after COMMIT ACK.  Resolves the transaction as OUTCOME_UNKNOWN.
 * Timer handle is stored on the transaction for direct cleanup. */

static void reconciliation_timeout_cb(TimerHandle_t timer)
{
    /* The timer ID is the transaction pointer. */
    ds_transaction_t *tx = (ds_transaction_t *)pvTimerGetTimerID(timer);
    if (tx == NULL || !tx->active) return;

    ds_event_t ev = { .type = DS_EVENT_TRANSACTION_TIMEOUT };
    strlcpy(ev.device_id, tx->device_id, sizeof(ev.device_id));
    (void)ds_events_post(&ev);
}

static void cancel_reconciliation_timer(ds_transaction_t *tx)
{
    if (tx == NULL || tx->recon_timer == NULL) return;
    xTimerDelete((TimerHandle_t)tx->recon_timer, 0);
    tx->recon_timer = NULL;
}

static void start_reconciliation_timer(ds_transaction_t *tx)
{
    if (tx == NULL || tx->recon_timer != NULL) return;

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

static void release_tx_lease(ds_transaction_t *tx)
{
    if (tx == NULL || tx->lease_id == 0) return;
    esp_err_t err = device_control_scheduler_release_lease(tx->lease_id);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "[%s] lease %lu release failed: %s", tx->device_id,
                 (unsigned long)tx->lease_id, esp_err_to_name(err));
    }
    tx->lease_id = 0;
}

static void mark_reconciliation_pending(ds_transaction_t *tx)
{
    if (tx == NULL || !tx->active) return;

    uint32_t expected = tx->new_config_rev != 0
                            ? tx->new_config_rev
                            : tx->expected_config_rev + 1;
    ds_device_record_t *rec = device_settings_find_record(tx->device_id);
    if (rec != NULL) {
        rec->pending_reconciliation = true;
        rec->reconciliation_expected_rev = expected;
        rec->reconciliation_old_rev = tx->expected_config_rev;
    }
    transition_tx(tx, DS_TX_WAITING_REBOOT);
    start_reconciliation_timer(tx);
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

            if ((desc->flags & DS_FLAG_WRITABLE) == 0) return false;

            /* Type must match. */
            if (desc->type != change->type) return false;
            if (desc->type == DS_TYPE_FLOAT) return false; /* not a G7 wire value */

            /* Range check for INT. */
            if (desc->type == DS_TYPE_INT) {
                if (change->int_val < desc->min_value ||
                    change->int_val > desc->max_value ||
                    (desc->step != 0 &&
                     ((uint32_t)(change->int_val - desc->min_value) %
                      desc->step) != 0)) {
                    return false;
                }
            }
            if (desc->type == DS_TYPE_ENUM) {
                bool found = false;
                for (uint8_t opt = 0; opt < desc->option_count; opt++) {
                    if (schema->enum_option_pool[desc->option_index + opt].value ==
                        change->enum_val) {
                        found = true;
                        break;
                    }
                }
                if (!found) return false;
            }
            if (desc->type == DS_TYPE_STRING &&
                (strnlen(change->string_val.str,
                         sizeof(change->string_val.str)) >=
                     sizeof(change->string_val.str) ||
                 strnlen(change->string_val.str,
                         sizeof(change->string_val.str)) >=
                     GW_SETTINGS_VALUE_STR_LEN ||
                 (desc->max_length != 0 &&
                  strlen(change->string_val.str) > desc->max_length))) {
                return false;
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
        transition_tx(tx, DS_TX_FAILED);
        ds_tx_complete(tx, DS_TX_RESULT_INTERNAL, 0);
        return;
    }

    /* Check if we have more SETs to send. */
    if (tx->next_change_index < tx->change_count) {
        /* Build canonical settings_tx_set command with the current change. */
        const ds_change_request_t *ch = &tx->changes[tx->next_change_index];

        device_command_request_t req = {0};
        req.origin = DEVICE_CMD_ORIGIN_SETTINGS;
        strlcpy(req.device_id, tx->device_id, sizeof(req.device_id));
        strlcpy(req.command, GW_SETTINGS_CMD_TX_SET, sizeof(req.command));
        req.settings.has_transaction_id = true;
        req.settings.transaction_id = tx->transaction_id;
        req.settings.has_setting_id = true;
        strlcpy(req.settings.setting_id, ch->setting_id,
                sizeof(req.settings.setting_id));
        req.settings.has_setting_value = true;
        req.settings.setting_type = ch->type;
        switch (ch->type) {
        case DS_TYPE_BOOL:
            req.settings.value.bool_value = ch->bool_val;
            break;
        case DS_TYPE_INT:
            req.settings.value.int_value = ch->int_val;
            break;
        case DS_TYPE_ENUM:
            req.settings.value.enum_value = (uint8_t)ch->enum_val;
            break;
        case DS_TYPE_STRING:
            strlcpy(req.settings.value.string_value, ch->string_val.str,
                    sizeof(req.settings.value.string_value));
            break;
        default:
            break;
        }

        transition_tx(tx, DS_TX_SET_SENT);
        ESP_LOGI(TAG, "[TX_SET] device=%s tx_id=%llu sequence=%u/%u id=%s",
                 tx->device_id,
                 (unsigned long long)tx->transaction_id,
                 (unsigned)(tx->next_change_index + 1),
                 (unsigned)tx->change_count,
                 ch->setting_id);

        esp_err_t err = submit_tx_command(tx, &req);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "[%s] SET submit failed: %s",
                     tx->device_id, esp_err_to_name(err));
            transition_tx(tx, DS_TX_FAILED);
            ds_tx_complete(tx, DS_TX_RESULT_INTERNAL, 0);
        }
        return;
    }

    /* All SETs sent — send canonical COMMIT. */
    device_command_request_t req = {0};
    req.origin = DEVICE_CMD_ORIGIN_SETTINGS;
    strlcpy(req.device_id, tx->device_id, sizeof(req.device_id));
    strlcpy(req.command, GW_SETTINGS_CMD_TX_COMMIT, sizeof(req.command));
    req.settings.has_transaction_id = true;
    req.settings.transaction_id = tx->transaction_id;

    transition_tx(tx, DS_TX_COMMIT_SENT);
    ESP_LOGI(TAG, "[TX_COMMIT] device=%s tx_id=%llu", tx->device_id,
             (unsigned long long)tx->transaction_id);

    esp_err_t err = submit_tx_command(tx, &req);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[%s] COMMIT submit failed: %s",
                 tx->device_id, esp_err_to_name(err));
        transition_tx(tx, DS_TX_FAILED);
        ds_tx_complete(tx, DS_TX_RESULT_INTERNAL, 0);
    }
}

/* ── Command completion callback ────────────────────────────────────── */

void device_settings_tx_actor_command_complete(const char *device_id,
                                               const device_command_result_t *result)
{
    ds_transaction_t *tx = ds_tx_find(device_id);
    if (tx == NULL || !tx->active) return;

    if (result == NULL) {
        ESP_LOGE(TAG, "[%s] cmd callback with NULL result", tx->device_id);
        transition_tx(tx, DS_TX_FAILED);
        ds_tx_complete(tx, DS_TX_RESULT_INTERNAL, 0);
        return;
    }

    /* A peripheral reboot can race the final COMMIT/CONFIRM completion with
     * the BLE disconnect event.  Once reconciliation is pending, transport
     * cancellation is expected and must not destroy the transaction that the
     * post-reboot values stream will verify. */
    if (tx->state == DS_TX_WAITING_REBOOT) {
        if (result->status == DEVICE_CMD_STATUS_OK && result->has_int_value) {
            tx->new_config_rev = (uint32_t)result->int_value;
            ds_device_record_t *rec = device_settings_find_record(tx->device_id);
            if (rec != NULL) {
                rec->reconciliation_expected_rev = tx->new_config_rev;
            }
        }
        ESP_LOGI(TAG, "[%s] ignore transport completion status=%d while waiting reboot",
                 tx->device_id, (int)result->status);
        return;
    }

    switch (result->status) {
    case DEVICE_CMD_STATUS_OK:
        /* Success — advance state machine. */
        if (tx->state == DS_TX_BEGIN_SENT) {
            submit_next_command(tx);
        } else if (tx->state == DS_TX_SET_SENT) {
            tx->next_change_index++;
            publish_tx(tx);
            submit_next_command(tx);
        } else if (tx->state == DS_TX_COMMIT_SENT) {
            /* COMMIT ACK received — transition to WAITING_REBOOT.
             * Do not fire completion yet; verify after reconnect. */
            if (result->has_int_value) {
                tx->new_config_rev = (uint32_t)result->int_value;
            }
            ESP_LOGI(TAG, "[COMMIT_ACK] device=%s tx_id=%llu new_config_rev=%lu",
                     tx->device_id,
                     (unsigned long long)tx->transaction_id,
                     (unsigned long)tx->new_config_rev);

            device_command_request_t confirm = {0};
            confirm.origin = DEVICE_CMD_ORIGIN_SETTINGS;
            strlcpy(confirm.device_id, tx->device_id, sizeof(confirm.device_id));
            strlcpy(confirm.command, GW_SETTINGS_CMD_COMMIT_CONFIRM,
                    sizeof(confirm.command));
            confirm.settings.has_transaction_id = true;
            confirm.settings.transaction_id = tx->transaction_id;
            confirm.settings.has_new_revision = true;
            confirm.settings.new_revision = tx->new_config_rev;
            transition_tx(tx, DS_TX_CONFIRM_SENT);
            ESP_LOGI(TAG, "[TX_CONFIRM] device=%s tx_id=%llu new_config_rev=%lu",
                     tx->device_id, (unsigned long long)tx->transaction_id,
                     (unsigned long)tx->new_config_rev);
            if (submit_tx_command(tx, &confirm) != ESP_OK) {
                transition_tx(tx, DS_TX_FAILED);
                ds_tx_complete(tx, DS_TX_RESULT_INTERNAL, 0);
            }
        } else if (tx->state == DS_TX_CONFIRM_SENT) {
            mark_reconciliation_pending(tx);
            DS_DIAG_INC(tx_success);
            ESP_LOGI(TAG, "[WAIT_REBOOT] device=%s tx_id=%llu new_config_rev=%lu",
                     tx->device_id, (unsigned long long)tx->transaction_id,
                     (unsigned long)tx->new_config_rev);

        }
        break;

    case DEVICE_CMD_STATUS_REJECTED:
        ESP_LOGW(TAG, "[%s] REJECTED in state %d", tx->device_id, tx->state);
        if (tx->state == DS_TX_BEGIN_SENT) {
            transition_tx(tx, DS_TX_CONFLICT);
            DS_DIAG_INC(tx_conflict);
            ds_tx_complete(tx, DS_TX_RESULT_DEVICE_CONFLICT, 0);
            break;
        }
        transition_tx(tx, DS_TX_FAILED);
        DS_DIAG_INC(tx_fail);
        ds_tx_complete(tx, DS_TX_RESULT_DEVICE_REJECTED, 0);
        break;

    case DEVICE_CMD_STATUS_BUSY:
        /* Device was busy — could retry, but for now fail. */
        ESP_LOGW(TAG, "[%s] BUSY in state %d", tx->device_id, tx->state);
        transition_tx(tx, DS_TX_FAILED);
        DS_DIAG_INC(tx_fail);
        ds_tx_complete(tx, DS_TX_RESULT_DEVICE_REJECTED, 0);
        break;

    case DEVICE_CMD_STATUS_TIMEOUT:
        ESP_LOGW(TAG, "[%s] TIMEOUT in state %d", tx->device_id, tx->state);
        if (tx->state == DS_TX_COMMIT_SENT || tx->state == DS_TX_CONFIRM_SENT) {
            /* Ambiguous — commit may have persisted.  Set up reconciliation. */
            ESP_LOGW(TAG, "[%s] TIMEOUT during COMMIT — OUTCOME_UNKNOWN",
                     tx->device_id);
            DS_DIAG_INC(outcome_unknown);
            mark_reconciliation_pending(tx);
            release_tx_lease(tx);
        } else {
            transition_tx(tx, DS_TX_FAILED);
            DS_DIAG_INC(tx_fail);
            ds_tx_complete(tx, DS_TX_RESULT_TIMEOUT, 0);
        }
        break;

    case DEVICE_CMD_STATUS_NOT_CONNECTED:
        ESP_LOGW(TAG, "[%s] DISCONNECTED in state %d",
                 tx->device_id, tx->state);
        if (tx->state == DS_TX_COMMIT_SENT || tx->state == DS_TX_CONFIRM_SENT) {
            /* Ambiguous — commit may have persisted.  Set up reconciliation. */
            ESP_LOGW(TAG, "[%s] DISCONNECT during COMMIT — OUTCOME_UNKNOWN",
                     tx->device_id);
            DS_DIAG_INC(outcome_unknown);
            mark_reconciliation_pending(tx);
            release_tx_lease(tx);
        } else {
            transition_tx(tx, DS_TX_FAILED);
            DS_DIAG_INC(tx_fail);
            ds_tx_complete(tx, DS_TX_RESULT_DISCONNECTED, 0);
        }
        break;

    case DEVICE_CMD_STATUS_CANCELLED:
        ESP_LOGI(TAG, "[%s] CANCELLED", tx->device_id);
        transition_tx(tx, DS_TX_CANCELLED);
        DS_DIAG_INC(tx_fail);
        ds_tx_complete(tx, DS_TX_RESULT_CANCELLED, 0);
        break;

    default:
        ESP_LOGW(TAG, "[%s] UNEXPECTED status=%d in state %d",
                 tx->device_id, result->status, tx->state);
        transition_tx(tx, DS_TX_FAILED);
        ds_tx_complete(tx, DS_TX_RESULT_INTERNAL, 0);
        break;
    }
}

/* ── Public API: save ───────────────────────────────────────────────── */

esp_err_t device_settings_save(const char *device_id,
                               const ds_change_request_t *changes,
                               uint16_t change_count,
                               uint32_t expected_config_rev,
                               uint64_t *out_transaction_id,
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
        DS_DIAG_INC(tx_conflict);
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
    tx->transaction_id = next_transaction_id();
    if (out_transaction_id != NULL) *out_transaction_id = tx->transaction_id;
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

    publish_tx(tx); /* queued, before validation/dispatch begins */

    /* Prevalidate all changes against schema. */
    transition_tx(tx, DS_TX_PREVALIDATING);
    for (uint16_t i = 0; i < change_count; i++) {
        if (!prevalidate_change(rec->schema, &tx->changes[i])) {
            ESP_LOGW(TAG, "[%s] prevalidate FAIL at change %u id='%s'",
                     device_id, (unsigned)i, tx->changes[i].setting_id);
            DS_DIAG_INC(tx_prevalidate_fail);
            ds_tx_free(tx);
            return ESP_ERR_INVALID_ARG;
        }
    }

    ESP_LOGI(TAG, "[TX_BEGIN] device=%s tx_id=%llu changes=%u config_rev=%lu",
             device_id, (unsigned long long)tx->transaction_id, (unsigned)change_count,
             (unsigned long)expected_config_rev);

    /* The actor owns the first transition and lease acquisition. */
    device_command_request_t req = {0};
    req.origin = DEVICE_CMD_ORIGIN_SETTINGS;
    strlcpy(req.device_id, device_id, sizeof(req.device_id));
    strlcpy(req.command, GW_SETTINGS_CMD_TX_BEGIN, sizeof(req.command));
    req.settings.has_transaction_id = true;
    req.settings.transaction_id = tx->transaction_id;
    req.settings.has_expected_revision = true;
    req.settings.expected_revision = expected_config_rev;

    ds_event_t ev = { .type = DS_EVENT_TRANSACTION_START,
        .generation = (uint32_t)tx->transaction_id,
        .owner_token = (uint32_t)tx->transaction_id };
    strlcpy(ev.device_id, device_id, sizeof(ev.device_id));
    (void)req; /* canonical BEGIN is built in actor begin below */
    if (!ds_events_post(&ev)) { ds_tx_free(tx); return ESP_ERR_NO_MEM; }
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

    ds_event_t ev = { .type = DS_EVENT_TRANSACTION_ABORT };
    strlcpy(ev.device_id, device_id, sizeof(ev.device_id));
    (void)ds_events_post(&ev);
    return ESP_OK;
}

void device_settings_tx_actor_begin(const char *device_id, uint32_t owner, uint32_t lease)
{
    ds_transaction_t *tx = ds_tx_find(device_id);
    if (tx == NULL) { if (lease) (void)device_control_scheduler_release_lease(lease); return; }
    tx->owner_token = owner; tx->lease_id = lease;
    device_command_request_t req = { .origin = DEVICE_CMD_ORIGIN_SETTINGS };
    strlcpy(req.device_id, device_id, sizeof(req.device_id));
    strlcpy(req.command, GW_SETTINGS_CMD_TX_BEGIN, sizeof(req.command));
    req.settings.has_transaction_id = true; req.settings.transaction_id = tx->transaction_id;
    req.settings.has_expected_revision = true; req.settings.expected_revision = tx->expected_config_rev;
    transition_tx(tx, DS_TX_BEGIN_SENT);
    if (submit_tx_command(tx, &req) != ESP_OK) ds_tx_complete(tx, DS_TX_RESULT_INTERNAL, 0);
}

void device_settings_tx_actor_abort(const char *device_id)
{
    ds_transaction_t *tx = ds_tx_find(device_id);
    if (tx == NULL) return;
    (void)device_control_scheduler_cancel_source(device_id, DEVICE_CTRL_SOURCE_SETTINGS, tx->owner_token);
    ds_tx_complete(tx, DS_TX_RESULT_CANCELLED, 0);
}

bool device_settings_tx_actor_on_disconnect(const char *device_id)
{
    ds_transaction_t *tx = ds_tx_find(device_id);
    if (tx == NULL) return false;

    if (tx->state != DS_TX_COMMIT_SENT &&
        tx->state != DS_TX_CONFIRM_SENT &&
        tx->state != DS_TX_WAITING_REBOOT) {
        return false;
    }

    if (tx->state != DS_TX_WAITING_REBOOT) {
        DS_DIAG_INC(outcome_unknown);
        mark_reconciliation_pending(tx);
    }
    release_tx_lease(tx);
    ESP_LOGI(TAG, "[REBOOT_DISCONNECT] device=%s tx_id=%llu preserved",
             tx->device_id, (unsigned long long)tx->transaction_id);
    return true;
}

void device_settings_tx_actor_timeout(const char *device_id)
{
    ds_transaction_t *tx = ds_tx_find(device_id);
    if (tx == NULL) return;
    if (tx->state != DS_TX_WAITING_REBOOT) {
        device_settings_tx_actor_abort(device_id);
        return;
    }
    DS_DIAG_INC(outcome_unknown);
    ds_tx_complete(tx, DS_TX_RESULT_OUTCOME_UNKNOWN, 0);
}

/* ── Public API: transaction status (for web layer) ─────────────────── */

bool device_settings_tx_get_status(const char *device_id,
                                   bool *out_active,
                                   ds_tx_state_t *out_state,
                                   ds_tx_result_t *out_last_result)
{
    if (device_id == NULL || device_id[0] == '\0') return false;
    if (out_active != NULL) *out_active = false;
    if (out_state != NULL) *out_state = DS_TX_IDLE;
    if (out_last_result != NULL) *out_last_result = DS_TX_RESULT_OK;

    ds_transaction_t *tx = ds_tx_find(device_id);
    if (tx != NULL) {
        if (out_active != NULL) *out_active = true;
        if (out_state != NULL) *out_state = tx->state;
        return true;
    }

    /* No active transaction — return last completed result. */
    for (int i = 0; i < DEVICE_SETTINGS_MAX_DEVICES; i++) {
        if (s_tx_results[i].used &&
            strcmp(s_tx_results[i].device_id, device_id) == 0) {
            if (out_last_result != NULL) {
                *out_last_result = s_tx_results[i].result;
            }
            return true;
        }
    }

    return false;
}
