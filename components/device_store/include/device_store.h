#ifndef DEVICE_STORE_H
#define DEVICE_STORE_H

#include <stdint.h>

#define DEVICE_STORE_MAX_DEVICES   16
#define DEVICE_ID_MAX_LEN          32
#define DEVICE_NAME_MAX_LEN        32
#define DEVICE_TYPE_MAX_LEN        16

typedef struct {
    char device_id[DEVICE_ID_MAX_LEN];
    char name[DEVICE_NAME_MAX_LEN];
    char type[DEVICE_TYPE_MAX_LEN];
    uint8_t ble_addr[6];
    uint8_t ble_addr_type;
    uint8_t has_ble_addr;
    int  connected; // Trang thai runtime, khong luu NVS
} device_entry_t;

int device_store_init(void);
int device_store_add(const char *device_id, const char *name, const char *type);
int device_store_delete(const char *device_id);
int device_store_edit(const char *device_id, const char *new_name, const char *new_type);
const device_entry_t *device_store_list(int *out_count);
device_entry_t *device_store_find(const char *device_id);
void device_store_set_connected(const char *device_id, int connected);

#endif // DEVICE_STORE_H
