#ifndef GATEWAY_OTA_VALIDATE_H
#define GATEWAY_OTA_VALIDATE_H

#include "esp_err.h"

// Performs only early structural checks and records whether runtime
// finalization is required. Call after NVS init.
esp_err_t gateway_ota_validate(void);

// Applies the full-init memory gate and marks the image valid, or rolls back
// when required resources/floors are not available.
esp_err_t gateway_ota_finalize(const char *profile);

#endif // GATEWAY_OTA_VALIDATE_H
