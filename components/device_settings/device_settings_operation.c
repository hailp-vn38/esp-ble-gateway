#include <string.h>

#include "device_settings.h"
#include "esp_log.h"

static const char *TAG = "ds_operation";

/* ── Operation tracking ──────────────────────────────────────────────
 * Each device has at most one active settings operation.  The operation
 * state machine tracks the lifecycle:
 *
 *   IDLE -> QUEUED -> RUNNING -> (WAITING_REBOOT) -> VERIFYING -> done
 *
 * Completion callbacks are invoked from the service task context. */

typedef struct {
    bool                  active;
    char                  device_id[GW_MSG_DEVICE_ID_LEN];
    ds_op_kind_t          kind;
    ds_op_state_t         state;
    uint32_t              op_id;
    ds_op_completion_fn   completion;
    void                 *context;
} ds_op_record_t;

static ds_op_record_t s_ops[DEVICE_SETTINGS_MAX_DEVICES];
static uint32_t       s_next_op_id = 1;

/* ── Internal helpers ──────────────────────────────────────────────── */

static ds_op_record_t *find_op(const char *device_id)
{
    for (int i = 0; i < DEVICE_SETTINGS_MAX_DEVICES; i++) {
        if (s_ops[i].active &&
            strncmp(s_ops[i].device_id, device_id,
                    GW_MSG_DEVICE_ID_LEN) == 0) {
            return &s_ops[i];
        }
    }
    return NULL;
}

static ds_op_record_t *alloc_op(void)
{
    for (int i = 0; i < DEVICE_SETTINGS_MAX_DEVICES; i++) {
        if (!s_ops[i].active) {
            memset(&s_ops[i], 0, sizeof(s_ops[i]));
            s_ops[i].active = true;
            s_ops[i].op_id = s_next_op_id++;
            return &s_ops[i];
        }
    }
    return NULL;
}

static void complete_op(ds_op_record_t *op, ds_op_result_t result)
{
    if (op == NULL) return;
    ds_op_completion_fn cb = op->completion;
    void *ctx = op->context;
    op->active = false;
    op->state = DS_OP_IDLE;
    if (cb != NULL) {
        cb(result, ctx);
    }
}

/* ── Public API ────────────────────────────────────────────────────── */

esp_err_t device_settings_describe(const char *device_id,
                                   ds_op_completion_fn completion,
                                   void *context)
{
    if (device_id == NULL || device_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    /* Check if a settings operation is already active. */
    if (find_op(device_id) != NULL) {
        ESP_LOGW(TAG, "[%s] describe: already active", device_id);
        return ESP_ERR_INVALID_STATE;
    }

    /* Check schema state. */
    ds_schema_state_t state;
    device_settings_get_state(device_id, &state);
    if (state == DS_SCHEMA_UNSUPPORTED) {
        ESP_LOGW(TAG, "[%s] describe: settings unsupported", device_id);
        return ESP_ERR_NOT_SUPPORTED;
    }

    ds_op_record_t *op = alloc_op();
    if (op == NULL) {
        ESP_LOGE(TAG, "[%s] describe: no free operation slot", device_id);
        return ESP_ERR_NO_MEM;
    }

    strlcpy(op->device_id, device_id, sizeof(op->device_id));
    op->kind = DS_OP_DESCRIBE;
    op->state = DS_OP_QUEUED;
    op->completion = completion;
    op->context = context;

    ESP_LOGI(TAG, "[%s] DESCRIBE queued op_id=%lu",
             device_id, (unsigned long)op->op_id);
    return ESP_OK;
}

esp_err_t device_settings_get(const char *device_id,
                              ds_op_completion_fn completion,
                              void *context)
{
    if (device_id == NULL || device_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    if (find_op(device_id) != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    ds_schema_state_t state;
    device_settings_get_state(device_id, &state);
    if (state != DS_SCHEMA_READY) {
        return ESP_ERR_INVALID_STATE;
    }

    ds_op_record_t *op = alloc_op();
    if (op == NULL) return ESP_ERR_NO_MEM;

    strlcpy(op->device_id, device_id, sizeof(op->device_id));
    op->kind = DS_OP_GET;
    op->state = DS_OP_QUEUED;
    op->completion = completion;
    op->context = context;

    ESP_LOGI(TAG, "[%s] GET queued op_id=%lu",
             device_id, (unsigned long)op->op_id);
    return ESP_OK;
}

esp_err_t device_settings_cancel(const char *device_id)
{
    if (device_id == NULL || device_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    ds_op_record_t *op = find_op(device_id);
    if (op == NULL) return ESP_ERR_NOT_FOUND;

    complete_op(op, DS_OP_RESULT_INTERNAL);
    ESP_LOGI(TAG, "[%s] operation cancelled", device_id);
    return ESP_OK;
}

/* ── Reset (test only) ────────────────────────────────────────────── */

void device_settings_operation_reset_for_test(void)
{
    for (int i = 0; i < DEVICE_SETTINGS_MAX_DEVICES; i++) {
        s_ops[i].active = false;
    }
    s_next_op_id = 1;
}
