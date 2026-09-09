#include "device_command_service_internal.h"

/* Urgent events are transport-critical.  They never share normal submit
 * capacity, and every successful enqueue wakes the DCS worker immediately. */
bool dcs_urgent_enqueue(const dcs_urgent_event_t *event, TickType_t wait_ticks)
{
    if (xQueueSend(g_dcs.urgent_queue, event, wait_ticks) != pdTRUE) {
        dcs_stats_inc(&g_dcs.stats.urgent_queue_full);
        return false;
    }
    xTaskNotifyGive(g_dcs.task);
    return true;
}
