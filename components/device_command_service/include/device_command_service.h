#ifndef DEVICE_COMMAND_SERVICE_H
#define DEVICE_COMMAND_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "device_command_types.h"
#include "device_types.h"
#include "esp_err.h"

/* ── Completion callback ─────────────────────────────────────────────── */

typedef void (*device_command_completion_fn)(
    const device_command_result_t *result,
    void *context);

/* ── Transport hooks (mockable for testing) ──────────────────────────── */

typedef struct {
    int (*send_command)(const char *device_id, const gw_message_t *message);
    int (*is_connected)(const char *device_id);
} device_command_transport_hooks_t;

/* ── Service stats ───────────────────────────────────────────────────── */

typedef struct {
    uint32_t submitted;
    uint32_t completed_ok;
    uint32_t completed_error;
    uint32_t busy_rejections;
    uint32_t timeout_count;
    uint32_t disconnect_count;
    uint32_t transport_errors;
    uint32_t queue_full;
    uint32_t urgent_queue_full;
    uint32_t ack_received;
    uint32_t ack_unmatched;
    uint32_t ack_duplicate;
    uint32_t ack_dispatch_latency_max_us;
    uint32_t max_pending;
} device_command_service_stats_t;

#define DEVICE_COMMAND_SERVICE_MAX_PENDING 4

/* ── Public API ──────────────────────────────────────────────────────── */

/**
 * Initialize the device command service.
 * Creates one event queue and one service task.
 * Must be called once before any submit/notify calls.
 */
esp_err_t device_command_service_init(void);

/**
 * Deinitialize the device command service.
 * Waits for the service task to stop, frees resources.
 */
void device_command_service_deinit(void);

/**
 * Submit a device command request.
 * Returns immediately after queueing. Completion callback is invoked
 * from the service task context when ACK/timeout/disconnect occurs.
 *
 * @param request   Typed command request (copied internally).
 * @param completion Completion callback (must not be NULL).
 * @param context   Opaque context passed to completion.
 * @return ESP_OK on success, ESP_ERR_NO_MEM if queue full,
 *         ESP_ERR_INVALID_STATE if not initialized.
 */
esp_err_t device_command_service_submit(
    const device_command_request_t *request,
    device_command_completion_fn completion,
    void *context);

/**
 * Notify the service of a BLE ACK/device_ack message.
 * Called from the BLE notify path. The service copies what it needs.
 *
 * @return true if the message was consumed (matched a pending request).
 */
bool device_command_service_on_notify(
    const char *device_id,
    const gw_message_t *message);

/**
 * Notify the service that a device has disconnected.
 * Fails any pending request for that device with NOT_CONNECTED.
 */
void device_command_service_on_disconnect(const char *device_id);

/** Cancel a pending request for one device with CANCELLED. */
esp_err_t device_command_service_cancel_device(const char *device_id);

/**
 * Get service statistics.
 */
void device_command_service_get_stats(device_command_service_stats_t *out);

/**
 * Set transport hooks (for testing with mock BLE).
 * Pass NULL to reset to default BLE central hooks.
 */
void device_command_service_set_hooks(const device_command_transport_hooks_t *hooks);

/**
 * Get the number of pending requests (for testing).
 */
uint32_t device_command_service_get_pending_count(void);

#endif // DEVICE_COMMAND_SERVICE_H
