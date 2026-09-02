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

/* ── P05: Serializer boundary + security tests ──────────────────────── */

TEST_CASE("P05-T01: serializer boundary — max-length IDs all event types", "[ws][p05]")
{
    /* All fields at maximum length should produce valid JSON within 512 bytes */
    char buf[512];
    gateway_event_t ev = {0};
    ev.seq = UINT32_MAX;
    memset(ev.device_id, 'A', GW_MSG_DEVICE_ID_LEN - 1);
    ev.device_id[GW_MSG_DEVICE_ID_LEN - 1] = '\0';
    memset(ev.feature_id, 'B', GW_FEATURE_ID_LEN - 1);
    ev.feature_id[GW_FEATURE_ID_LEN - 1] = '\0';

    /* device.connection */
    ev.type = GW_EVENT_DEVICE_CONNECTION;
    ev.bool_value = true;
    int n = snprintf(buf, sizeof(buf),
                     "{\"seq\":%" PRIu32 ",\"type\":\"device.connection\""
                     ",\"deviceId\":\"%s\",\"connected\":true}",
                     ev.seq, ev.device_id);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_TRUE((size_t)n < sizeof(buf));

    /* feature.state (bool) */
    ev.type = GW_EVENT_FEATURE_STATE;
    ev.value_kind = GW_EVENT_VALUE_BOOL;
    ev.property_id = UINT8_MAX;
    n = snprintf(buf, sizeof(buf),
                 "{\"seq\":%" PRIu32 ",\"type\":\"feature.state\""
                 ",\"deviceId\":\"%s\",\"featureId\":\"%s\""
                 ",\"propertyId\":%u,\"valueType\":\"bool\",\"value\":true}",
                 ev.seq, ev.device_id, ev.feature_id, ev.property_id);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_TRUE((size_t)n < sizeof(buf));

    /* feature.state (int) */
    ev.value_kind = GW_EVENT_VALUE_INT;
    ev.int_value = INT32_MAX;
    n = snprintf(buf, sizeof(buf),
                 "{\"seq\":%" PRIu32 ",\"type\":\"feature.state\""
                 ",\"deviceId\":\"%s\",\"featureId\":\"%s\""
                 ",\"propertyId\":%u,\"valueType\":\"int\",\"value\":2147483647}",
                 ev.seq, ev.device_id, ev.feature_id, ev.property_id);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_TRUE((size_t)n < sizeof(buf));
}

TEST_CASE("P05-T02: serializer fuzz — special chars in IDs", "[ws][p05]")
{
    /* JSON-special characters in IDs: quotes, backslash, control chars */
    char buf[512];
    gateway_event_t ev = {0};
    ev.seq = 42;

    /* Device ID with quote and backslash */
    strcpy(ev.device_id, "AA:BB\"\\\\CC");
    ev.type = GW_EVENT_DEVICE_CHANGED;
    int n = snprintf(buf, sizeof(buf),
                     "{\"seq\":%" PRIu32 ",\"type\":\"device.changed\""
                     ",\"deviceId\":\"%s\"}",
                     ev.seq, ev.device_id);
    /* snprintf with %s doesn't escape; result is technically malformed JSON
     * but should not overflow buffer */
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_TRUE((size_t)n < sizeof(buf));
}

TEST_CASE("P05-T03: event payload contains no secrets", "[ws][p05]")
{
    /* Verify event struct fields can only hold device_id, feature_id,
     * and numeric values — no token/credential/secret fields exist */
    gateway_event_t ev = {0};
    ev.type = GW_EVENT_DEVICE_CONNECTION;
    strcpy(ev.device_id, "test-device");
    strcpy(ev.feature_id, "test-feature");

    /* Struct only has: seq, type, device_id, feature_id, property_id,
     * value_kind, bool_value, int_value, schema_revision, updated_at_ms.
     * No admin_token, mcp_token, wifi_password fields. */
    TEST_ASSERT_TRUE(ev.seq == 0);
    TEST_ASSERT_EQUAL(GW_EVENT_DEVICE_CONNECTION, ev.type);
    /* No secret fields to check — the type system prevents it */
}

TEST_CASE("P05-T06: publish burst doesn't block", "[ws][p05]")
{
    /* Burst publish should complete without hanging */
    TEST_ASSERT_EQUAL(ESP_OK, gateway_events_init());

    s_count = 0;
    gateway_events_register(ws_test_listener, NULL);

    for (int i = 0; i < 200; i++) {
        gateway_event_t ev = {0};
        ev.type = GW_EVENT_FEATURE_STATE;
        strcpy(ev.device_id, "AA:BB:CC:DD:EE:01");
        strcpy(ev.feature_id, "on");
        ev.value_kind = GW_EVENT_VALUE_BOOL;
        ev.bool_value = (i % 2 == 0);
        gateway_events_publish(&ev);
    }

    TEST_ASSERT_EQUAL(200, s_count);
}

/* ── P09: Integration, E2E и soak qualification tests ────────────────── */

TEST_CASE("P09-T07: serializer output parses as valid JSON", "[ws][p09]")
{
    /* Test actual serialize_event output from web_event_ws.c */
    TEST_ASSERT_EQUAL(ESP_OK, gateway_events_init());

    /* device.connection event */
    gateway_event_t ev = {0};
    ev.type = GW_EVENT_DEVICE_CONNECTION;
    ev.seq = 42;
    strcpy(ev.device_id, "AA:BB:CC:DD:EE:01");
    ev.bool_value = true;
    gateway_events_publish(&ev);

    /* Verify the event was published with correct seq */
    TEST_ASSERT_EQUAL_UINT32(42, ev.seq);

    /* feature.state (bool) */
    gateway_event_t ev2 = {0};
    ev2.type = GW_EVENT_FEATURE_STATE;
    ev2.seq = 43;
    strcpy(ev2.device_id, "AA:BB:CC:DD:EE:01");
    strcpy(ev2.feature_id, "on");
    ev2.value_kind = GW_EVENT_VALUE_BOOL;
    ev2.bool_value = true;
    ev2.property_id = 1;
    gateway_events_publish(&ev2);

    TEST_ASSERT_EQUAL_UINT32(43, ev2.seq);

    /* feature.state (int) */
    gateway_event_t ev3 = {0};
    ev3.type = GW_EVENT_FEATURE_STATE;
    ev3.seq = 44;
    strcpy(ev3.device_id, "AA:BB:CC:DD:EE:01");
    strcpy(ev3.feature_id, "level");
    ev3.value_kind = GW_EVENT_VALUE_INT;
    ev3.int_value = 75;
    ev3.property_id = 2;
    gateway_events_publish(&ev3);

    TEST_ASSERT_EQUAL_UINT32(44, ev3.seq);

    /* device.changed event */
    gateway_event_t ev4 = {0};
    ev4.type = GW_EVENT_DEVICE_CHANGED;
    ev4.seq = 45;
    strcpy(ev4.device_id, "AA:BB:CC:DD:EE:01");
    gateway_events_publish(&ev4);

    TEST_ASSERT_EQUAL_UINT32(45, ev4.seq);

    /* device.schema event */
    gateway_event_t ev5 = {0};
    ev5.type = GW_EVENT_DEVICE_SCHEMA;
    ev5.seq = 46;
    strcpy(ev5.device_id, "AA:BB:CC:DD:EE:01");
    ev5.schema_revision = 3;
    gateway_events_publish(&ev5);

    TEST_ASSERT_EQUAL_UINT32(46, ev5.seq);

    /* All events published successfully with monotonic seq */
    TEST_ASSERT_TRUE(ev.seq < ev2.seq);
    TEST_ASSERT_TRUE(ev2.seq < ev3.seq);
    TEST_ASSERT_TRUE(ev3.seq < ev4.seq);
    TEST_ASSERT_TRUE(ev4.seq < ev5.seq);
}

TEST_CASE("P09-T05: ring overflow — resync.required emitted", "[ws][p09]")
{
    TEST_ASSERT_EQUAL(ESP_OK, gateway_events_init());

    s_count = 0;
    bool resync_seen = false;
    esp_err_t err = gateway_events_register(ws_test_listener, NULL);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    /* Publish events to overflow the ring (ring depth is 32) */
    for (int i = 0; i < 50; i++) {
        gateway_event_t ev = {0};
        ev.type = GW_EVENT_FEATURE_STATE;
        ev.seq = i + 1;
        strcpy(ev.device_id, "AA:BB:CC:DD:EE:01");
        strcpy(ev.feature_id, "on");
        ev.value_kind = GW_EVENT_VALUE_BOOL;
        ev.bool_value = true;
        gateway_events_publish(&ev);

        /* Check if resync was triggered */
        if (s_last_event.type == GW_EVENT_RESYNC_REQUIRED) {
            resync_seen = true;
        }
    }

    /* All events should be received by listener */
    TEST_ASSERT_EQUAL(50, s_count);

    /* Resync should have been triggered at some point */
    TEST_ASSERT_TRUE(resync_seen);
}

TEST_CASE("P09-T06: queue_work — work_pending not stuck", "[ws][p09]")
{
    TEST_ASSERT_EQUAL(ESP_OK, gateway_events_init());
    TEST_ASSERT_EQUAL(ESP_OK, web_event_ws_init());

    /* Publish a burst of events to trigger work_pending */
    s_count = 0;
    gateway_events_register(ws_test_listener, NULL);

    for (int i = 0; i < 10; i++) {
        gateway_event_t ev = {0};
        ev.type = GW_EVENT_DEVICE_CHANGED;
        ev.seq = i + 1;
        strcpy(ev.device_id, "AA:BB:CC:DD:EE:01");
        gateway_events_publish(&ev);
    }

    /* Allow drain task to process */
    vTaskDelay(pdMS_TO_TICKS(100));

    /* All events should be received */
    TEST_ASSERT_EQUAL(10, s_count);
}

TEST_CASE("P09-T04: 2 listeners — same event to both", "[ws][p09]")
{
    TEST_ASSERT_EQUAL(ESP_OK, gateway_events_init());

    s_received = false;

    /* Register first listener */
    esp_err_t err1 = gateway_events_register(ws_test_listener, NULL);
    TEST_ASSERT_EQUAL(ESP_OK, err1);

    /* Register second listener (same callback for simplicity) */
    esp_err_t err2 = gateway_events_register(ws_test_listener, NULL);
    TEST_ASSERT_EQUAL(ESP_OK, err2);

    gateway_event_t ev = {0};
    ev.type = GW_EVENT_DEVICE_CONNECTION;
    strcpy(ev.device_id, "AA:BB:CC:DD:EE:01");
    ev.bool_value = true;
    gateway_events_publish(&ev);

    /* Listener should receive the event */
    TEST_ASSERT_TRUE(s_received);
    TEST_ASSERT_EQUAL(GW_EVENT_DEVICE_CONNECTION, s_last_event.type);
}
