#include "memory_policy.h"

#include "cJSON.h"

static void *gw_cjson_malloc(size_t size)
{
    return gw_mem_alloc(size, GW_MEM_EXTERNAL_PREFERRED);
}

static void gw_cjson_free(void *ptr)
{
    gw_mem_free(ptr);
}

void gw_cjson_init_hooks(void)
{
    cJSON_Hooks hooks = {
        .malloc_fn = gw_cjson_malloc,
        .free_fn = gw_cjson_free,
    };
    cJSON_InitHooks(&hooks);
}
