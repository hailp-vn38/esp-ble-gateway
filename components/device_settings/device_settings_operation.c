#include <string.h>

#include "device_settings.h"
#include "device_settings_internal.h"
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
    uint32_t              generation;
    ds_op_completion_fn   completion;
    void                 *context;
} ds_op_record_t;

static ds_op_record_t s_ops[DEVICE_SETTINGS_MAX_DEVICES];
static uint32_t       s_next_op_id = 1;

/* Worker-visible arrays (extern in header). */
bool     s_ops_active[DEVICE_SETTINGS_MAX_DEVICES];
uint32_t s_ops_generation[DEVICE_SETTINGS_MAX_DEVICES];

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
            s_ops[i].generation = 1;
            s_ops_active[i] = true;
            s_ops_generation[i] = 1;
            return &s_ops[i];
        }
    }
    return NULL;
}

/* ── Worker accessor functions ─────────────────────────────────────── */

void s_ops_invoke_completion(int slot, ds_op_result_t result)
{
    if (slot < 0 || slot >= DEVICE_SETTINGS_MAX_DEVICES) return;
    ds_op_record_t *op = &s_ops[slot];
    if (!op->active) return;

    ds_op_completion_fn cb = op->completion;
    void *ctx = op->context;
    op->active = false;
    op->state = DS_OP_IDLE;
    s_ops_active[slot] = false;

    if (cb != NULL) {
        cb(result, ctx);
    }
}

const char *s_ops_get_device_id(int slot)
{
    if (slot < 0 || slot >= DEVICE_SETTINGS_MAX_DEVICES) return "";
    return s_ops[slot].device_id;
}

ds_op_kind_t s_ops_get_kind(int slot)
{
    if (slot < 0 || slot >= DEVICE_SETTINGS_MAX_DEVICES) return DS_OP_NONE;
    return s_ops[slot].kind;
}

ds_op_state_t s_ops_get_state(int slot)
{
    if (slot < 0 || slot >= DEVICE_SETTINGS_MAX_DEVICES) return DS_OP_IDLE;
    return s_ops[slot].state;
}

void s_ops_set_state(int slot, ds_op_state_t state)
{
    if (slot < 0 || slot >= DEVICE_SETTINGS_MAX_DEVICES) return;
    s_ops[slot].state = state;
}

void s_ops_set_kind(int slot, ds_op_kind_t kind)
{
    if (slot < 0 || slot >= DEVICE_SETTINGS_MAX_DEVICES) return;
    s_ops[slot].kind = kind;
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

    /* Signal worker to process. */
    device_settings_worker_submit();
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

    /* Signal worker to process. */
    device_settings_worker_submit();
    return ESP_OK;
}

esp_err_t device_settings_cancel(const char *device_id)
{
    if (device_id == NULL || device_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    ds_op_record_t *op = find_op(device_id);
    if (op == NULL) return ESP_ERR_NOT_FOUND;
    /* Before the runtime actor starts (notably schema capability setup),
     * there is no cross-task state to serialize. */
    if (!device_settings_worker_actor_running()) {
        int slot = (int)(op - s_ops);
        s_ops_invoke_completion(slot, DS_OP_RESULT_INTERNAL);
        return ESP_OK;
    }
    ds_event_t event = { .type = DS_EVENT_CANCEL_DEVICE };
    strlcpy(event.device_id, device_id, sizeof(event.device_id));
    return ds_events_post(&event) ? ESP_OK : ESP_ERR_NO_MEM;
}

/* ── Reset (test only) ────────────────────────────────────────────── */

void device_settings_operation_reset_for_test(void)
{
    for (int i = 0; i < DEVICE_SETTINGS_MAX_DEVICES; i++) {
        s_ops[i].active = false;
        s_ops_active[i] = false;
        s_ops_generation[i] = 0;
    }
    s_next_op_id = 1;
}

void device_settings_fill_all_ops_for_test(void)
{
    for (int i = 0; i < DEVICE_SETTINGS_MAX_DEVICES; i++) {
        memset(&s_ops[i], 0, sizeof(s_ops[i]));
        s_ops[i].active = true;
        s_ops[i].kind = DS_OP_DESCRIBE;
        s_ops[i].state = DS_OP_RUNNING;
        strlcpy(s_ops[i].device_id, "fill_dummy",
                sizeof(s_ops[i].device_id));
        s_ops_active[i] = true;
        s_ops_generation[i] = 1;
    }
}
