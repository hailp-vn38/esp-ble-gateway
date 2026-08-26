#include "device_capabilities.h"

#include <ctype.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "device_store.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#define CAP_EVENT_QUEUE_DEPTH 32
#define CAP_WORKER_STACK 6144
#define CAP_WORKER_PRIORITY (tskIDLE_PRIORITY + 3)
#define CAP_STORE_SCHEMA_VERSION 1
#define CAP_NVS_NAMESPACE "dev_caps"

static const char *TAG = "device_caps";

typedef struct {
    bool used;
    bool has_committed;
    bool attempted_session;
    bool discovery_pending;
    device_capability_snapshot_t committed;
    bool staging_active;
    uint16_t staging_expected;
    device_capability_snapshot_t staging;
} capability_record_t;

typedef struct {
    uint8_t schema_version;
    uint8_t count;
    uint16_t reserved;
    char device_id[GW_MSG_DEVICE_ID_LEN];
    uint32_t revision;
    uint32_t snapshot_id;
    device_capability_t items[DEVICE_CAP_MAX_PER_DEVICE];
} persisted_snapshot_t;

typedef enum {
    CAP_EVENT_READY = 0,
    CAP_EVENT_REFRESH,
    CAP_EVENT_DISCONNECT,
    CAP_EVENT_NOTIFY,
    CAP_EVENT_COMPLETION,
} capability_event_type_t;

typedef struct {
    capability_event_type_t type;
    char device_id[GW_MSG_DEVICE_ID_LEN];
    gw_message_t message;
    device_cap_submit_result_t completion;
} capability_event_t;

typedef struct {
    char device_id[GW_MSG_DEVICE_ID_LEN];
} completion_context_t;

static capability_record_t s_records[DEVICE_STORE_MAX_DEVICES];
static SemaphoreHandle_t s_mutex;
static QueueHandle_t s_queue;
static TaskHandle_t s_worker;
static device_cap_submit_fn s_submitter;
static bool s_initialized;
static bool s_discovery_active;

static bool lock_records(void);
static void unlock_records(void);
static void start_discovery(const char *device_id, bool automatic);

static void start_next_pending(void)
{
    char device_id[GW_MSG_DEVICE_ID_LEN] = {0};
    if (!lock_records()) return;
    if (!s_discovery_active) {
        for (int i = 0; i < DEVICE_STORE_MAX_DEVICES; i++) {
            if (!s_records[i].used || !s_records[i].discovery_pending) continue;
            strlcpy(device_id, s_records[i].committed.device_id,
                    sizeof(device_id));
            break;
        }
    }
    unlock_records();
    if (device_id[0] != '\0') start_discovery(device_id, false);
}

static bool lock_records(void)
{
    return s_mutex != NULL &&
           xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1000)) == pdTRUE;
}

static void unlock_records(void)
{
    xSemaphoreGive(s_mutex);
}

static int find_record_locked(const char *device_id)
{
    for (int i = 0; i < DEVICE_STORE_MAX_DEVICES; i++) {
        if (s_records[i].used &&
            strcmp(s_records[i].committed.device_id, device_id) == 0) {
            return i;
        }
    }
    return -1;
}

static int find_or_create_record_locked(const char *device_id)
{
    int existing = find_record_locked(device_id);
    if (existing >= 0) return existing;

    for (int i = 0; i < DEVICE_STORE_MAX_DEVICES; i++) {
        if (s_records[i].used) continue;
        memset(&s_records[i], 0, sizeof(s_records[i]));
        s_records[i].used = true;
        strlcpy(s_records[i].committed.device_id, device_id,
                sizeof(s_records[i].committed.device_id));
        s_records[i].committed.state = DEVICE_CAP_STATE_UNKNOWN;
        return i;
    }
    return -1;
}

static bool valid_command_name(const char *command)
{
    size_t length = strnlen(command, GW_MSG_COMMAND_LEN);
    if (length == 0 || length >= GW_MSG_COMMAND_LEN) return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)command[i];
        if (!isalnum(c) && c != '_' && c != '-' && c != '.') return false;
    }
    return true;
}

static bool valid_capability(const device_capability_t *capability)
{
    if (!valid_command_name(capability->command) ||
        capability->value_type > DEVICE_CAP_VALUE_INT) {
        return false;
    }
    if (capability->value_type == DEVICE_CAP_VALUE_INT) {
        if (capability->min_value > capability->max_value ||
            capability->step == 0) {
            return false;
        }
    }
    return true;
}

static void capability_nvs_key(int index, char key[8])
{
    unsigned bounded = (unsigned)index % DEVICE_STORE_MAX_DEVICES;
    snprintf(key, 8, "cap%02u", bounded);
}

static esp_err_t persist_record(int index,
                                const device_capability_snapshot_t *snapshot)
{
    persisted_snapshot_t persisted = {
        .schema_version = CAP_STORE_SCHEMA_VERSION,
        .count = (uint8_t)snapshot->count,
        .revision = snapshot->revision,
        .snapshot_id = snapshot->snapshot_id,
    };
    strlcpy(persisted.device_id, snapshot->device_id,
            sizeof(persisted.device_id));
    if (snapshot->count > 0) {
        memcpy(persisted.items, snapshot->items,
               snapshot->count * sizeof(snapshot->items[0]));
    }

    nvs_handle_t handle;
    esp_err_t error = nvs_open(CAP_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) return error;
    char key[8];
    capability_nvs_key(index, key);
    size_t blob_size = offsetof(persisted_snapshot_t, items) +
                       snapshot->count * sizeof(persisted.items[0]);
    error = nvs_set_blob(handle, key, &persisted, blob_size);
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    return error;
}

static void load_persisted(void)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open(CAP_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Could not open capability NVS: %s",
                 esp_err_to_name(error));
        return;
    }

    for (int i = 0; i < DEVICE_STORE_MAX_DEVICES; i++) {
        char key[8];
        capability_nvs_key(i, key);
        persisted_snapshot_t persisted;
        memset(&persisted, 0, sizeof(persisted));
        size_t length = 0;
        error = nvs_get_blob(handle, key, NULL, &length);
        if (error == ESP_ERR_NVS_NOT_FOUND) continue;
        if (error != ESP_OK || length < offsetof(persisted_snapshot_t, items) ||
            length > sizeof(persisted)) {
            ESP_LOGW(TAG, "Ignoring invalid capability record %s", key);
            continue;
        }
        error = nvs_get_blob(handle, key, &persisted, &length);
        size_t expected_length = offsetof(persisted_snapshot_t, items) +
                                 persisted.count * sizeof(persisted.items[0]);
        if (error != ESP_OK || length != expected_length ||
            persisted.schema_version != CAP_STORE_SCHEMA_VERSION ||
            persisted.count > DEVICE_CAP_MAX_PER_DEVICE ||
            persisted.device_id[0] == '\0') {
            ESP_LOGW(TAG, "Ignoring invalid capability record %s", key);
            continue;
        }

        device_entry_t device;
        if (device_store_get(persisted.device_id, &device) != DEVICE_STORE_OK) {
            continue;
        }

        bool valid = true;
        for (size_t item = 0; item < persisted.count; item++) {
            if (!valid_capability(&persisted.items[item])) {
                valid = false;
                break;
            }
        }
        if (!valid) continue;

        capability_record_t *record = &s_records[i];
        memset(record, 0, sizeof(*record));
        record->used = true;
        record->has_committed = true;
        strlcpy(record->committed.device_id, persisted.device_id,
                sizeof(record->committed.device_id));
        record->committed.state = DEVICE_CAP_STATE_STALE;
        record->committed.revision = persisted.revision;
        record->committed.snapshot_id = persisted.snapshot_id;
        record->committed.count = persisted.count;
        memcpy(record->committed.items, persisted.items,
               persisted.count * sizeof(persisted.items[0]));
    }
    nvs_close(handle);
}

static void set_failure_state_locked(capability_record_t *record,
                                     device_cap_state_t empty_state)
{
    record->staging_active = false;
    memset(&record->staging, 0, sizeof(record->staging));
    record->committed.state =
        record->has_committed ? DEVICE_CAP_STATE_STALE : empty_state;
}

static void discovery_done(device_cap_submit_result_t result, void *context)
{
    completion_context_t *done_context = context;
    capability_event_t event = {
        .type = CAP_EVENT_COMPLETION,
        .completion = result,
    };
    if (done_context != NULL) {
        strlcpy(event.device_id, done_context->device_id,
                sizeof(event.device_id));
        free(done_context);
    }
    if (s_queue == NULL ||
        xQueueSend(s_queue, &event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Dropping discovery completion for %s", event.device_id);
        if (lock_records()) {
            int index = find_record_locked(event.device_id);
            if (index >= 0) {
                set_failure_state_locked(&s_records[index],
                                         DEVICE_CAP_STATE_ERROR);
            }
            s_discovery_active = false;
            unlock_records();
        }
        start_next_pending();
    }
}

static void start_discovery(const char *device_id, bool automatic)
{
    if (!lock_records()) return;
    int index = find_or_create_record_locked(device_id);
    if (index < 0) {
        unlock_records();
        return;
    }
    capability_record_t *record = &s_records[index];
    bool was_pending = record->discovery_pending;
    if ((automatic && record->attempted_session && !was_pending) ||
        (record->committed.state == DEVICE_CAP_STATE_DISCOVERING &&
         !was_pending)) {
        unlock_records();
        return;
    }
    record->attempted_session = true;
    if (s_discovery_active) {
        record->discovery_pending = true;
        record->committed.state = DEVICE_CAP_STATE_DISCOVERING;
        unlock_records();
        return;
    }
    s_discovery_active = true;
    record->discovery_pending = false;
    record->staging_active = false;
    record->committed.state = DEVICE_CAP_STATE_DISCOVERING;
    unlock_records();

    if (s_submitter == NULL) {
        if (lock_records()) {
            set_failure_state_locked(record, DEVICE_CAP_STATE_ERROR);
            s_discovery_active = false;
            unlock_records();
        }
        start_next_pending();
        return;
    }

    completion_context_t *context = calloc(1, sizeof(*context));
    if (context == NULL) {
        if (lock_records()) {
            index = find_record_locked(device_id);
            if (index >= 0) {
                set_failure_state_locked(&s_records[index],
                                         DEVICE_CAP_STATE_ERROR);
            }
            s_discovery_active = false;
            unlock_records();
        }
        start_next_pending();
        return;
    }
    strlcpy(context->device_id, device_id, sizeof(context->device_id));

    gw_message_t query = {
        .protocol_version = GW_PROTOCOL_VERSION,
        .has_device_id = 1,
    };
    strlcpy(query.type, "device_command", sizeof(query.type));
    strlcpy(query.device_id, device_id, sizeof(query.device_id));
    strlcpy(query.command, DEVICE_CAP_RESERVED_COMMAND, sizeof(query.command));

    esp_err_t error = s_submitter(&query, discovery_done, context);
    if (error != ESP_OK) {
        free(context);
        if (lock_records()) {
            index = find_record_locked(device_id);
            if (index >= 0) {
                set_failure_state_locked(&s_records[index],
                                         DEVICE_CAP_STATE_ERROR);
            }
            s_discovery_active = false;
            unlock_records();
        }
        start_next_pending();
    }
}

static bool message_device_matches(const char *device_id,
                                   const gw_message_t *message)
{
    return message->protocol_version >= 3 && message->has_device_id &&
           strcmp(device_id, message->device_id) == 0;
}

static void handle_begin(const char *device_id, const gw_message_t *message)
{
    if (!message_device_matches(device_id, message) ||
        !message->has_snapshot_id || !message->has_total ||
        !message->has_capability_revision ||
        message->total > DEVICE_CAP_MAX_PER_DEVICE) {
        return;
    }
    if (!lock_records()) return;
    int index = find_or_create_record_locked(device_id);
    if (index >= 0) {
        capability_record_t *record = &s_records[index];
        memset(&record->staging, 0, sizeof(record->staging));
        strlcpy(record->staging.device_id, device_id,
                sizeof(record->staging.device_id));
        record->staging.state = DEVICE_CAP_STATE_DISCOVERING;
        record->staging.snapshot_id = message->snapshot_id;
        record->staging.revision = message->capability_revision;
        record->staging_expected = message->total;
        record->staging_active = true;
    }
    unlock_records();
}

static void handle_item(const char *device_id, const gw_message_t *message)
{
    if (!message_device_matches(device_id, message) ||
        !message->has_snapshot_id || !message->has_sequence ||
        !message->has_value_type) {
        return;
    }

    device_capability_t item = {
        .value_type = (device_cap_value_type_t)message->value_type,
        .flags = message->has_capability_flags
                     ? message->capability_flags
                     : 0,
        .min_value = message->min_value,
        .max_value = message->max_value,
        .step = message->step,
    };
    strlcpy(item.command, message->command, sizeof(item.command));
    strlcpy(item.label,
            message->capability_label[0] != '\0'
                ? message->capability_label
                : message->command,
            sizeof(item.label));
    strlcpy(item.unit, message->capability_unit, sizeof(item.unit));

    if (item.value_type == DEVICE_CAP_VALUE_INT &&
        (!message->has_min_value || !message->has_max_value ||
         !message->has_step)) {
        return;
    }
    if (!valid_capability(&item)) return;

    if (!lock_records()) return;
    int index = find_record_locked(device_id);
    if (index >= 0) {
        capability_record_t *record = &s_records[index];
        size_t next = record->staging.count;
        bool valid_sequence =
            record->staging_active &&
            record->staging.snapshot_id == message->snapshot_id &&
            message->sequence == next && next < record->staging_expected;
        if (valid_sequence) {
            for (size_t i = 0; i < next; i++) {
                if (strcmp(record->staging.items[i].command,
                           item.command) == 0) {
                    valid_sequence = false;
                    break;
                }
            }
        }
        if (valid_sequence) {
            record->staging.items[next] = item;
            record->staging.count++;
        } else {
            set_failure_state_locked(record, DEVICE_CAP_STATE_ERROR);
        }
    }
    unlock_records();
}

static void handle_end(const char *device_id, const gw_message_t *message)
{
    if (!message_device_matches(device_id, message) ||
        !message->has_snapshot_id || !message->has_total) {
        return;
    }

    device_capability_snapshot_t committed;
    int persist_index = -1;
    if (!lock_records()) return;
    int index = find_record_locked(device_id);
    if (index >= 0) {
        capability_record_t *record = &s_records[index];
        bool complete = record->staging_active &&
                        record->staging.snapshot_id == message->snapshot_id &&
                        message->total == record->staging_expected &&
                        record->staging.count == record->staging_expected;
        if (complete) {
            record->staging.state = DEVICE_CAP_STATE_READY;
            record->staging.updated_at_ms = esp_timer_get_time() / 1000;
            record->committed = record->staging;
            record->has_committed = true;
            record->staging_active = false;
            committed = record->committed;
            persist_index = index;
        } else {
            set_failure_state_locked(record, DEVICE_CAP_STATE_ERROR);
        }
    }
    unlock_records();

    if (persist_index >= 0) {
        esp_err_t error = persist_record(persist_index, &committed);
        if (error != ESP_OK) {
            ESP_LOGW(TAG, "Could not persist capabilities for %s: %s",
                     device_id, esp_err_to_name(error));
        } else {
            ESP_LOGI(TAG, "[%s] committed %u capabilities (revision=%lu)",
                     device_id, (unsigned)committed.count,
                     (unsigned long)committed.revision);
        }
    }
}

static void handle_completion(const char *device_id,
                              device_cap_submit_result_t completion)
{
    if (!lock_records()) return;
    int index = find_record_locked(device_id);
    if (index >= 0) {
        capability_record_t *record = &s_records[index];
        if (completion == DEVICE_CAP_SUBMIT_OK && record->has_committed &&
            record->committed.state == DEVICE_CAP_STATE_READY) {
            // Snapshot was committed before its final ACK.
        } else if (completion == DEVICE_CAP_SUBMIT_REJECTED ||
                   completion == DEVICE_CAP_SUBMIT_TIMEOUT) {
            set_failure_state_locked(record, DEVICE_CAP_STATE_UNSUPPORTED);
        } else {
            set_failure_state_locked(record, DEVICE_CAP_STATE_ERROR);
        }
    }
    s_discovery_active = false;
    unlock_records();
    start_next_pending();
}

static void capability_worker(void *arg)
{
    (void)arg;
    capability_event_t event;
    for (;;) {
        if (xQueueReceive(s_queue, &event, portMAX_DELAY) != pdTRUE) continue;
        switch (event.type) {
        case CAP_EVENT_READY:
            start_discovery(event.device_id, true);
            break;
        case CAP_EVENT_REFRESH:
            start_discovery(event.device_id, false);
            break;
        case CAP_EVENT_DISCONNECT:
            if (lock_records()) {
                int index = find_record_locked(event.device_id);
                if (index >= 0) {
                    capability_record_t *record = &s_records[index];
                    record->attempted_session = false;
                    record->discovery_pending = false;
                    set_failure_state_locked(record, DEVICE_CAP_STATE_UNKNOWN);
                }
                unlock_records();
            }
            break;
        case CAP_EVENT_NOTIFY:
            if (strcmp(event.message.type, "capabilities_begin") == 0) {
                handle_begin(event.device_id, &event.message);
            } else if (strcmp(event.message.type, "capability_item") == 0) {
                handle_item(event.device_id, &event.message);
            } else {
                handle_end(event.device_id, &event.message);
            }
            break;
        case CAP_EVENT_COMPLETION:
            handle_completion(event.device_id, event.completion);
            break;
        }
    }
}

esp_err_t device_capabilities_init(void)
{
    if (s_initialized) return ESP_ERR_INVALID_STATE;
    if (s_mutex == NULL) s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) return ESP_ERR_NO_MEM;
    memset(s_records, 0, sizeof(s_records));
    s_discovery_active = false;
    load_persisted();

    s_queue = xQueueCreate(CAP_EVENT_QUEUE_DEPTH, sizeof(capability_event_t));
    if (s_queue == NULL) return ESP_ERR_NO_MEM;
    if (xTaskCreate(capability_worker, "device_caps", CAP_WORKER_STACK, NULL,
                    CAP_WORKER_PRIORITY, &s_worker) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_initialized = true;
    return ESP_OK;
}

void device_capabilities_set_submitter(device_cap_submit_fn submitter)
{
    s_submitter = submitter;
}

static esp_err_t enqueue_device_event(capability_event_type_t type,
                                      const char *device_id)
{
    if (!s_initialized || s_queue == NULL) return ESP_ERR_INVALID_STATE;
    if (device_id == NULL || device_id[0] == '\0' ||
        strnlen(device_id, GW_MSG_DEVICE_ID_LEN) >= GW_MSG_DEVICE_ID_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    capability_event_t event = {.type = type};
    strlcpy(event.device_id, device_id, sizeof(event.device_id));
    return xQueueSend(s_queue, &event, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t device_capabilities_on_ready(const char *device_id)
{
    return enqueue_device_event(CAP_EVENT_READY, device_id);
}

void device_capabilities_on_disconnect(const char *device_id)
{
    enqueue_device_event(CAP_EVENT_DISCONNECT, device_id);
}

bool device_capabilities_on_notify(const char *device_id,
                                   const gw_message_t *message)
{
    if (message == NULL ||
        (strcmp(message->type, "capabilities_begin") != 0 &&
         strcmp(message->type, "capability_item") != 0 &&
         strcmp(message->type, "capabilities_end") != 0)) {
        return false;
    }
    if (!s_initialized || device_id == NULL) return true;
    capability_event_t event = {.type = CAP_EVENT_NOTIFY, .message = *message};
    strlcpy(event.device_id, device_id, sizeof(event.device_id));
    if (xQueueSend(s_queue, &event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "[%s] capability queue full", device_id);
    }
    return true;
}

esp_err_t device_capabilities_refresh(const char *device_id)
{
    device_entry_t entry;
    if (device_store_get(device_id, &entry) != DEVICE_STORE_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    return enqueue_device_event(CAP_EVENT_REFRESH, device_id);
}

esp_err_t device_capabilities_get(const char *device_id,
                                  device_capability_snapshot_t *out_snapshot)
{
    if (device_id == NULL || out_snapshot == NULL) return ESP_ERR_INVALID_ARG;
    device_entry_t entry;
    if (device_store_get(device_id, &entry) != DEVICE_STORE_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    memset(out_snapshot, 0, sizeof(*out_snapshot));
    strlcpy(out_snapshot->device_id, device_id,
            sizeof(out_snapshot->device_id));
    out_snapshot->state = DEVICE_CAP_STATE_UNKNOWN;
    if (!lock_records()) return ESP_ERR_TIMEOUT;
    int index = find_record_locked(device_id);
    if (index >= 0) *out_snapshot = s_records[index].committed;
    unlock_records();
    return ESP_OK;
}

device_cap_validation_t device_capabilities_validate_command(
    const gw_message_t *message, device_capability_t *out_capability)
{
    if (message == NULL || !message->has_device_id ||
        message->command[0] == '\0') {
        return DEVICE_CAP_VALID_ARGUMENT;
    }
    if (strcmp(message->command, DEVICE_CAP_RESERVED_COMMAND) == 0) {
        return DEVICE_CAP_VALID;
    }
    if (!lock_records()) return DEVICE_CAP_VALID_UNKNOWN;
    int index = find_record_locked(message->device_id);
    if (index < 0 || !s_records[index].has_committed ||
        (s_records[index].committed.state != DEVICE_CAP_STATE_READY &&
         s_records[index].committed.state != DEVICE_CAP_STATE_STALE &&
         s_records[index].committed.state != DEVICE_CAP_STATE_DISCOVERING)) {
        unlock_records();
        return DEVICE_CAP_VALID_UNKNOWN;
    }

    const device_capability_t *capability = NULL;
    for (size_t i = 0; i < s_records[index].committed.count; i++) {
        if (strcmp(s_records[index].committed.items[i].command,
                   message->command) == 0) {
            capability = &s_records[index].committed.items[i];
            break;
        }
    }
    if (capability == NULL) {
        unlock_records();
        return DEVICE_CAP_VALID_UNSUPPORTED_COMMAND;
    }

    bool valid_argument = false;
    switch (capability->value_type) {
    case DEVICE_CAP_VALUE_NONE:
        valid_argument = !message->has_int_value && !message->has_bool_value;
        break;
    case DEVICE_CAP_VALUE_BOOL:
        valid_argument = message->has_bool_value && !message->has_int_value;
        break;
    case DEVICE_CAP_VALUE_INT: {
        int64_t delta = (int64_t)message->int_value -
                        (int64_t)capability->min_value;
        valid_argument = message->has_int_value && !message->has_bool_value &&
                         message->int_value >= capability->min_value &&
                         message->int_value <= capability->max_value &&
                         ((uint64_t)delta % capability->step == 0);
        break;
    }
    }
    if (valid_argument && out_capability != NULL) {
        *out_capability = *capability;
    }
    unlock_records();
    return valid_argument ? DEVICE_CAP_VALID : DEVICE_CAP_VALID_ARGUMENT;
}

esp_err_t device_capabilities_forget(const char *device_id)
{
    if (device_id == NULL || device_id[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (!lock_records()) return ESP_ERR_TIMEOUT;
    int index = find_record_locked(device_id);
    if (index >= 0) memset(&s_records[index], 0, sizeof(s_records[index]));
    unlock_records();
    if (index < 0) return ESP_OK;

    nvs_handle_t handle;
    esp_err_t error = nvs_open(CAP_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) return error;
    char key[8];
    capability_nvs_key(index, key);
    error = nvs_erase_key(handle, key);
    if (error == ESP_ERR_NVS_NOT_FOUND) error = ESP_OK;
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    return error;
}

const char *device_capabilities_state_name(device_cap_state_t state)
{
    switch (state) {
    case DEVICE_CAP_STATE_UNKNOWN: return "unknown";
    case DEVICE_CAP_STATE_DISCOVERING: return "discovering";
    case DEVICE_CAP_STATE_READY: return "ready";
    case DEVICE_CAP_STATE_STALE: return "stale";
    case DEVICE_CAP_STATE_UNSUPPORTED: return "unsupported";
    case DEVICE_CAP_STATE_ERROR: return "error";
    default: return "unknown";
    }
}

void device_capabilities_reset_for_test(void)
{
    if (lock_records()) {
        memset(s_records, 0, sizeof(s_records));
        s_discovery_active = false;
        unlock_records();
    }
}
