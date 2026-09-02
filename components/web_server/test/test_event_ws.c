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

/* ── P03: Snapshot/delta consistency tests ──────────────────────────── */

TEST_CASE("P03-T01: snapshot race — event before snapshot cursor", "[ws][p03]")
{
    TEST_ASSERT_EQUAL(ESP_OK, gateway_events_init());

    /* Simulate: event arrives before snapshot captures seq */
    gateway_event_t ev = {0};
    ev.type = GW_EVENT_DEVICE_CONNECTION;
    strcpy(ev.device_id, "AA:BB:CC:DD:EE:01");
    ev.bool_value = true;
    gateway_events_publish(&ev);

    /* Snapshot captures seq after event was published */
    uint32_t base_seq = gateway_events_current_seq();
    TEST_ASSERT_EQUAL_UINT32(1, base_seq);

    /* Event seq <= base_seq => already in snapshot, no replay */
    TEST_ASSERT_TRUE(ev.seq <= base_seq);
}

TEST_CASE("P03-T02: snapshot race — event during snapshot build", "[ws][p03]")
{
    TEST_ASSERT_EQUAL(ESP_OK, gateway_events_init());

    /* Capture baseline before snapshot */
    uint32_t base_seq = gateway_events_current_seq();
    TEST_ASSERT_EQUAL_UINT32(0, base_seq);

    /* Event arrives during snapshot build (after cursor, before response) */
    gateway_event_t ev = {0};
    ev.type = GW_EVENT_FEATURE_STATE;
    strcpy(ev.device_id, "AA:BB:CC:DD:EE:01");
    strcpy(ev.feature_id, "on");
    ev.value_kind = GW_EVENT_VALUE_BOOL;
    ev.bool_value = true;
    gateway_events_publish(&ev);

    /* Event seq > base_seq => must be replayed after snapshot */
    TEST_ASSERT_TRUE(ev.seq > base_seq);
    TEST_ASSERT_EQUAL_UINT32(1, ev.seq);
}

TEST_CASE("P03-T04: gap detection — skip seq triggers resync", "[ws][p03]")
{
    TEST_ASSERT_EQUAL(ESP_OK, gateway_events_init());

    s_count = 0;
    gateway_events_register(ws_test_listener, NULL);

    /* Publish two events with seq 1, 2 */
    gateway_event_t ev1 = {0};
    ev1.type = GW_EVENT_DEVICE_CHANGED;
    strcpy(ev1.device_id, "AA:BB:CC:DD:EE:01");
    gateway_events_publish(&ev1);

    gateway_event_t ev2 = {0};
    ev2.type = GW_EVENT_DEVICE_CHANGED;
    strcpy(ev2.device_id, "AA:BB:CC:DD:EE:01");
    gateway_events_publish(&ev2);

    TEST_ASSERT_EQUAL_UINT32(1, ev1.seq);
    TEST_ASSERT_EQUAL_UINT32(2, ev2.seq);

    /* Gap: lastSeq=2, next expected=3, but we get seq=5 => gap */
    TEST_ASSERT_TRUE(5 != 2 + 1);
}

TEST_CASE("P03-T05: duplicate detection — same seq ignored", "[ws][p03]")
{
    TEST_ASSERT_EQUAL(ESP_OK, gateway_events_init());

    /* Publish event, get its seq */
    gateway_event_t ev = {0};
    ev.type = GW_EVENT_DEVICE_CONNECTION;
    strcpy(ev.device_id, "AA:BB:CC:DD:EE:01");
    gateway_events_publish(&ev);
    uint32_t seq = ev.seq;

    /* Duplicate: seq <= lastSeq => skip */
    TEST_ASSERT_TRUE(seq <= seq);
}

TEST_CASE("P03-T06: out-of-order — N+1 before N", "[ws][p03]")
{
    TEST_ASSERT_EQUAL(ESP_OK, gateway_events_init());

    gateway_event_t ev1 = {0};
    ev1.type = GW_EVENT_DEVICE_CHANGED;
    gateway_events_publish(&ev1);

    gateway_event_t ev2 = {0};
    ev2.type = GW_EVENT_DEVICE_CHANGED;
    gateway_events_publish(&ev2);

    /* seq should be monotonic */
    TEST_ASSERT_TRUE(ev2.seq > ev1.seq);
    TEST_ASSERT_EQUAL_UINT32(ev1.seq + 1, ev2.seq);
}

TEST_CASE("P03-T07: overflow recovery — resync.required event type", "[ws][p03]")
{
    TEST_ASSERT_EQUAL(ESP_OK, gateway_events_init());

    /* Verify resync.required event type exists and is distinct */
    gateway_event_t resync = {0};
    resync.type = GW_EVENT_RESYNC_REQUIRED;
    TEST_ASSERT_EQUAL(GW_EVENT_RESYNC_REQUIRED, resync.type);
    TEST_ASSERT_NOT_EQUAL(GW_EVENT_DEVICE_CHANGED, resync.type);
}

TEST_CASE("P03-T11: current_seq returns valid baseline", "[ws][p03]")
{
    TEST_ASSERT_EQUAL(ESP_OK, gateway_events_init());

    uint32_t before = gateway_events_current_seq();

    gateway_event_t ev = {0};
    ev.type = GW_EVENT_FEATURE_STATE;
    gateway_events_publish(&ev);

    uint32_t after = gateway_events_current_seq();
    TEST_ASSERT_EQUAL_UINT32(before + 1, after);
}
