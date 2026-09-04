#include "device_command_service_internal.h"

#include <string.h>

void dcs_pending_reset(void)
{
    memset(g_dcs.pending, 0, sizeof(g_dcs.pending));
    g_dcs.next_request_id = 0;
}

dcs_pending_slot_t *dcs_pending_find_device(const char *device_id)
{
    for (size_t i = 0; i < DCS_MAX_PENDING; i++) {
        if (g_dcs.pending[i].in_use &&
            strcmp(g_dcs.pending[i].device_id, device_id) == 0) {
            return &g_dcs.pending[i];
        }
    }
    return NULL;
}

dcs_pending_slot_t *dcs_pending_find_id(const char *device_id, uint32_t request_id)
{
    for (size_t i = 0; i < DCS_MAX_PENDING; i++) {
        if (g_dcs.pending[i].in_use &&
            strcmp(g_dcs.pending[i].device_id, device_id) == 0 &&
            g_dcs.pending[i].request_id == request_id) {
            return &g_dcs.pending[i];
        }
    }
    return NULL;
}

dcs_pending_slot_t *dcs_pending_allocate(void)
{
    for (size_t i = 0; i < DCS_MAX_PENDING; i++) {
        if (!g_dcs.pending[i].in_use) {
            return &g_dcs.pending[i];
        }
    }
    return NULL;
}

uint32_t dcs_pending_next_request_id(void)
{
    uint32_t id;
    bool collision;
    do {
        id = ++g_dcs.next_request_id;
        if (id == 0) {
            id = ++g_dcs.next_request_id;
        }
        collision = false;
        for (size_t i = 0; i < DCS_MAX_PENDING; i++) {
            if (g_dcs.pending[i].in_use && g_dcs.pending[i].request_id == id) {
                collision = true;
                break;
            }
        }
    } while (collision);
    return id;
}

uint32_t dcs_pending_count(void)
{
    uint32_t count = 0;
    for (size_t i = 0; i < DCS_MAX_PENDING; i++) {
        if (g_dcs.pending[i].in_use) {
            count++;
        }
    }
    return count;
}

void dcs_pending_complete(dcs_pending_slot_t *slot,
                          const device_command_result_t *result)
{
    if (slot->completion != NULL) {
        slot->completion(result, slot->context);
    }
    slot->in_use = false;
    slot->completion = NULL;
    slot->context = NULL;
}

void dcs_pending_complete_status(dcs_pending_slot_t *slot,
                                 device_command_status_t status)
{
    device_command_result_t result = {
        .status = status,
        .request_id = slot->request_id,
    };
    dcs_pending_complete(slot, &result);
}
