#ifndef DEVICE_STATE_INTERNAL_H
#define DEVICE_STATE_INTERNAL_H

#include "esp_err.h"

esp_err_t device_state_seed_init(void);
void device_state_seed_forget(const char *device_id);
void device_state_seed_reset_for_test(void);

#endif /* DEVICE_STATE_INTERNAL_H */
