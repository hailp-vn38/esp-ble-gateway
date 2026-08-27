#ifndef MEMORY_POLICY_H
#define MEMORY_POLICY_H

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GW_MEM_INTERNAL_REQUIRED = 0,
    GW_MEM_EXTERNAL_REQUIRED,
    GW_MEM_EXTERNAL_PREFERRED,
    GW_MEM_DEFAULT,
} gw_mem_class_t;

void *gw_mem_alloc(size_t size, gw_mem_class_t mem_class);
void *gw_mem_calloc(size_t count, size_t size, gw_mem_class_t mem_class);
void gw_mem_free(void *ptr);

esp_err_t gw_memory_verify_psram(void);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_POLICY_H */
