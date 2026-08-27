#ifndef GATEWAY_OTA_VALIDATE_H
#define GATEWAY_OTA_VALIDATE_H

#include "esp_err.h"

// Checks OTA image state; if PENDING_VERIFY, runs bounded self-test and
// marks valid or triggers rollback. Call early in app_main() after NVS init.
esp_err_t gateway_ota_validate(void);

#endif // GATEWAY_OTA_VALIDATE_H
