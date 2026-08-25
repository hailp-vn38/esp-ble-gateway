#ifndef COMMAND_DISPATCHER_INTERNAL_H
#define COMMAND_DISPATCHER_INTERNAL_H

#include <stdbool.h>

#include "command_dispatcher.h"

int command_registry_init(void);
int command_registry_register(const char *command_name, gateway_command_fn_t fn);
gateway_command_fn_t command_registry_find(const char *command_name);
int command_registry_get_names(char out_names[][GW_MSG_COMMAND_LEN], int max_names);
int command_registry_count(void);
int command_registry_freeze(void);
bool command_registry_is_frozen(void);

int gateway_commands_register_defaults(void);
void gateway_command_handle(const gw_message_t *msg, dispatch_result_t *result);

int device_command_init(void);
void device_command_handle(const gw_message_t *msg, dispatch_result_t *result);
void device_command_on_notify(const char *device_id, const gw_message_t *msg);

// Test-only seam: override BLE transport dependencies so ACK correlation
// can be unit tested without radio hardware. Passing NULL restores the
// real ble_central implementations.
typedef struct {
    int (*send_command)(const char *device_id, const gw_message_t *msg);
    int (*is_connected)(const char *device_id);
} device_command_hooks_t;
void device_command_set_hooks(const device_command_hooks_t *hooks);

// Test-only: fully resets dispatcher, registry and pending request state
// so TEST_CASEs can call command_dispatcher_init() again. Never use in
// production code.
void command_dispatcher_reset_for_test(void);

#endif // COMMAND_DISPATCHER_INTERNAL_H
