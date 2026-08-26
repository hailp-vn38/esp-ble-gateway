#ifndef COMMAND_DISPATCHER_H
#define COMMAND_DISPATCHER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "cbor_codec.h"

#define DISPATCHER_MAX_RESULT_LEN  4096
#define DISPATCHER_MAX_COMMANDS      16
#define DISPATCHER_ACK_TIMEOUT_MS   2000

typedef enum {
    DISPATCH_RESULT_TEXT = 0,
    DISPATCH_RESULT_JSON,
} dispatch_result_format_t;

// status is the single source of truth for the outcome of a dispatch.
// There is deliberately no separate success flag.
typedef enum {
    DISPATCH_STATUS_OK = 0,
    DISPATCH_STATUS_INVALID_ARGUMENT,
    DISPATCH_STATUS_NOT_FOUND,
    DISPATCH_STATUS_BUSY,
    DISPATCH_STATUS_TIMEOUT,
    DISPATCH_STATUS_NOT_CONNECTED,
    DISPATCH_STATUS_TRANSPORT_ERROR,
    DISPATCH_STATUS_INTERNAL_ERROR,
    // The peripheral received and answered the command but reported failure.
    DISPATCH_STATUS_DEVICE_ERROR,
    // Appended (values preserved for on-wire numeric compatibility):
    DISPATCH_STATUS_CONFLICT,
    DISPATCH_STATUS_RESOURCE_EXHAUSTED,
    DISPATCH_STATUS_UNSUPPORTED_COMMAND,
    DISPATCH_STATUS_INVALID_COMMAND_ARGUMENT,
} dispatch_status_t;

typedef struct {
    dispatch_status_t status;
    dispatch_result_format_t format;
    char payload[DISPATCHER_MAX_RESULT_LEN];
} dispatch_result_t;

static inline bool dispatch_result_is_ok(const dispatch_result_t *result)
{
    return result != NULL && result->status == DISPATCH_STATUS_OK;
}

typedef void (*gateway_command_fn_t)(const gw_message_t *msg, dispatch_result_t *result);

// Init is single-shot: a second call returns ESP_ERR_INVALID_STATE.
int command_dispatcher_init(void);

// Fails once the registry is frozen. See command_dispatcher_freeze_registry().
int command_dispatcher_register(const char *command_name, gateway_command_fn_t fn);

// Freezes the registry: dispatching is only accepted after freeze,
// and register calls after freeze are rejected.
int command_dispatcher_freeze_registry(void);

// Copy-out API: caller owns the returned names. Returns the number copied
// or -1 on invalid arguments.
int command_dispatcher_get_registered_names(
    char out_names[][GW_MSG_COMMAND_LEN], int max_names);

bool command_dispatcher_is_registered(const char *command_name);

void command_dispatcher_handle(const gw_message_t *msg, dispatch_result_t *result);

void command_dispatcher_set_text_result(dispatch_result_t *result,
                                        dispatch_status_t status,
                                        const char *format, ...);

void command_dispatcher_set_json_result(dispatch_result_t *result,
                                        dispatch_status_t status,
                                        const char *json);

void command_dispatcher_on_device_notify(const char *device_id, const gw_message_t *msg);

#endif // COMMAND_DISPATCHER_H
