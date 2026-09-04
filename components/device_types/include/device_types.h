#ifndef DEVICE_TYPES_H
#define DEVICE_TYPES_H

/*
 * Protocol-independent domain string storage.
 *
 * Each limit includes the trailing NUL byte. Keep protocol aliases and
 * persistence layouts pinned to these values unless a versioned migration is
 * introduced.
 */
#define DEVICE_ID_MAX_LEN          32
#define DEVICE_NAME_MAX_LEN        32
#define DEVICE_COMMAND_MAX_LEN     32
#define DEVICE_FEATURE_ID_MAX_LEN  32

typedef char device_id_t[DEVICE_ID_MAX_LEN];
typedef char device_name_t[DEVICE_NAME_MAX_LEN];
typedef char device_command_t[DEVICE_COMMAND_MAX_LEN];
typedef char device_feature_id_t[DEVICE_FEATURE_ID_MAX_LEN];

#endif /* DEVICE_TYPES_H */
