#ifndef WEB_EXPOSURE_API_INTERNAL_H
#define WEB_EXPOSURE_API_INTERNAL_H

#include <stdbool.h>

#include "cJSON.h"
#include "cbor_codec.h"

typedef enum {
    WEB_EXPOSURE_PARSE_OK = 0,
    WEB_EXPOSURE_PARSE_INVALID_ARGUMENT,
    WEB_EXPOSURE_PARSE_MISSING_DEVICE_ID,
    WEB_EXPOSURE_PARSE_INVALID_FEATURE_ID,
    WEB_EXPOSURE_PARSE_INVALID_ENABLED,
} web_exposure_parse_status_t;

typedef struct {
    char device_id[GW_MSG_DEVICE_ID_LEN];
    char feature_id[GW_FEATURE_ID_LEN];
    bool enabled;
} web_exposure_update_request_t;

web_exposure_parse_status_t web_exposure_parse_update_request(
    const cJSON *json, web_exposure_update_request_t *out);

#endif /* WEB_EXPOSURE_API_INTERNAL_H */
