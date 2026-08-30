#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "memory_policy.h"
#include "mcp_tool_exposure_internal.h"

static const char *TAG = "mcp_catalog";

#ifndef CONFIG_MCP_DYNAMIC_TOOL_MAX_ENABLED
#define CONFIG_MCP_DYNAMIC_TOOL_MAX_ENABLED 32
#endif

typedef struct {
    char tool_name[MCP_DYNAMIC_TOOL_NAME_MAX + 1];
    char device_id[GW_MSG_DEVICE_ID_LEN];
    char command[GW_MSG_COMMAND_LEN];
    device_capability_t capability;
} catalog_entry_t;

/* Runtime-allocated so memory_policy places it in PSRAM (Plan v1.1 §11.2). */
static catalog_entry_t *s_entries;
static size_t s_count = 0;
static uint32_t s_revision = 0;

static SemaphoreHandle_t s_mutex = NULL;
static mcp_catalog_change_fn s_on_change = NULL;
static void *s_change_context = NULL;

static SemaphoreHandle_t ensure_mutex(void)
{
    if (s_mutex != NULL) return s_mutex;
    s_mutex = xSemaphoreCreateMutex();
    return s_mutex;
}

esp_err_t mcp_tool_catalog_init(mcp_catalog_change_fn on_change,
                                void *change_context)
{
    s_on_change = on_change;
    s_change_context = change_context;
    s_count = 0;
    s_revision = 0;
    if (s_entries == NULL) {
        s_entries = gw_mem_calloc(CONFIG_MCP_DYNAMIC_TOOL_MAX_ENABLED,
                                  sizeof(*s_entries),
                                  GW_MEM_EXTERNAL_PREFERRED);
        if (s_entries == NULL) {
            ESP_LOGE(TAG, "Failed to allocate catalog entries");
            return ESP_ERR_NO_MEM;
        }
    }
    memset(s_entries, 0,
           CONFIG_MCP_DYNAMIC_TOOL_MAX_ENABLED * sizeof(*s_entries));
    if (ensure_mutex() == NULL) {
        ESP_LOGE(TAG, "Failed to create catalog mutex");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static size_t find_by_name(const char *tool_name)
{
    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_entries[i].tool_name, tool_name) == 0) return i;
    }
    return s_count;
}

esp_err_t mcp_tool_catalog_add(const mcp_tool_binding_t *binding)
{
    if (binding == NULL) return ESP_ERR_INVALID_ARG;

    SemaphoreHandle_t mtx = ensure_mutex();
    if (mtx == NULL) return ESP_ERR_NO_MEM;
    xSemaphoreTake(mtx, portMAX_DELAY);

    if (find_by_name(binding->tool_name) < s_count) {
        xSemaphoreGive(mtx);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_entries == NULL || s_count >= CONFIG_MCP_DYNAMIC_TOOL_MAX_ENABLED) {
        xSemaphoreGive(mtx);
        return ESP_ERR_NO_MEM;
    }

    catalog_entry_t *e = &s_entries[s_count];
    strlcpy(e->tool_name, binding->tool_name, sizeof(e->tool_name));
    strlcpy(e->device_id, binding->device_id, sizeof(e->device_id));
    strlcpy(e->command, binding->command, sizeof(e->command));
    memcpy(&e->capability, &binding->capability, sizeof(e->capability));
    s_count++;
    s_revision++;

    uint32_t rev = s_revision;
    xSemaphoreGive(mtx);

    if (s_on_change != NULL) s_on_change(rev, s_change_context);
    return ESP_OK;
}

esp_err_t mcp_tool_catalog_remove(const char *tool_name)
{
    if (tool_name == NULL) return ESP_ERR_INVALID_ARG;

    SemaphoreHandle_t mtx = ensure_mutex();
    if (mtx == NULL) return ESP_ERR_NO_MEM;
    xSemaphoreTake(mtx, portMAX_DELAY);

    size_t idx = find_by_name(tool_name);
    if (idx >= s_count) {
        xSemaphoreGive(mtx);
        return ESP_ERR_NOT_FOUND;
    }

    /* Compact array. */
    for (size_t i = idx; i + 1 < s_count; i++) {
        memcpy(&s_entries[i], &s_entries[i + 1], sizeof(catalog_entry_t));
    }
    s_count--;
    memset(&s_entries[s_count], 0, sizeof(catalog_entry_t));
    s_revision++;

    uint32_t rev = s_revision;
    xSemaphoreGive(mtx);

    if (s_on_change != NULL) s_on_change(rev, s_change_context);
    return ESP_OK;
}

const mcp_tool_binding_t *mcp_tool_catalog_find_ptr(const char *tool_name)
{
    SemaphoreHandle_t mtx = ensure_mutex();
    if (mtx == NULL) return NULL;
    xSemaphoreTake(mtx, portMAX_DELAY);

    size_t idx = find_by_name(tool_name);
    if (idx >= s_count) {
        xSemaphoreGive(mtx);
        return NULL;
    }
    /* Return pointer to static entry — caller must use while holding no
     * catalog mutation.  This is acceptable for single-threaded MCP HTTPD. */
    const mcp_tool_binding_t *result = (const mcp_tool_binding_t *)&s_entries[idx];
    xSemaphoreGive(mtx);
    return result;
}

uint32_t mcp_tool_catalog_get_revision(void)
{
    SemaphoreHandle_t mtx = ensure_mutex();
    if (mtx == NULL) return 0;
    xSemaphoreTake(mtx, portMAX_DELAY);
    uint32_t rev = s_revision;
    xSemaphoreGive(mtx);
    return rev;
}

void mcp_tool_catalog_get_snapshot(mcp_tool_binding_t *out,
                                  size_t capacity, size_t *out_count)
{
    SemaphoreHandle_t mtx = ensure_mutex();
    if (mtx == NULL) {
        *out_count = 0;
        return;
    }
    xSemaphoreTake(mtx, portMAX_DELAY);

    size_t n = s_count < capacity ? s_count : capacity;
    for (size_t i = 0; i < n; i++) {
        memcpy(&out[i], &s_entries[i], sizeof(mcp_tool_binding_t));
    }
    *out_count = n;

    xSemaphoreGive(mtx);
}

void mcp_tool_catalog_remove_device(const char *device_id)
{
    if (device_id == NULL) return;

    SemaphoreHandle_t mtx = ensure_mutex();
    if (mtx == NULL) return;
    xSemaphoreTake(mtx, portMAX_DELAY);

    bool changed = false;
    for (size_t i = s_count; i > 0; ) {
        i--;
        if (strcmp(s_entries[i].device_id, device_id) == 0) {
            for (size_t j = i; j + 1 < s_count; j++) {
                memcpy(&s_entries[j], &s_entries[j + 1],
                       sizeof(catalog_entry_t));
            }
            s_count--;
            memset(&s_entries[s_count], 0, sizeof(catalog_entry_t));
            changed = true;
        }
    }

    if (changed) {
        s_revision++;
    }
    uint32_t rev = s_revision;
    xSemaphoreGive(mtx);

    if (changed && s_on_change != NULL) s_on_change(rev, s_change_context);
}
