#ifndef DEVICE_REQUEST_MANAGER_H
#define DEVICE_REQUEST_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "cbor_codec.h"
#include "sdkconfig.h"

// Internal module owning pending device-command correlation.
// One pending request per device (Phase 1 invariant).
// The manager owns BLE request_id generation: IDs are monotonically
// increasing, never 0, and never reused while a matching request is pending.

typedef struct {
    bool in_use;
    bool completed;
    uint32_t request_id;
    char device_id[GW_MSG_DEVICE_ID_LEN];
    char command[GW_MSG_COMMAND_LEN];
    gw_message_t response;
    SemaphoreHandle_t semaphore;
} pending_request_t;

#define DEVICE_REQUEST_MAX_PENDING CONFIG_DEVICE_REQUEST_MAX_PENDING

int device_request_manager_init(void);

int device_request_allocate(const char *device_id, const char *command,
                            pending_request_t **out_request);

int device_request_wait(pending_request_t *request, TickType_t timeout);

// Completes a pending request only when response is a well-formed
// device_ack whose device_id and request_id match exactly.
// Returns true if a request was completed.
bool device_request_complete(const char *device_id, const gw_message_t *response);

void device_request_release(pending_request_t *request);

#endif // DEVICE_REQUEST_MANAGER_H
