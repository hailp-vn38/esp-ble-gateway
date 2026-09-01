#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "device_state.h"

static void reset_state(void)
{
    device_state_reset_for_test();
}

/* ── Helper: build a feature_state notification ─────────────────────── */

static gw_message_t make_feature_state(const char *device_id,
                                        const char *feature_id,
                                        uint8_t property_id,
                                        bool is_bool,
                                        bool bool_val,
                                        int32_t int_val)
{
    gw_message_t msg = {0};
    msg.protocol_version = 4;
    strlcpy(msg.type, "device_event", sizeof(msg.type));
    strlcpy(msg.command, "feature_state", sizeof(msg.command));
    strlcpy(msg.device_id, device_id, sizeof(msg.device_id));
    strlcpy(msg.feature_id, feature_id, sizeof(msg.feature_id));
    msg.has_device_id = true;
    msg.has_feature_id = true;
    msg.property_id = property_id;
    msg.has_property_id = true;
    if (is_bool) {
        msg.feature_value_bool = bool_val;
        msg.has_feature_value_bool = true;
    } else {
        msg.feature_value_int = int_val;
        msg.has_feature_value_int = true;
    }
    return msg;
}

/* ── Tests ──────────────────────────────────────────────────────────── */

TEST_CASE("init succeeds", "[device_state]")
{
    reset_state();
    TEST_ASSERT_EQUAL_INT(ESP_OK, device_state_init());
    reset_state();
}

TEST_CASE("on_notify consumes feature_state bool", "[device_state]")
{
    reset_state();
    device_state_init();

    gw_message_t msg = make_feature_state("gw-1", "led_main",
                                           1 /* GW_PROP_ON_OFF */,
                                           true, true, 0);
    TEST_ASSERT_TRUE(device_state_on_notify("gw-1", &msg));

    device_state_entry_t entry;
    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          device_state_get("gw-1", "led_main", 1, &entry));
    TEST_ASSERT_TRUE(entry.valid);
    TEST_ASSERT_TRUE(entry.value_bool);
    TEST_ASSERT_EQUAL_STRING("gw-1", entry.device_id);
    TEST_ASSERT_EQUAL_STRING("led_main", entry.feature_id);
    TEST_ASSERT_EQUAL_UINT8(1, entry.property_id);

    reset_state();
}

TEST_CASE("on_notify consumes feature_state int", "[device_state]")
{
    reset_state();
    device_state_init();

    gw_message_t msg = make_feature_state("gw-1", "temp_sensor",
                                           5 /* GW_PROP_TEMPERATURE */,
                                           false, false, 2500);
    TEST_ASSERT_TRUE(device_state_on_notify("gw-1", &msg));

    device_state_entry_t entry;
    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          device_state_get("gw-1", "temp_sensor", 5, &entry));
    TEST_ASSERT_TRUE(entry.valid);
    TEST_ASSERT_EQUAL_INT32(2500, entry.value_int);

    reset_state();
}

TEST_CASE("on_notify returns false for non-feature_state", "[device_state]")
{
    reset_state();
    device_state_init();

    /* device_ack should not be consumed */
    gw_message_t msg = {0};
    msg.protocol_version = 4;
    strlcpy(msg.type, "device_ack", sizeof(msg.type));
    strlcpy(msg.command, "toggle", sizeof(msg.command));
    msg.has_device_id = true;
    strlcpy(msg.device_id, "gw-1", sizeof(msg.device_id));
    msg.has_request_id = true;
    msg.request_id = 1;
    TEST_ASSERT_FALSE(device_state_on_notify("gw-1", &msg));

    /* capabilities_begin should not be consumed */
    strlcpy(msg.type, "capabilities_begin", sizeof(msg.type));
    TEST_ASSERT_FALSE(device_state_on_notify("gw-1", &msg));

    reset_state();
}

TEST_CASE("valid flag is false for unknown entry", "[device_state]")
{
    reset_state();
    device_state_init();

    device_state_entry_t entry;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_NOT_FOUND,
                          device_state_get("gw-1", "led_main", 1, &entry));

    reset_state();
}

TEST_CASE("update overwrites previous value", "[device_state]")
{
    reset_state();
    device_state_init();

    gw_message_t msg1 = make_feature_state("gw-1", "led_main", 1,
                                            true, true, 0);
    device_state_on_notify("gw-1", &msg1);

    gw_message_t msg2 = make_feature_state("gw-1", "led_main", 1,
                                            true, false, 0);
    device_state_on_notify("gw-1", &msg2);

    device_state_entry_t entry;
    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          device_state_get("gw-1", "led_main", 1, &entry));
    TEST_ASSERT_FALSE(entry.value_bool);

    reset_state();
}

TEST_CASE("timestamp updates between notifications", "[device_state]")
{
    reset_state();
    device_state_init();

    gw_message_t msg = make_feature_state("gw-1", "led_main", 1,
                                           true, true, 0);
    device_state_on_notify("gw-1", &msg);

    device_state_entry_t entry1;
    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          device_state_get("gw-1", "led_main", 1, &entry1));

    /* Second notification */
    device_state_on_notify("gw-1", &msg);

    device_state_entry_t entry2;
    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          device_state_get("gw-1", "led_main", 1, &entry2));
    TEST_ASSERT_TRUE(entry2.updated_at_ms >= entry1.updated_at_ms);

    reset_state();
}

TEST_CASE("forget clears entries for device", "[device_state]")
{
    reset_state();
    device_state_init();

    gw_message_t msg = make_feature_state("gw-1", "led_main", 1,
                                           true, true, 0);
    device_state_on_notify("gw-1", &msg);

    device_state_entry_t entry;
    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          device_state_get("gw-1", "led_main", 1, &entry));

    device_state_forget("gw-1");

    TEST_ASSERT_EQUAL_INT(ESP_ERR_NOT_FOUND,
                          device_state_get("gw-1", "led_main", 1, &entry));

    reset_state();
}

TEST_CASE("two devices same feature_id do not cross-update", "[device_state]")
{
    reset_state();
    device_state_init();

    gw_message_t msg1 = make_feature_state("gw-1", "led_main", 1,
                                            true, true, 0);
    gw_message_t msg2 = make_feature_state("gw-2", "led_main", 1,
                                            true, false, 0);
    device_state_on_notify("gw-1", &msg1);
    device_state_on_notify("gw-2", &msg2);

    device_state_entry_t entry1, entry2;
    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          device_state_get("gw-1", "led_main", 1, &entry1));
    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          device_state_get("gw-2", "led_main", 1, &entry2));
    TEST_ASSERT_TRUE(entry1.value_bool);
    TEST_ASSERT_FALSE(entry2.value_bool);

    reset_state();
}

TEST_CASE("get_all returns entries for device", "[device_state]")
{
    reset_state();
    device_state_init();

    gw_message_t msg1 = make_feature_state("gw-1", "led_main", 1,
                                            true, true, 0);
    gw_message_t msg2 = make_feature_state("gw-1", "temp_sensor", 5,
                                            false, false, 2500);
    gw_message_t msg3 = make_feature_state("gw-2", "led_main", 1,
                                            true, false, 0);
    device_state_on_notify("gw-1", &msg1);
    device_state_on_notify("gw-1", &msg2);
    device_state_on_notify("gw-2", &msg3);

    device_state_view_t view;
    TEST_ASSERT_EQUAL_INT(ESP_OK, device_state_get_all("gw-1", &view));
    TEST_ASSERT_EQUAL_UINT32(2, view.count);
    TEST_ASSERT_NOT_NULL(view.entries);

    TEST_ASSERT_EQUAL_INT(ESP_OK, device_state_get_all("gw-2", &view));
    TEST_ASSERT_EQUAL_UINT32(1, view.count);

    TEST_ASSERT_EQUAL_INT(ESP_OK, device_state_get_all("gw-3", &view));
    TEST_ASSERT_EQUAL_UINT32(0, view.count);

    reset_state();
}

TEST_CASE("reset clears all state", "[device_state]")
{
    reset_state();
    device_state_init();

    gw_message_t msg = make_feature_state("gw-1", "led_main", 1,
                                           true, true, 0);
    device_state_on_notify("gw-1", &msg);

    device_state_entry_t entry;
    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          device_state_get("gw-1", "led_main", 1, &entry));

    device_state_reset_for_test();

    TEST_ASSERT_EQUAL_INT(ESP_ERR_NOT_FOUND,
                          device_state_get("gw-1", "led_main", 1, &entry));

    reset_state();
}
