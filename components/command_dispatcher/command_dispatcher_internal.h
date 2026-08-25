#ifndef COMMAND_DISPATCHER_INTERNAL_H
#define COMMAND_DISPATCHER_INTERNAL_H

#include <stdbool.h>

#include "command_dispatcher.h"

void command_dispatcher_set_result(dispatch_result_t *result, bool success,
                                   const char *format, ...);

int command_registry_init(void);
gateway_command_fn_t command_registry_find(const char *command_name);
int command_registry_count(void);

int gateway_commands_register_defaults(void);
void gateway_command_handle(const gw_message_t *msg, dispatch_result_t *result);

int device_command_init(void);
void device_command_handle(const gw_message_t *msg, dispatch_result_t *result);
void device_command_on_notify(const char *device_id, const gw_message_t *msg);

#endif // COMMAND_DISPATCHER_INTERNAL_H
