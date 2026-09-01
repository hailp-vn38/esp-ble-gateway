#include <string.h>

#include "device_store_internal.h"

static bool valid_text(const char *value, size_t max_length)
{
    if (value == NULL) return false;
    size_t length = strnlen(value, max_length);
    return length > 0 && length < max_length;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int device_store_entry_parse_ble_addr(const char *text, uint8_t addr[6])
{
    if (text == NULL || addr == NULL) return -1;

    uint8_t display_order[6];
    for (int i = 0; i < 6; i++) {
        int high = hex_value(*text++);
        if (high < 0) return -1;
        int low = hex_value(*text++);
        if (low < 0) return -1;
        display_order[i] = (uint8_t)((high << 4) | low);
        if (i < 5) {
            if (*text != ':' && *text != '-') return -1;
            text++;
        }
    }
    if (*text != '\0') return -1;

    /* NimBLE stores addresses least-significant byte first. */
    for (int i = 0; i < 6; i++) addr[i] = display_order[5 - i];
    return 0;
}

bool device_store_entry_create(device_entry_t *entry, const char *device_id,
                               const char *name)
{
    if (entry == NULL || !valid_text(device_id, DEVICE_ID_MAX_LEN) ||
        !valid_text(name, DEVICE_NAME_MAX_LEN)) {
        return false;
    }

    // device_id is a logical identifier only. BLE transport identity is
    // set exclusively through device_store_set_ble_identity(); a MAC-looking
    // device_id must never imply one (refactor plan §7).
    memset(entry, 0, sizeof(*entry));
    strlcpy(entry->device_id, device_id, sizeof(entry->device_id));
    strlcpy(entry->name, name, sizeof(entry->name));
    return true;
}

bool device_store_entry_edit(device_entry_t *entry, const char *new_name)
{
    if (entry == NULL || new_name == NULL ||
        !valid_text(new_name, DEVICE_NAME_MAX_LEN)) {
        return false;
    }

    strlcpy(entry->name, new_name, sizeof(entry->name));
    return true;
}
