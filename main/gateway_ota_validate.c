#include "gateway_ota_validate.h"

#include "esp_ota_ops.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"

#include "wifi_prov.h"

static const char *TAG = "ota_validate";

static bool gateway_post_ota_self_test(void)
{
    // 1. PSRAM init check
    if (!esp_psram_is_initialized()) {
        ESP_LOGE(TAG, "Self-test FAIL: PSRAM not initialized");
        return false;
    }

    // 2. Internal/external heap query
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (internal_free == 0) {
        ESP_LOGE(TAG, "Self-test FAIL: no internal heap");
        return false;
    }

    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (psram_free == 0) {
        ESP_LOGE(TAG, "Self-test FAIL: no PSRAM heap");
        return false;
    }

    // 3. NVS init (already done in main, but verify it's accessible)
    nvs_stats_t nvs_stats;
    if (nvs_get_stats(NULL, &nvs_stats) != ESP_OK) {
        ESP_LOGE(TAG, "Self-test FAIL: NVS not accessible");
        return false;
    }

    // 4. Wi-Fi subsystem reachable
    if (!wifi_prov_is_provisioning() && !wifi_prov_is_connected()) {
        // Wi-Fi may not be connected yet after OTA, but subsystem should be init
        ESP_LOGW(TAG, "Self-test WARN: Wi-Fi not connected (may be normal after fresh OTA)");
    }

    ESP_LOGI(TAG, "Self-test PASS: PSRAM=%u, internal=%u, NVS=%u/%u pages used",
             (unsigned)psram_free, (unsigned)internal_free,
             (unsigned)nvs_stats.used_entries, (unsigned)nvs_stats.total_entries);
    return true;
}

esp_err_t gateway_ota_validate(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL) {
        ESP_LOGE(TAG, "Could not determine running partition");
        return ESP_ERR_INVALID_STATE;
    }

    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) {
        ESP_LOGW(TAG, "Could not read OTA state for partition %s", running->label);
        return ESP_OK;
    }

    if (state != ESP_OTA_IMG_PENDING_VERIFY) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "OTA image %s is PENDING_VERIFY — running self-test", running->label);

    bool ok = gateway_post_ota_self_test();

    if (ok) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to mark app valid: %s", esp_err_to_name(err));
            return err;
        }
        ESP_LOGI(TAG, "OTA image %s marked valid", running->label);
    } else {
        ESP_LOGE(TAG, "OTA self-test FAILED — rolling back to previous image");
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }

    return ESP_OK;
}
