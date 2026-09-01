#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "device_state.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

TEST_CASE("snapshot returns entries for device", "[device_state]")
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

    device_state_snapshot_t snap;
    TEST_ASSERT_EQUAL_INT(ESP_OK, device_state_snapshot("gw-1", &snap));
    TEST_ASSERT_EQUAL_UINT32(2, snap.count);

    TEST_ASSERT_EQUAL_INT(ESP_OK, device_state_snapshot("gw-2", &snap));
    TEST_ASSERT_EQUAL_UINT32(1, snap.count);

    TEST_ASSERT_EQUAL_INT(ESP_OK, device_state_snapshot("gw-3", &snap));
    TEST_ASSERT_EQUAL_UINT32(0, snap.count);

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

/* ── P00-T01: Write BOOL state then read snapshot ──────────────────── */

TEST_CASE("P00-T01: snapshot after bool write", "[device_state][p00]")
{
    reset_state();
    device_state_init();

    gw_message_t msg = make_feature_state("gw-1", "led_main", 1,
                                           true, true, 0);
    device_state_on_notify("gw-1", &msg);

    device_state_snapshot_t snap;
    TEST_ASSERT_EQUAL_INT(ESP_OK, device_state_snapshot("gw-1", &snap));
    TEST_ASSERT_EQUAL_UINT32(1, snap.count);
    TEST_ASSERT_EQUAL_STRING("gw-1", snap.entries[0].device_id);
    TEST_ASSERT_EQUAL_STRING("led_main", snap.entries[0].feature_id);
    TEST_ASSERT_EQUAL_UINT8(1, snap.entries[0].property_id);
    TEST_ASSERT_TRUE(snap.entries[0].value_bool);
    TEST_ASSERT_TRUE(snap.entries[0].valid);

    reset_state();
}

/* ── P00-T02: Write INT state at min/0/max and read back ──────────── */

TEST_CASE("P00-T02: snapshot after int min/0/max", "[device_state][p00]")
{
    reset_state();
    device_state_init();

    int32_t values[] = {INT32_MIN, 0, INT32_MAX};
    for (int t = 0; t < 3; t++) {
        gw_message_t msg = make_feature_state("gw-1", "temp", 5,
                                               false, false, values[t]);
        device_state_on_notify("gw-1", &msg);

        device_state_entry_t entry;
        TEST_ASSERT_EQUAL_INT(ESP_OK,
                              device_state_get("gw-1", "temp", 5, &entry));
        TEST_ASSERT_EQUAL_INT32(values[t], entry.value_int);
        TEST_ASSERT_TRUE(entry.valid);
    }

    reset_state();
}

/* ── P00-T03: Concurrent writer + reader ───────────────────────────── */

static void writer_task(void *arg)
{
    int *running = (int *)arg;
    gw_message_t msg = make_feature_state("gw-w", "feat", 1,
                                           true, true, 0);
    for (int i = 0; i < 200 && *running; i++) {
        msg.feature_value_bool = (i % 2 == 0);
        device_state_on_notify("gw-w", &msg);
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    vTaskDelete(NULL);
}

TEST_CASE("P00-T03: concurrent write + snapshot", "[device_state][p00]")
{
    reset_state();
    device_state_init();

    int running = 1;
    xTaskCreate(writer_task, "wr", 4096, &running, 5, NULL);
    vTaskDelay(pdMS_TO_TICKS(5));

    for (int i = 0; i < 100; i++) {
        device_state_snapshot_t snap;
        TEST_ASSERT_EQUAL_INT(ESP_OK, device_state_snapshot("gw-w", &snap));
        if (snap.count > 0) {
            TEST_ASSERT_TRUE(snap.entries[0].valid);
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    running = 0;
    vTaskDelay(pdMS_TO_TICKS(50));
    reset_state();
}

/* ── P00-T04: Interleaved get_all, update, forget for 2 devices ───── */

TEST_CASE("P00-T04: interleaved update/forget two devices", "[device_state][p00]")
{
    reset_state();
    device_state_init();

    gw_message_t msgA = make_feature_state("dev-A", "featX", 1,
                                            true, true, 0);
    gw_message_t msgB = make_feature_state("dev-B", "featY", 2,
                                            false, false, 100);
    device_state_on_notify("dev-A", &msgA);
    device_state_on_notify("dev-B", &msgB);

    /* Snapshot A */
    device_state_snapshot_t snap;
    TEST_ASSERT_EQUAL_INT(ESP_OK, device_state_snapshot("dev-A", &snap));
    TEST_ASSERT_EQUAL_UINT32(1, snap.count);

    /* Update B */
    msgB.feature_value_int = 200;
    device_state_on_notify("dev-B", &msgB);

    /* Forget A */
    device_state_forget("dev-A");

    /* A should be gone */
    TEST_ASSERT_EQUAL_INT(ESP_OK, device_state_snapshot("dev-A", &snap));
    TEST_ASSERT_EQUAL_UINT32(0, snap.count);

    /* B should still be present with updated value */
    TEST_ASSERT_EQUAL_INT(ESP_OK, device_state_snapshot("dev-B", &snap));
    TEST_ASSERT_EQUAL_UINT32(1, snap.count);
    TEST_ASSERT_EQUAL_INT32(200, snap.entries[0].value_int);

    reset_state();
}

/* ── P00-T05: Capacity limit ───────────────────────────────────────── */

TEST_CASE("P00-T05: table full then update old key", "[device_state][p00]")
{
    reset_state();
    device_state_init();

    /* Fill table */
    for (int i = 0; i < DEVICE_STATE_MAX_ENTRIES; i++) {
        gw_message_t msg = {0};
        msg.protocol_version = 4;
        strlcpy(msg.type, "device_event", sizeof(msg.type));
        strlcpy(msg.command, "feature_state", sizeof(msg.command));
        strlcpy(msg.device_id, "cap", sizeof(msg.device_id));
        char fid[8];
        snprintf(fid, sizeof(fid), "f%d", i);
        strlcpy(msg.feature_id, fid, sizeof(msg.feature_id));
        msg.has_device_id = true;
        msg.has_feature_id = true;
        msg.property_id = 0;
        msg.has_property_id = true;
        msg.feature_value_bool = true;
        msg.has_feature_value_bool = true;
        device_state_on_notify("cap", &msg);
    }

    /* New key should fail (table full, but on_notify returns true = consumed) */
    gw_message_t new_msg = make_feature_state("cap", "new_feat", 0,
                                               true, false, 0);
    TEST_ASSERT_TRUE(device_state_on_notify("cap", &new_msg));

    /* Update existing key should still work */
    gw_message_t update = make_feature_state("cap", "f0", 0,
                                              true, false, 0);
    TEST_ASSERT_TRUE(device_state_on_notify("cap", &update));

    device_state_entry_t entry;
    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          device_state_get("cap", "f0", 0, &entry));
    TEST_ASSERT_FALSE(entry.value_bool);

    reset_state();
}

/* ── P00-T06: Lifecycle — ready -> state -> disconnect ──────────────── */

TEST_CASE("P00-T06: lifecycle disconnect clears state", "[device_state][p00]")
{
    reset_state();
    device_state_init();

    /* Device gets state */
    gw_message_t msg = make_feature_state("lifecycle", "feat", 1,
                                           true, true, 0);
    device_state_on_notify("lifecycle", &msg);

    device_state_entry_t entry;
    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          device_state_get("lifecycle", "feat", 1, &entry));
    TEST_ASSERT_TRUE(entry.valid);

    /* Simulate disconnect */
    device_state_forget("lifecycle");

    /* State should be gone */
    TEST_ASSERT_EQUAL_INT(ESP_ERR_NOT_FOUND,
                          device_state_get("lifecycle", "feat", 1, &entry));

    reset_state();
}

/* ── P00-T07: Regression — existing tests already cover this ───────── */

TEST_CASE("P00-T07: regression - existing API still works", "[device_state][p00]")
{
    reset_state();
    device_state_init();

    /* get single entry */
    gw_message_t msg = make_feature_state("reg", "led", 1, true, true, 0);
    device_state_on_notify("reg", &msg);

    device_state_entry_t entry;
    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          device_state_get("reg", "led", 1, &entry));
    TEST_ASSERT_TRUE(entry.valid);
    TEST_ASSERT_TRUE(entry.value_bool);

    /* forget works */
    device_state_forget("reg");
    TEST_ASSERT_EQUAL_INT(ESP_ERR_NOT_FOUND,
                          device_state_get("reg", "led", 1, &entry));

    /* reset works */
    device_state_on_notify("reg", &msg);
    device_state_reset_for_test();
    TEST_ASSERT_EQUAL_INT(ESP_ERR_NOT_FOUND,
                          device_state_get("reg", "led", 1, &entry));

    reset_state();
}

/* ── P00-T08: Build test — snapshot capacity check ─────────────────── */

TEST_CASE("P00-T08: snapshot bounded by DEVICE_STATE_SNAPSHOT_MAX",
          "[device_state][p00]")
{
    reset_state();
    device_state_init();

    /* Write more entries than snapshot capacity for one device */
    for (int i = 0; i < DEVICE_STATE_SNAPSHOT_MAX + 4; i++) {
        gw_message_t msg = {0};
        msg.protocol_version = 4;
        strlcpy(msg.type, "device_event", sizeof(msg.type));
        strlcpy(msg.command, "feature_state", sizeof(msg.command));
        strlcpy(msg.device_id, "big", sizeof(msg.device_id));
        char fid[8];
        snprintf(fid, sizeof(fid), "feat%d", i);
        strlcpy(msg.feature_id, fid, sizeof(msg.feature_id));
        msg.has_device_id = true;
        msg.has_feature_id = true;
        msg.property_id = 0;
        msg.has_property_id = true;
        msg.feature_value_int = i;
        msg.has_feature_value_int = true;
        device_state_on_notify("big", &msg);
    }

    /* Snapshot should be bounded */
    device_state_snapshot_t snap;
    TEST_ASSERT_EQUAL_INT(ESP_OK, device_state_snapshot("big", &snap));
    TEST_ASSERT_TRUE(snap.count <= DEVICE_STATE_SNAPSHOT_MAX);

    reset_state();
}
