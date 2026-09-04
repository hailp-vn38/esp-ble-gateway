#include "cbor_codec.h"
#include "device_types.h"
#include "unity.h"

_Static_assert(DEVICE_ID_MAX_LEN == 32, "device ID persistence size changed");
_Static_assert(DEVICE_NAME_MAX_LEN == 32, "device name persistence size changed");
_Static_assert(DEVICE_COMMAND_MAX_LEN == 32, "device command size changed");
_Static_assert(DEVICE_FEATURE_ID_MAX_LEN == 32,
               "device feature ID size changed");
_Static_assert(GW_MSG_DEVICE_ID_LEN == DEVICE_ID_MAX_LEN,
               "wire/domain device ID sizes diverged");
_Static_assert(GW_MSG_NAME_LEN == DEVICE_NAME_MAX_LEN,
               "wire/domain device name sizes diverged");
_Static_assert(GW_MSG_COMMAND_LEN == DEVICE_COMMAND_MAX_LEN,
               "wire/domain command sizes diverged");
_Static_assert(GW_FEATURE_ID_LEN == DEVICE_FEATURE_ID_MAX_LEN,
               "wire/domain feature ID sizes diverged");
_Static_assert(sizeof(((gw_message_t *)0)->device_id) == sizeof(device_id_t),
               "gw_message_t device_id layout changed");
_Static_assert(sizeof(((gw_message_t *)0)->name) == sizeof(device_name_t),
               "gw_message_t name layout changed");
_Static_assert(sizeof(((gw_message_t *)0)->command) == sizeof(device_command_t),
               "gw_message_t command layout changed");
_Static_assert(sizeof(((gw_message_t *)0)->feature_id) ==
                   sizeof(device_feature_id_t),
               "gw_message_t feature_id layout changed");

TEST_CASE("shared device types preserve protocol storage sizes",
          "[device_types]")
{
    TEST_ASSERT_EQUAL_UINT32(32, sizeof(device_id_t));
    TEST_ASSERT_EQUAL_UINT32(32, sizeof(device_name_t));
    TEST_ASSERT_EQUAL_UINT32(32, sizeof(device_command_t));
    TEST_ASSERT_EQUAL_UINT32(32, sizeof(device_feature_id_t));
}
