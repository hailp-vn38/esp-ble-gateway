#ifndef DEVICE_SCHEMA_CONTROL_ADAPTER_H
#define DEVICE_SCHEMA_CONTROL_ADAPTER_H

#include "esp_err.h"

/* Installs the schema submitter bridge.  The scheduler and DCS must be
 * initialized before this is called. */
esp_err_t device_schema_control_adapter_init(void);

#endif /* DEVICE_SCHEMA_CONTROL_ADAPTER_H */
