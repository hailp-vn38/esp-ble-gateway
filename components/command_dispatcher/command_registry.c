#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "command_dispatcher.h"
#include "command_dispatcher_internal.h"

typedef struct {
    char command_name[GW_MSG_COMMAND_LEN];
    gateway_command_fn_t fn;
} registry_entry_t;

static registry_entry_t s_registry[DISPATCHER_MAX_COMMANDS];
static int s_registry_count;
static SemaphoreHandle_t s_registry_mutex;

int command_registry_init(void)
{
    if (s_registry_mutex == NULL) s_registry_mutex = xSemaphoreCreateMutex();
    if (s_registry_mutex == NULL ||
        xSemaphoreTake(s_registry_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return -1;
    }

    memset(s_registry, 0, sizeof(s_registry));
    s_registry_count = 0;
    xSemaphoreGive(s_registry_mutex);
    return 0;
}

int command_dispatcher_register(const char *command_name, gateway_command_fn_t fn)
{
    if (command_name == NULL || fn == NULL || command_name[0] == '\0' ||
        strnlen(command_name, GW_MSG_COMMAND_LEN) >= GW_MSG_COMMAND_LEN ||
        s_registry_mutex == NULL ||
        xSemaphoreTake(s_registry_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return -1;
    }

    for (int i = 0; i < s_registry_count; i++) {
        if (strcmp(s_registry[i].command_name, command_name) == 0) {
            xSemaphoreGive(s_registry_mutex);
            return -1;
        }
    }
    if (s_registry_count >= DISPATCHER_MAX_COMMANDS) {
        xSemaphoreGive(s_registry_mutex);
        return -1;
    }

    strlcpy(s_registry[s_registry_count].command_name, command_name,
            sizeof(s_registry[s_registry_count].command_name));
    s_registry[s_registry_count++].fn = fn;
    xSemaphoreGive(s_registry_mutex);
    return 0;
}

gateway_command_fn_t command_registry_find(const char *command_name)
{
    if (command_name == NULL || s_registry_mutex == NULL ||
        xSemaphoreTake(s_registry_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return NULL;
    }

    gateway_command_fn_t fn = NULL;
    for (int i = 0; i < s_registry_count; i++) {
        if (strcmp(s_registry[i].command_name, command_name) == 0) {
            fn = s_registry[i].fn;
            break;
        }
    }
    xSemaphoreGive(s_registry_mutex);
    return fn;
}

int command_dispatcher_get_registered_names(const char **out_names, int max_names)
{
    if (out_names == NULL || max_names <= 0 || s_registry_mutex == NULL ||
        xSemaphoreTake(s_registry_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return -1;
    }

    int count = s_registry_count < max_names ? s_registry_count : max_names;
    for (int i = 0; i < count; i++) out_names[i] = s_registry[i].command_name;
    xSemaphoreGive(s_registry_mutex);
    return count;
}

int command_dispatcher_is_registered(const char *command_name)
{
    return command_registry_find(command_name) != NULL;
}

int command_registry_count(void)
{
    if (s_registry_mutex == NULL ||
        xSemaphoreTake(s_registry_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return -1;
    }

    int count = s_registry_count;
    xSemaphoreGive(s_registry_mutex);
    return count;
}
