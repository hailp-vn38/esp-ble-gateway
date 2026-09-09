#include "device_state_internal.h"

#include <string.h>

#include "device_control_scheduler.h"
#include "device_schema.h"
#include "device_store.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#define STATE_SEED_MAX_RETRIES 1

typedef struct {
    bool used;
    device_id_t device_id;
    uint32_t schema_revision;
    device_control_owner_token_t owner_token;
    size_t next_feature_index;
    device_control_job_id_t active_job_id;
    uint8_t retry_count;
    bool job_active;
    bool active;
} state_seed_session_t;

typedef struct {
    bool in_use;
    device_id_t device_id;
    device_control_owner_token_t owner_token;
} state_seed_context_t;

static const char *TAG = "state_seed";
static state_seed_session_t s_sessions[DEVICE_STORE_MAX_DEVICES];
static state_seed_context_t s_contexts[DEVICE_STORE_MAX_DEVICES];
static uint32_t s_next_owner;
static portMUX_TYPE s_seed_lock = portMUX_INITIALIZER_UNLOCKED;

static state_seed_session_t *find_session_locked(const char *device_id, bool create)
{
    for (size_t i = 0; i < DEVICE_STORE_MAX_DEVICES; i++) {
        if (s_sessions[i].used && strcmp(s_sessions[i].device_id, device_id) == 0) {
            return &s_sessions[i];
        }
    }
    if (!create) return NULL;
    for (size_t i = 0; i < DEVICE_STORE_MAX_DEVICES; i++) {
        if (!s_sessions[i].used) {
            s_sessions[i].used = true;
            strlcpy(s_sessions[i].device_id, device_id, sizeof(s_sessions[i].device_id));
            return &s_sessions[i];
        }
    }
    return NULL;
}

static state_seed_context_t *find_context_locked(const char *device_id)
{
    for (size_t i = 0; i < DEVICE_STORE_MAX_DEVICES; i++) {
        if (!s_contexts[i].in_use || strcmp(s_contexts[i].device_id, device_id) == 0) {
            return &s_contexts[i];
        }
    }
    return NULL;
}

static void state_seed_start_next(const char *device_id);

static void state_seed_completion(const device_control_result_t *result, void *context)
{
    state_seed_context_t *seed_context = context;
    if (seed_context == NULL) return;

    char device_id[GW_MSG_DEVICE_ID_LEN] = {0};
    device_control_owner_token_t owner = 0;
    bool retry = false;
    taskENTER_CRITICAL(&s_seed_lock);
    strlcpy(device_id, seed_context->device_id, sizeof(device_id));
    owner = seed_context->owner_token;
    seed_context->in_use = false;
    state_seed_session_t *session = find_session_locked(device_id, false);
    if (session != NULL && session->owner_token == owner) {
        session->job_active = false;
        session->active_job_id = 0;
        if (result != NULL && result->terminal == DEVICE_CTRL_TERMINAL_COMMAND &&
            result->command_result.status != DEVICE_CMD_STATUS_OK &&
            result->command_result.status != DEVICE_CMD_STATUS_NOT_CONNECTED &&
            session->retry_count < STATE_SEED_MAX_RETRIES) {
            session->retry_count++;
            if (session->next_feature_index > 0) session->next_feature_index--;
            retry = true;
        } else {
            session->retry_count = 0;
        }
    }
    taskEXIT_CRITICAL(&s_seed_lock);

    if (retry) ESP_LOGD(TAG, "[%s] retrying seed read", device_id);
    /* This also resumes a newer revision after an old cancelled completion. */
    state_seed_start_next(device_id);
}

static void state_seed_start_next(const char *device_id)
{
    for (size_t attempts = 0; attempts < DEVICE_SCHEMA_MAX_FEATURES; attempts++) {
        uint32_t revision;
        size_t index;
        device_control_owner_token_t owner;
        taskENTER_CRITICAL(&s_seed_lock);
        state_seed_session_t *session = find_session_locked(device_id, false);
        if (session == NULL || !session->active || session->job_active) {
            taskEXIT_CRITICAL(&s_seed_lock);
            return;
        }
        revision = session->schema_revision;
        owner = session->owner_token;
        index = session->next_feature_index;
        taskEXIT_CRITICAL(&s_seed_lock);

        device_schema_feature_ref_t feature = {0};
        esp_err_t ref_err = device_schema_get_feature_at(device_id, revision, index, &feature);
        if (ref_err != ESP_OK) {
            taskENTER_CRITICAL(&s_seed_lock);
            session = find_session_locked(device_id, false);
            if (session != NULL && session->owner_token == owner) session->active = false;
            taskEXIT_CRITICAL(&s_seed_lock);
            return;
        }

        taskENTER_CRITICAL(&s_seed_lock);
        session = find_session_locked(device_id, false);
        if (session == NULL || !session->active || session->owner_token != owner) {
            taskEXIT_CRITICAL(&s_seed_lock);
            return;
        }
        session->next_feature_index = index + 1;
        if (!feature.readable) {
            taskEXIT_CRITICAL(&s_seed_lock);
            continue;
        }
        state_seed_context_t *seed_context = find_context_locked(device_id);
        if (seed_context == NULL || seed_context->in_use) {
            session->active = false;
            taskEXIT_CRITICAL(&s_seed_lock);
            ESP_LOGW(TAG, "[%s] seed context pool exhausted", device_id);
            return;
        }
        seed_context->in_use = true;
        seed_context->owner_token = owner;
        strlcpy(seed_context->device_id, device_id, sizeof(seed_context->device_id));
        session->job_active = true;
        taskEXIT_CRITICAL(&s_seed_lock);

        device_control_job_t job = {0};
        job.priority = DEVICE_CTRL_PRIORITY_BACKGROUND;
        job.source = DEVICE_CTRL_SOURCE_STATE;
        job.owner_token = owner;
        job.dedupe = DEVICE_CTRL_DEDUPE_DROP_IF_EXISTS;
        job.dedupe_hash = 0x53544154u;
        job.request.origin = DEVICE_CMD_ORIGIN_STATE_READ;
        strlcpy(job.request.device_id, device_id, sizeof(job.request.device_id));
        strlcpy(job.request.command, "read_feature_state", sizeof(job.request.command));
        strlcpy(job.request.feature_id, feature.feature_id, sizeof(job.request.feature_id));
        job.request.has_feature_id = true;
        job.request.property_id = feature.property_id;
        job.request.has_property_id = true;

        device_control_job_id_t job_id = 0;
        esp_err_t submit_err = device_control_scheduler_submit(&job, state_seed_completion,
                                                                seed_context, &job_id);
        taskENTER_CRITICAL(&s_seed_lock);
        session = find_session_locked(device_id, false);
        if (submit_err != ESP_OK) {
            seed_context->in_use = false;
            if (session != NULL && session->owner_token == owner) {
                session->job_active = false;
                session->active = false;
            }
        } else if (session != NULL && session->owner_token == owner) {
            session->active_job_id = job_id;
        }
        taskEXIT_CRITICAL(&s_seed_lock);
        if (submit_err != ESP_OK) {
            ESP_LOGD(TAG, "[%s] scheduler rejected state seed: %s", device_id,
                     esp_err_to_name(submit_err));
        }
        return;
    }
}

static void state_seed_on_schema_commit(const char *device_id, uint32_t revision,
                                        void *context)
{
    (void)context;
    device_control_owner_token_t old_owner = 0;
    taskENTER_CRITICAL(&s_seed_lock);
    state_seed_session_t *session = find_session_locked(device_id, true);
    if (session == NULL) {
        taskEXIT_CRITICAL(&s_seed_lock);
        ESP_LOGW(TAG, "[%s] state seed session pool exhausted", device_id);
        return;
    }
    old_owner = session->job_active ? session->owner_token : 0;
    session->schema_revision = revision;
    s_next_owner++;
    if (s_next_owner == 0) {
        s_next_owner++;
    }
    session->owner_token = s_next_owner;
    session->next_feature_index = 0;
    session->active_job_id = 0;
    session->retry_count = 0;
    session->active = true;
    taskEXIT_CRITICAL(&s_seed_lock);

    if (old_owner != 0) {
        (void)device_control_scheduler_cancel_source(device_id, DEVICE_CTRL_SOURCE_STATE,
                                                      old_owner);
    } else {
        state_seed_start_next(device_id);
    }
}

esp_err_t device_state_seed_init(void)
{
    return device_schema_register_commit_listener3(state_seed_on_schema_commit, NULL);
}

void device_state_seed_forget(const char *device_id)
{
    if (device_id == NULL) return;
    device_control_owner_token_t owner = 0;
    taskENTER_CRITICAL(&s_seed_lock);
    state_seed_session_t *session = find_session_locked(device_id, false);
    if (session != NULL) {
        owner = session->owner_token;
        memset(session, 0, sizeof(*session));
    }
    taskEXIT_CRITICAL(&s_seed_lock);
    if (owner != 0) {
        (void)device_control_scheduler_cancel_source(device_id, DEVICE_CTRL_SOURCE_STATE, owner);
    }
}

void device_state_seed_reset_for_test(void)
{
    taskENTER_CRITICAL(&s_seed_lock);
    memset(s_sessions, 0, sizeof(s_sessions));
    memset(s_contexts, 0, sizeof(s_contexts));
    s_next_owner = 0;
    taskEXIT_CRITICAL(&s_seed_lock);
}
