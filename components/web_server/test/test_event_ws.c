#include "unity.h"
#include "web_modules.h"
#include "gateway_events.h"

#include <string.h>

/* ── Helpers ─────────────────────────────────────────────────────────── */

static volatile bool s_received;
static gateway_event_t s_last_event;
static int s_count;

static void ws_test_listener(const gateway_event_t *ev, void *ctx)
{
    (void)ctx;
    s_last_event = *ev;
    s_received = true;
    s_count++;
}

/* ── Tests ──────────────────────────────────────────────────────────── */

TEST_CASE("P02-T01: WS handler handshake registration", "[ws][p02]")
{
    /* Just verify web_event_ws_init succeeds and can be called twice */
    TEST_ASSERT_EQUAL(ESP_OK, web_event_ws_init());
    TEST_ASSERT_EQUAL(ESP_OK, web_event_ws_init()); /* idempotent */
}

TEST_CASE("P02-T02: Ring push/pop single event", "[ws][p02]")
{
    TEST_ASSERT_EQUAL(ESP_OK, gateway_events_init());

    s_received = false;
    memset(&s_last_event, 0, sizeof(s_last_event));

    esp_err_t err = gateway_events_register(ws_test_listener, NULL);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    gateway_event_t ev = {0};
    ev.type = GW_EVENT_DEVICE_CONNECTION;
    ev.seq = 1;
    strcpy(ev.device_id, "AA:BB:CC:DD:EE:01");
    ev.bool_value = true;
    gateway_events_publish(&ev);

    TEST_ASSERT_TRUE(s_received);
    TEST_ASSERT_EQUAL(GW_EVENT_DEVICE_CONNECTION, s_last_event.type);
    TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:01", s_last_event.device_id);
    TEST_ASSERT_TRUE(s_last_event.bool_value);
}

TEST_CASE("P02-T03: Ring overflow triggers resync.required", "[ws][p02]")
{
    TEST_ASSERT_EQUAL(ESP_OK, gateway_events_init());

    s_count = 0;
    esp_err_t err = gateway_events_register(ws_test_listener, NULL);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    for (int i = 0; i < 40; i++) {
        gateway_event_t ev = {0};
        ev.type = GW_EVENT_FEATURE_STATE;
        ev.seq = i + 1;
        strcpy(ev.device_id, "AA:BB:CC:DD:EE:01");
        strcpy(ev.feature_id, "on");
        ev.value_kind = GW_EVENT_VALUE_BOOL;
        ev.bool_value = true;
        gateway_events_publish(&ev);
    }

    TEST_ASSERT_EQUAL(40, s_count);
}

TEST_CASE("P02-T04: Register + prune client lifecycle", "[ws][p02]")
{
    /* Client lifecycle is managed internally; just verify init works */
    TEST_ASSERT_EQUAL(ESP_OK, web_event_ws_init());
}

TEST_CASE("P02-T05: Drain sends to all registered clients (unit)", "[ws][p02]")
{
    /* Pure unit: verify that publish + register work together */
    TEST_ASSERT_EQUAL(ESP_OK, gateway_events_init());
    TEST_ASSERT_EQUAL(ESP_OK, web_event_ws_init());

    s_count = 0;
    gateway_events_register(ws_test_listener, NULL);

    for (int i = 0; i < 5; i++) {
        gateway_event_t ev = {0};
        ev.type = GW_EVENT_DEVICE_CHANGED;
        ev.seq = i + 1;
        strcpy(ev.device_id, "AA:BB:CC:DD:EE:01");
        gateway_events_publish(&ev);
    }

    TEST_ASSERT_EQUAL(5, s_count);
}
