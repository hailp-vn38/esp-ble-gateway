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
static bool s_frozen;
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
    s_frozen = false;
    xSemaphoreGive(s_registry_mutex);
    return 0;
}

int command_registry_register(const char *command_name, gateway_command_fn_t fn)
{
    if (command_name == NULL || fn == NULL || command_name[0] == '\0' ||
        strnlen(command_name, GW_MSG_COMMAND_LEN) >= GW_MSG_COMMAND_LEN ||
        s_registry_mutex == NULL ||
        xSemaphoreTake(s_registry_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return -1;
    }

    int result = -1;
    if (!s_frozen && s_registry_count < DISPATCHER_MAX_COMMANDS) {
        bool duplicate = false;
        for (int i = 0; i < s_registry_count; i++) {
            if (strcmp(s_registry[i].command_name, command_name) == 0) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            strlcpy(s_registry[s_registry_count].command_name, command_name,
                    sizeof(s_registry[s_registry_count].command_name));
            s_registry[s_registry_count++].fn = fn;
            result = 0;
        }
    }
    xSemaphoreGive(s_registry_mutex);
    return result;
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

int command_registry_get_names(char out_names[][GW_MSG_COMMAND_LEN], int max_names)
{
    if (out_names == NULL || max_names <= 0 || s_registry_mutex == NULL ||
        xSemaphoreTake(s_registry_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return -1;
    }

    int count = s_registry_count < max_names ? s_registry_count : max_names;
    for (int i = 0; i < count; i++) {
        strlcpy(out_names[i], s_registry[i].command_name, GW_MSG_COMMAND_LEN);
    }
    xSemaphoreGive(s_registry_mutex);
    return count;
}

int command_registry_freeze(void)
{
    if (s_registry_mutex == NULL ||
        xSemaphoreTake(s_registry_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return -1;
    }
    s_frozen = true;
    xSemaphoreGive(s_registry_mutex);
    return 0;
}

bool command_registry_is_frozen(void)
{
    if (s_registry_mutex == NULL ||
        xSemaphoreTake(s_registry_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return false;
    }
    bool frozen = s_frozen;
    xSemaphoreGive(s_registry_mutex);
    return frozen;
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
