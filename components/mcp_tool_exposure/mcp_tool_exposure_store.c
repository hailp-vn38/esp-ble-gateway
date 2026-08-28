#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "mcp_tool_exposure_internal.h"

static const char *TAG = "mcp_exp_store";

esp_err_t mcp_exposure_store_load(mcp_exposure_persisted_record_t *records,
                                  size_t capacity, size_t *out_count,
                                  uint32_t *out_revision)
{
    *out_count = 0;
    *out_revision = 0;

    nvs_handle_t h;
    esp_err_t err = nvs_open(MCP_EXP_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No exposure store found (first boot)");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Read blob size first. */
    size_t blob_size = 0;
    err = nvs_get_blob(h, MCP_EXP_NVS_KEY, NULL, &blob_size);
    if (err != ESP_OK || blob_size < sizeof(mcp_exposure_store_blob_t)) {
        ESP_LOGW(TAG, "Exposure blob missing or too small (size=%zu)", blob_size);
        nvs_close(h);
        return (err == ESP_OK) ? ESP_ERR_INVALID_SIZE : err;
    }

    mcp_exposure_store_blob_t *blob = malloc(blob_size);
    if (blob == NULL) {
        nvs_close(h);
        return ESP_ERR_NO_MEM;
    }

    err = nvs_get_blob(h, MCP_EXP_NVS_KEY, blob, &blob_size);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS blob read failed: %s", esp_err_to_name(err));
        free(blob);
        return err;
    }

    if (blob->schema_version != MCP_EXP_STORE_SCHEMA_VERSION) {
        ESP_LOGW(TAG, "Exposure schema version mismatch: got %u, expected %u",
                 blob->schema_version, MCP_EXP_STORE_SCHEMA_VERSION);
        free(blob);
        return ESP_ERR_INVALID_VERSION;
    }

    /* Validate count. */
    size_t max_records = (blob_size - sizeof(mcp_exposure_store_blob_t)) /
                         sizeof(mcp_exposure_persisted_record_t);
    size_t n = blob->count;
    if (n > max_records) n = max_records;
    if (n > capacity) n = capacity;

    memcpy(records, blob->records, n * sizeof(mcp_exposure_persisted_record_t));
    *out_count = n;
    *out_revision = blob->catalog_revision;

    ESP_LOGI(TAG, "Loaded %zu exposure records, revision=%" PRIu32,
             n, blob->catalog_revision);
    free(blob);
    return ESP_OK;
}

esp_err_t mcp_exposure_store_save(const mcp_exposure_persisted_record_t *records,
                                  size_t count, uint32_t revision)
{
    size_t blob_size = sizeof(mcp_exposure_store_blob_t) +
                       count * sizeof(mcp_exposure_persisted_record_t);

    mcp_exposure_store_blob_t *blob = malloc(blob_size);
    if (blob == NULL) return ESP_ERR_NO_MEM;

    blob->schema_version = MCP_EXP_STORE_SCHEMA_VERSION;
    blob->reserved0 = 0;
    blob->count = (uint16_t)count;
    blob->catalog_revision = revision;
    if (count > 0) {
        memcpy(blob->records, records,
               count * sizeof(mcp_exposure_persisted_record_t));
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(MCP_EXP_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed for save: %s", esp_err_to_name(err));
        free(blob);
        return err;
    }

    err = nvs_set_blob(h, MCP_EXP_NVS_KEY, blob, blob_size);
    free(blob);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS blob write failed: %s", esp_err_to_name(err));
        nvs_close(h);
        return err;
    }

    err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS commit failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Saved %zu exposure records, revision=%" PRIu32,
             count, revision);
    return ESP_OK;
}

esp_err_t mcp_exposure_store_erase(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(MCP_EXP_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
        return err;
    }
    err = nvs_erase_all(h);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}
