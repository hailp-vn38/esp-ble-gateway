#ifndef DEVICE_COMMAND_SERVICE_H
#define DEVICE_COMMAND_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "cbor_codec.h"
#include "device_types.h"
#include "esp_err.h"

/* ── Origin types ────────────────────────────────────────────────────── */

typedef enum {
    DEVICE_CMD_ORIGIN_CONTROL = 0,
    DEVICE_CMD_ORIGIN_SCHEMA_DISCOVERY,
    DEVICE_CMD_ORIGIN_STATE_READ,
} device_command_origin_t;

/* ── Typed request ───────────────────────────────────────────────────── */

typedef struct {
    device_command_origin_t origin;

    device_id_t device_id;
    device_command_t command;

    bool has_bool_value;
    bool bool_value;

    bool has_int_value;
    int32_t int_value;

    bool has_feature_id;
    device_feature_id_t feature_id;

    bool has_property_id;
    uint8_t property_id;
} device_command_request_t;

/* ── Result status ───────────────────────────────────────────────────── */

typedef enum {
    DEVICE_CMD_STATUS_OK = 0,
    DEVICE_CMD_STATUS_INVALID_ARGUMENT,
    DEVICE_CMD_STATUS_UNSUPPORTED_COMMAND,
    DEVICE_CMD_STATUS_BUSY,
    DEVICE_CMD_STATUS_NOT_CONNECTED,
    DEVICE_CMD_STATUS_TRANSPORT_ERROR,
    DEVICE_CMD_STATUS_TIMEOUT,
    DEVICE_CMD_STATUS_DEVICE_REJECTED,
    DEVICE_CMD_STATUS_INTERNAL_ERROR,
} device_command_status_t;

/* ── Typed result ────────────────────────────────────────────────────── */

typedef struct {
    device_command_status_t status;
    uint32_t request_id;

    bool accepted;

    bool has_bool_value;
    bool bool_value;

    bool has_int_value;
    int32_t int_value;

    bool has_feature_value_bool;
    bool feature_value_bool;

    bool has_feature_value_int;
    int32_t feature_value_int;
} device_command_result_t;

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
    uint32_t max_pending;
} device_command_service_stats_t;

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
