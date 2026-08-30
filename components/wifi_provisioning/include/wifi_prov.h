#ifndef WIFI_PROV_H
#define WIFI_PROV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_PROV_MAX_SCAN_RESULTS 16

typedef enum {
    WIFI_PROV_STATE_UNINITIALIZED = 0,
    WIFI_PROV_STATE_BOOT_CONNECTING,
    WIFI_PROV_STATE_PROVISIONING,
    WIFI_PROV_STATE_TESTING,
    WIFI_PROV_STATE_RESTART_PENDING,
    WIFI_PROV_STATE_CONNECTED,
    WIFI_PROV_STATE_RECONNECTING,
    WIFI_PROV_STATE_FAILED,
} wifi_prov_state_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t authmode;
} wifi_prov_ap_record_t;

/* Existing compatibility API */
int wifi_prov_init(void);
bool wifi_prov_is_connected(void);
bool wifi_prov_is_provisioning(void);
wifi_prov_state_t wifi_prov_get_state(void);
const char *wifi_prov_state_name(wifi_prov_state_t state);
int wifi_prov_scan(wifi_prov_ap_record_t *records,
                   size_t max_records,
                   size_t *out_count);
int wifi_prov_test_and_save(const char *ssid, const char *password);
int wifi_prov_save_and_connect(const char *ssid, const char *password);
int wifi_prov_schedule_restart(unsigned delay_ms);
void wifi_prov_get_ip(char *out_ip, size_t out_ip_len);

/* Optional new API; does not hot-switch HTTP mode at runtime. */
esp_err_t wifi_prov_clear_credentials(void);

/* State change observer (Plan v1.1 §16): called synchronously from
 * set_state() when the workflow state actually changes. Only one observer
 * slot; registering replaces the previous one. Passing NULL removes it. */
typedef void (*wifi_prov_state_change_fn)(wifi_prov_state_t new_state,
                                          void *context);
void wifi_prov_register_state_observer(wifi_prov_state_change_fn fn,
                                       void *context);

#ifdef __cplusplus
}
#endif

#endif // WIFI_PROV_H
