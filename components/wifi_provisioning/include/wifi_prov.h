#ifndef WIFI_PROV_H
#define WIFI_PROV_H

#include <stdbool.h>
#include <stddef.h>

int wifi_prov_init(void);
bool wifi_prov_is_connected(void);
int wifi_prov_save_and_connect(const char *ssid, const char *password);
void wifi_prov_get_ip(char *out_ip, size_t out_ip_len);

#endif // WIFI_PROV_H
