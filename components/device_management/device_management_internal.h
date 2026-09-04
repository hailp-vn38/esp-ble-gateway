#ifndef DEVICE_MANAGEMENT_INTERNAL_H
#define DEVICE_MANAGEMENT_INTERNAL_H

#include "ble_central.h"
#include "device_management.h"
#include "device_store.h"
#include "esp_err.h"
#include "gateway_events.h"

/* Dependency overrides for deterministic unit tests. NULL restores defaults. */
typedef struct {
    int (*connect)(const char *, const uint8_t *, uint8_t);
    ble_central_err_t (*get_status)(const char *,
                                    ble_central_device_status_t *);
    int (*forget_peer)(const char *, const uint8_t *, uint8_t, bool);
    esp_err_t (*schema_get)(const char *, device_schema_snapshot_t *);
    esp_err_t (*schema_forget)(const char *);
    void (*state_forget)(const char *);
    esp_err_t (*cancel_commands)(const char *);
    device_store_result_t (*store_delete)(const char *);
    void (*publish)(gateway_event_t *);
} device_management_hooks_t;

void device_management_set_hooks(const device_management_hooks_t *hooks);

#endif /* DEVICE_MANAGEMENT_INTERNAL_H */
