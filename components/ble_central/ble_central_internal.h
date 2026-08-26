#ifndef BLE_CENTRAL_INTERNAL_H
#define BLE_CENTRAL_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"

#include "ble_central.h"
#include "device_store.h"

#define BLE_GATEWAY_SERVICE_UUID         0xABF0
#define BLE_GATEWAY_COMMAND_UUID         0xABF1
#define BLE_GATEWAY_STATUS_UUID          0xABF2

#define BLE_CONN_ITVL_FAST_UNITS         12  /* 15 ms */
#define BLE_CONN_ITVL_MEDIUM_UNITS       24  /* 30 ms */
#define BLE_CONN_ITVL_BUSY_UNITS         40  /* 50 ms */
#define BLE_CONN_LATENCY                  0
#define BLE_CONN_TIMEOUT_UNITS          200  /* 2 seconds */
#define BLE_CONNECT_TIMEOUT_MS        10000
#define BLE_SECURITY_TIMEOUT_MS       10000
#define BLE_GATT_DISCOVERY_TIMEOUT_MS 10000
#define BLE_RETRY_INITIAL_MS           2000
#define BLE_RETRY_MAX_MS              30000
#define BLE_SUPERVISOR_TICK_MS         1000

#define BLE_NOTIFY_QUEUE_DEPTH             8
#define BLE_NOTIFY_TASK_STACK           4096
#define BLE_NOTIFY_TASK_PRIORITY           4

typedef enum {
    BLE_DEVICE_OFFLINE = 0,
    BLE_DEVICE_CONNECTING,
    BLE_DEVICE_CONNECTED,
    BLE_DEVICE_BACKOFF,
    BLE_DEVICE_REMOVING,
} ble_device_state_t;

typedef struct {
    bool in_use;
    char device_id[GW_MSG_DEVICE_ID_LEN];
    ble_addr_t peer_addr;
    bool has_peer_addr;
    int connection_slot;
    ble_device_state_t state;
    bool reconnect_enabled;
    uint8_t retry_count;
    int64_t last_attempt_ms;
    int64_t next_retry_ms;
} ble_device_runtime_t;

typedef enum {
    BLE_CONN_FREE = 0,
    BLE_CONN_CONNECTING,
    BLE_CONN_SECURING,
    BLE_CONN_DISCOVERING,
    BLE_CONN_READY,
    BLE_CONN_DISCONNECTING,
} ble_conn_state_t;

typedef struct {
    uint8_t slot_index;
    uint32_t generation;
} ble_conn_ref_t;

typedef struct {
    ble_conn_ref_t ref;
    uint16_t conn_handle;
} ble_conn_event_ref_t;

typedef struct {
    ble_conn_state_t state;
    int device_index;
    uint32_t generation;
    uint16_t conn_handle;
    uint16_t service_start_handle;
    uint16_t service_end_handle;
    uint16_t command_val_handle;
    uint16_t status_val_handle;
    uint16_t status_cccd_handle;
    uint16_t mtu;
    int64_t started_ms;
    int64_t discovery_started_ms;
} ble_conn_slot_t;

typedef struct {
    bool in_use;
    ble_conn_ref_t ref;
    uint16_t conn_handle;
} ble_callback_ctx_t;

typedef struct {
    uint16_t conn_handle;
    bool is_discovery;
    char device_id[GW_MSG_DEVICE_ID_LEN];
} ble_timeout_entry_t;

typedef struct {
    uint32_t connect_attempts;
    uint32_t connect_success;
    uint32_t connect_failures;
    uint32_t disconnects;
    uint32_t security_failures;
    uint32_t discovery_failures;
    uint32_t reconnect_attempts;
    uint32_t host_resets;
    uint32_t stale_callbacks;
    uint32_t notify_received;
    uint32_t notify_enqueued;
    uint32_t notify_dropped;
    uint32_t notify_decode_errors;
    uint32_t mtu_rejects;
    uint32_t identity_persist_failures;
} ble_central_metrics_t;

extern ble_conn_slot_t g_ble_connections[BLE_CENTRAL_MAX_CONN];
extern ble_device_runtime_t g_ble_devices[DEVICE_STORE_MAX_DEVICES];
extern uint8_t g_ble_own_addr_type;
extern const ble_uuid16_t g_ble_gateway_service_uuid;

int64_t ble_now_ms(void);

bool ble_state_lock(void);
void ble_state_unlock(void);
void ble_state_reset_slot_free_unlocked(int slot_index);
int ble_central_state_init(ble_central_notify_cb_t notify_cb);

bool ble_host_is_ready(void);
void ble_host_set_ready(bool ready);

int ble_state_reserve_connection(int device_index, ble_conn_ref_t *out_ref,
                                 int64_t now_ms);
void ble_state_rollback_connection_start(ble_conn_ref_t ref, int64_t now_ms);
bool ble_conn_snapshot(ble_conn_ref_t ref, ble_conn_slot_t *out);
int ble_conn_find_by_handle(uint16_t conn_handle, ble_conn_ref_t *out_ref,
                            char *out_device_id, size_t device_id_cap);
bool ble_state_on_connect_success(ble_conn_ref_t ref, uint16_t conn_handle,
                                  int64_t now_ms);
bool ble_state_on_disconnect(ble_conn_event_ref_t event_ref, int64_t now_ms,
                             char *out_device_id, size_t device_id_cap,
                             bool *out_removing, ble_addr_t *out_peer_addr,
                             bool *out_has_peer_addr);
void ble_state_mark_disconnecting(ble_conn_ref_t ref);
void ble_state_update_mtu(ble_conn_event_ref_t event_ref, uint16_t mtu);
void ble_state_set_ready(ble_conn_ref_t ref);
uint16_t ble_state_begin_discovery(ble_conn_ref_t ref, int64_t now_ms);
bool ble_state_set_service_range(ble_conn_ref_t ref, uint16_t start_handle,
                                 uint16_t end_handle);
bool ble_state_set_char_handle(ble_conn_ref_t ref, uint16_t uuid16,
                               uint16_t val_handle);
bool ble_state_set_cccd_handle(ble_conn_ref_t ref, uint16_t chr_val_handle,
                               uint16_t dsc_handle);
size_t ble_state_collect_timeouts(int64_t now_ms, ble_timeout_entry_t *out,
                                  size_t max_entries);
size_t ble_state_handle_host_reset(char (*out_device_ids)[GW_MSG_DEVICE_ID_LEN],
                                   size_t max_ids);

ble_callback_ctx_t *ble_state_ctx_acquire(ble_conn_ref_t ref);
void ble_state_ctx_release_by_ref(ble_conn_ref_t ref);
bool ble_state_ctx_snapshot(const ble_callback_ctx_t *ctx,
                            ble_conn_slot_t *out);
void ble_state_ctx_update_handle(ble_conn_ref_t ref, uint16_t conn_handle);

const ble_central_metrics_t *ble_central_metrics(void);
void ble_central_metrics_connect_failure(void);
void ble_central_metrics_stale_callback(void);
void ble_central_metrics_security_failure(void);
void ble_central_metrics_discovery_failure(void);
void ble_central_metrics_notify_enqueued(void);
void ble_central_metrics_notify_dropped(void);
void ble_central_metrics_notify_decode_error(void);
void ble_central_metrics_mtu_reject(void);
void ble_central_metrics_notify_received(void);
void ble_central_metrics_identity_persist_failure(void);
int ble_central_active_count_unlocked(void);
uint16_t ble_central_calculate_conn_interval(void);
int ble_connection_start(int device_index);
int64_t ble_backoff_delay_ms(uint8_t retry_count);

int ble_central_runtime_init(void);
int ble_runtime_find(const char *device_id);
int ble_runtime_find_or_register(const char *device_id, const ble_addr_t *addr);
bool ble_runtime_snapshot(int device_index, ble_device_runtime_t *out);
bool ble_runtime_get_peer_addr(int device_index, ble_addr_t *out);
bool ble_runtime_set_peer_addr(int device_index, const ble_addr_t *addr);
bool ble_runtime_get_device_id(int device_index, char *out, size_t cap);
void ble_runtime_finalize_remove(int device_index);
void ble_scheduler_note_success(int device_index);
int ble_scheduler_next_device(int64_t now_ms);
void ble_scheduler_note_failure(int device_index, int64_t now_ms);

int ble_central_notify_init(ble_central_notify_cb_t notify_cb);
void ble_central_notify_worker(void *arg);
void ble_central_notify_enqueue(const char *device_id, const uint8_t *data,
                                uint16_t len);

int ble_central_identity_init(void);
// Non-blocking identity hand-off for NimBLE host callbacks. The worker task
// owns the actual device_store persistence; see ble_central_identity.c.
void ble_central_identity_submit(const char *device_id, const ble_addr_t *addr);

void ble_central_abort_discovery(ble_callback_ctx_t *ctx, const char *reason);
void ble_central_start_gatt_discovery(ble_callback_ctx_t *ctx);
int ble_central_gap_event_handler(struct ble_gap_event *event, void *arg);
void ble_central_scan_reset(void);

#endif /* BLE_CENTRAL_INTERNAL_H */
