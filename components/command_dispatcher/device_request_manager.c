#include <string.h>

#include "esp_log.h"

#include "device_request_manager.h"
#include "device_store.h"

static const char *TAG = "dispatcher";

#define DEVICE_REQUEST_MAX_PENDING DEVICE_STORE_MAX_DEVICES

static SemaphoreHandle_t s_request_mutex;
static pending_request_t s_requests[DEVICE_REQUEST_MAX_PENDING];
static uint32_t s_next_request_id;

int device_request_manager_init(void)
{
    if (s_request_mutex == NULL) s_request_mutex = xSemaphoreCreateMutex();
    if (s_request_mutex == NULL ||
        xSemaphoreTake(s_request_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return -1;
    }

    for (size_t i = 0; i < DEVICE_REQUEST_MAX_PENDING; i++) {
        pending_request_t *request = &s_requests[i];
        if (request->semaphore == NULL) {
            request->semaphore = xSemaphoreCreateBinary();
        }
        request->in_use = false;
        request->completed = false;
        request->request_id = 0;
        request->device_id[0] = '\0';
        request->command[0] = '\0';
        memset(&request->response, 0, sizeof(request->response));
        if (request->semaphore == NULL) {
            xSemaphoreGive(s_request_mutex);
            return -1;
        }
        while (xSemaphoreTake(request->semaphore, 0) == pdTRUE) {}
    }

    s_next_request_id = 0;
    xSemaphoreGive(s_request_mutex);
    return 0;
}

// Caller must hold s_request_mutex.
static uint32_t generate_request_id_locked(void)
{
    uint32_t request_id;
    bool collision;
    do {
        request_id = ++s_next_request_id;
        if (request_id == 0) {
            // Skip 0 so it stays invalid on wire and for correlation.
            request_id = ++s_next_request_id;
        }
        collision = false;
        for (size_t i = 0; i < DEVICE_REQUEST_MAX_PENDING; i++) {
            if (s_requests[i].in_use && s_requests[i].request_id == request_id) {
                collision = true;
                break;
            }
        }
    } while (collision);
    return request_id;
}

int device_request_allocate(const char *device_id, const char *command,
                            pending_request_t **out_request)
{
    if (device_id == NULL || command == NULL || out_request == NULL ||
        s_request_mutex == NULL ||
        xSemaphoreTake(s_request_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return -1;
    }

    pending_request_t *available = NULL;
    for (size_t i = 0; i < DEVICE_REQUEST_MAX_PENDING; i++) {
        pending_request_t *request = &s_requests[i];
        if (request->in_use) {
            if (strcmp(request->device_id, device_id) == 0) {
                // Phase 1 invariant: one pending request per device.
                xSemaphoreGive(s_request_mutex);
                return -2;
            }
        } else if (available == NULL) {
            available = request;
        }
    }

    if (available != NULL) {
        while (xSemaphoreTake(available->semaphore, 0) == pdTRUE) {}
        available->in_use = true;
        available->completed = false;
        available->request_id = generate_request_id_locked();
        strlcpy(available->device_id, device_id, sizeof(available->device_id));
        strlcpy(available->command, command, sizeof(available->command));
        memset(&available->response, 0, sizeof(available->response));
        *out_request = available;
    }
    xSemaphoreGive(s_request_mutex);
    return available != NULL ? 0 : -3;
}

int device_request_wait(pending_request_t *request, TickType_t timeout)
{
    if (request == NULL || !request->in_use ||
        xSemaphoreTake(request->semaphore, timeout) != pdTRUE) {
        return -1;
    }
    return 0;
}

bool device_request_complete(const char *device_id, const gw_message_t *response)
{
    if (device_id == NULL || device_id[0] == '\0' || response == NULL ||
        strcmp(response->type, "device_ack") != 0 ||
        response->has_device_id == 0 || device_id[0] == '\0' ||
        !response->has_request_id || response->request_id == 0 ||
        s_request_mutex == NULL ||
        xSemaphoreTake(s_request_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return false;
    }

    bool matched = false;
    for (size_t i = 0; i < DEVICE_REQUEST_MAX_PENDING; i++) {
        pending_request_t *request = &s_requests[i];
        if (!request->in_use || request->completed ||
            strcmp(request->device_id, device_id) != 0 ||
            strcmp(response->device_id, device_id) != 0 ||
            request->request_id != response->request_id) {
            continue;
        }
        if (strcmp(request->command, response->command) != 0) {
            // request_id matched but command differs: protocol violation.
            ESP_LOGW(TAG,
                     "[ACK_PROTOCOL_ERROR] device=%s request_id=%lu expected_command=%s got_command=%s",
                     device_id, (unsigned long)response->request_id,
                     request->command, response->command);
            break;
        }
        request->response = *response;
        request->completed = true;
        xSemaphoreGive(request->semaphore);
        matched = true;
        break;
    }
    xSemaphoreGive(s_request_mutex);

    if (!matched) {
        ESP_LOGI(TAG, "[ACK_UNMATCHED] device=%s request_id=%lu command=%s",
                 device_id, (unsigned long)response->request_id,
                 response->command);
    }
    return matched;
}

void device_request_release(pending_request_t *request)
{
    if (request == NULL ||
        xSemaphoreTake(s_request_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }
    request->in_use = false;
    request->completed = false;
    request->request_id = 0;
    request->device_id[0] = '\0';
    request->command[0] = '\0';
    xSemaphoreGive(s_request_mutex);
}
