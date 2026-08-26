#ifndef DEVICE_CAPABILITIES_H
#define DEVICE_CAPABILITIES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cbor_codec.h"
#include "esp_err.h"

#define DEVICE_CAP_MAX_PER_DEVICE 12

#define DEVICE_CAP_RESERVED_COMMAND "describe_capabilities"

typedef enum {
    DEVICE_CAP_VALUE_NONE = 0,
    DEVICE_CAP_VALUE_BOOL = 1,
    DEVICE_CAP_VALUE_INT = 2,
} device_cap_value_type_t;

enum {
    DEVICE_CAP_FLAG_IDEMPOTENT = 1u << 0,
    DEVICE_CAP_FLAG_DESTRUCTIVE = 1u << 1,
};

typedef struct {
    char command[GW_MSG_COMMAND_LEN];
    char label[GW_MSG_CAP_LABEL_LEN];
    char unit[GW_MSG_CAP_UNIT_LEN];
    device_cap_value_type_t value_type;
    uint8_t flags;
    int32_t min_value;
    int32_t max_value;
    uint32_t step;
} device_capability_t;

typedef enum {
    DEVICE_CAP_STATE_UNKNOWN = 0,
    DEVICE_CAP_STATE_DISCOVERING,
    DEVICE_CAP_STATE_READY,
    DEVICE_CAP_STATE_STALE,
    DEVICE_CAP_STATE_UNSUPPORTED,
    DEVICE_CAP_STATE_ERROR,
} device_cap_state_t;

typedef struct {
    char device_id[GW_MSG_DEVICE_ID_LEN];
    device_cap_state_t state;
    uint32_t revision;
    uint32_t snapshot_id;
    int64_t updated_at_ms;
    size_t count;
    device_capability_t items[DEVICE_CAP_MAX_PER_DEVICE];
} device_capability_snapshot_t;

typedef enum {
    DEVICE_CAP_SUBMIT_OK = 0,
    DEVICE_CAP_SUBMIT_REJECTED,
    DEVICE_CAP_SUBMIT_TIMEOUT,
    DEVICE_CAP_SUBMIT_ERROR,
} device_cap_submit_result_t;

typedef void (*device_cap_submit_done_fn)(device_cap_submit_result_t result,
                                          void *context);
typedef esp_err_t (*device_cap_submit_fn)(const gw_message_t *message,
                                          device_cap_submit_done_fn done,
                                          void *context);

typedef enum {
    DEVICE_CAP_VALID = 0,
    DEVICE_CAP_VALID_UNKNOWN,
    DEVICE_CAP_VALID_UNSUPPORTED_COMMAND,
    DEVICE_CAP_VALID_ARGUMENT,
} device_cap_validation_t;

esp_err_t device_capabilities_init(void);
void device_capabilities_set_submitter(device_cap_submit_fn submitter);

// BLE lifecycle inputs. Both are non-blocking and safe from NimBLE callbacks.
esp_err_t device_capabilities_on_ready(const char *device_id);
void device_capabilities_on_disconnect(const char *device_id);

// Returns true when a capability begin/item/end message was recognized and
// consumed. The function only queues a copy; validation and NVS happen on the
// component worker.
bool device_capabilities_on_notify(const char *device_id,
                                   const gw_message_t *message);

esp_err_t device_capabilities_refresh(const char *device_id);
esp_err_t device_capabilities_get(const char *device_id,
                                  device_capability_snapshot_t *out_snapshot);
device_cap_validation_t device_capabilities_validate_command(
    const gw_message_t *message, device_capability_t *out_capability);
esp_err_t device_capabilities_forget(const char *device_id);

const char *device_capabilities_state_name(device_cap_state_t state);

// Test-only reset. It does not erase NVS.
void device_capabilities_reset_for_test(void);

#endif // DEVICE_CAPABILITIES_H
