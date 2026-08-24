#ifndef COMMAND_DISPATCHER_H
#define COMMAND_DISPATCHER_H

#include "cbor_codec.h"

#define DISPATCHER_MAX_RESULT_LEN   256
#define DISPATCHER_MAX_COMMANDS      16

typedef struct {
    int  success;
    char message[DISPATCHER_MAX_RESULT_LEN];
} dispatch_result_t;

typedef void (*gateway_command_fn_t)(const gw_message_t *msg, dispatch_result_t *result);

int command_dispatcher_init(void);
int command_dispatcher_register(const char *command_name, gateway_command_fn_t fn);
void command_dispatcher_handle(const gw_message_t *msg, dispatch_result_t *result);
void command_dispatcher_on_device_notify(const char *device_id, const gw_message_t *msg);

#endif // COMMAND_DISPATCHER_H
