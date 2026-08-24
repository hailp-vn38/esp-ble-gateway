#ifndef WIFI_PROV_H
#define WIFI_PROV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WIFI_PROV_MAX_SCAN_RESULTS 16

typedef enum {
    WIFI_PROV_STATE_SOFTAP = 0,
    WIFI_PROV_STATE_CONNECTING,
    WIFI_PROV_STATE_CONNECTED,
    WIFI_PROV_STATE_FAILED,
} wifi_prov_state_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t authmode;
} wifi_prov_ap_record_t;

int wifi_prov_init(void);
bool wifi_prov_is_connected(void);
bool wifi_prov_is_provisioning(void);
wifi_prov_state_t wifi_prov_get_state(void);
const char *wifi_prov_state_name(wifi_prov_state_t state);
int wifi_prov_scan(wifi_prov_ap_record_t *records, size_t max_records, size_t *out_count);
int wifi_prov_test_and_save(const char *ssid, const char *password);
int wifi_prov_save_and_connect(const char *ssid, const char *password);
int wifi_prov_schedule_restart(unsigned delay_ms);
void wifi_prov_get_ip(char *out_ip, size_t out_ip_len);

#endif // WIFI_PROV_H
