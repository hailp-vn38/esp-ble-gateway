#include <string.h>

#include "esp_log.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"

#include "ble_central_internal.h"

static const char *TAG = "ble_central_scan";
static ble_central_scan_result_cb_t s_scan_result_cb;
static volatile bool s_scanning;

static bool advertisement_has_gateway_service(
    const struct ble_hs_adv_fields *fields)
{
    for (int i = 0; i < fields->num_uuids16; i++) {
        if (ble_uuid_cmp(&fields->uuids16[i].u,
                         &g_ble_gateway_service_uuid.u) == 0) {
            return true;
        }
    }
    return false;
}

static int scan_event_handler(struct ble_gap_event *event, void *arg)
{
    if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        s_scanning = false;
        ESP_LOGI(TAG, "BLE scan complete, reason=%d",
                 event->disc_complete.reason);
        return 0;
    }
    if (event->type != BLE_GAP_EVENT_DISC) return 0;

    struct ble_hs_adv_fields fields;
    if (ble_hs_adv_parse_fields(&fields, event->disc.data,
                                event->disc.length_data) != 0 ||
        !advertisement_has_gateway_service(&fields)) {
        return 0;
    }

    ble_scan_result_t result = {0};
    memcpy(result.addr, event->disc.addr.val, sizeof(result.addr));
    result.addr_type = event->disc.addr.type;
    result.rssi = event->disc.rssi;
    if (fields.name != NULL && fields.name_len > 0) {
        size_t name_len = fields.name_len < sizeof(result.name) - 1
                              ? fields.name_len
                              : sizeof(result.name) - 1;
        memcpy(result.name, fields.name, name_len);
        result.name[name_len] = '\0';
    }
    if (s_scan_result_cb != NULL) s_scan_result_cb(&result);
    return 0;
}

void ble_central_scan_reset(void)
{
    s_scan_result_cb = NULL;
    s_scanning = false;
}

int ble_central_scan_start(ble_central_scan_result_cb_t scan_result_cb)
{
    if (!g_ble_host_synced || scan_result_cb == NULL || ble_gap_disc_active()) {
        return -1;
    }
    s_scan_result_cb = scan_result_cb;

    struct ble_gap_disc_params parameters = {
        .itvl = 48,
        .window = 48,
        .filter_policy = 0,
        .limited = 0,
        .passive = 0,
        .filter_duplicates = 1,
    };
    int rc = ble_gap_disc(g_ble_own_addr_type, BLE_HS_FOREVER, &parameters,
                          scan_event_handler, NULL);
    if (rc != 0) return -1;

    s_scanning = true;
    ESP_LOGI(TAG, "BLE scan started (service 0x%04X)",
             BLE_GATEWAY_SERVICE_UUID);
    return 0;
}

int ble_central_scan_stop(void)
{
    if (!ble_gap_disc_active()) {
        s_scanning = false;
        return 0;
    }
    int rc = ble_gap_disc_cancel();
    if (rc != 0 && rc != BLE_HS_EALREADY) return -1;
    return 0;
}

int ble_central_is_scanning(void)
{
    return s_scanning && ble_gap_disc_active();
}
