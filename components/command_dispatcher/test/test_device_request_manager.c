#include <string.h>

#include "unity.h"

#include "cbor_codec.h"
#include "../device_request_manager.h"

static gw_message_t make_response(const char *type, const char *device_id,
                                  const char *command, uint32_t request_id)
{
    gw_message_t response = {.protocol_version = GW_PROTOCOL_VERSION};
    strlcpy(response.type, type, sizeof(response.type));
    strlcpy(response.device_id, device_id, sizeof(response.device_id));
    strlcpy(response.command, command, sizeof(response.command));
    response.has_device_id = 1;
    if (request_id != 0) {
        response.request_id = request_id;
        response.has_request_id = 1;
    }
    return response;
}

static pending_request_t *allocate(const char *device_id, const char *command)
{
    pending_request_t *request = NULL;
    TEST_ASSERT_EQUAL_INT(0, device_request_allocate(device_id, command,
                                                     &request));
    TEST_ASSERT_NOT_NULL(request);
    TEST_ASSERT_TRUE(request->in_use);
    TEST_ASSERT_NOT_EQUAL(0, request->request_id);
    return request;
}

TEST_CASE("request manager allocates unique ids per request", "[dispatcher]")
{
    TEST_ASSERT_EQUAL_INT(0, device_request_manager_init());
    pending_request_t *a = allocate("relay-1", "set_power");
    uint32_t first_id = a->request_id;
    device_request_release(a);

    pending_request_t *b = allocate("relay-2", "set_power");
    // IDs are monotonic: even reusing the freed slot must yield a new id.
    TEST_ASSERT_NOT_EQUAL(first_id, b->request_id);
    device_request_release(b);
}

TEST_CASE("request manager completes matching ACK (case 1)", "[dispatcher]")
{
    TEST_ASSERT_EQUAL_INT(0, device_request_manager_init());
    pending_request_t *request = allocate("relay-1", "set_power");

    gw_message_t ack = make_response("device_ack", "relay-1", "set_power",
                                     request->request_id);
    TEST_ASSERT_TRUE(device_request_complete("relay-1", &ack));
    // Completion must wake the waiter immediately.
    TEST_ASSERT_EQUAL_INT(0, device_request_wait(request, 0));
    TEST_ASSERT_EQUAL_MEMORY(&ack, &request->response, sizeof(ack));

    // Duplicate completion is ignored.
    TEST_ASSERT_FALSE(device_request_complete("relay-1", &ack));
    device_request_release(request);
}

TEST_CASE("request manager rejects wrong request id (case 2)", "[dispatcher]")
{
    TEST_ASSERT_EQUAL_INT(0, device_request_manager_init());
    pending_request_t *request = allocate("relay-1", "set_power");

    gw_message_t ack = make_response("device_ack", "relay-1", "set_power",
                                     request->request_id + 1);
    TEST_ASSERT_FALSE(device_request_complete("relay-1", &ack));
    TEST_ASSERT_EQUAL_INT(-1, device_request_wait(request, 0));
    device_request_release(request);
}

TEST_CASE("request manager rejects wrong device (case 3)", "[dispatcher]")
{
    TEST_ASSERT_EQUAL_INT(0, device_request_manager_init());
    pending_request_t *request = allocate("relay-1", "set_power");

    gw_message_t ack = make_response("device_ack", "relay-2", "set_power",
                                     request->request_id);
    TEST_ASSERT_FALSE(device_request_complete("relay-2", &ack));
    TEST_ASSERT_EQUAL_INT(-1, device_request_wait(request, 0));
    device_request_release(request);
}

TEST_CASE("device event never completes a request (case 4)", "[dispatcher]")
{
    TEST_ASSERT_EQUAL_INT(0, device_request_manager_init());
    pending_request_t *request = allocate("relay-1", "set_power");

    gw_message_t event = make_response("device_event", "relay-1", "set_power",
                                       request->request_id);
    TEST_ASSERT_FALSE(device_request_complete("relay-1", &event));
    TEST_ASSERT_EQUAL_INT(-1, device_request_wait(request, 0));
    device_request_release(request);
}

TEST_CASE("ACK with mismatched command is rejected as protocol error (case 5)",
          "[dispatcher]")
{
    TEST_ASSERT_EQUAL_INT(0, device_request_manager_init());
    pending_request_t *request = allocate("relay-1", "set_power");

    gw_message_t ack = make_response("device_ack", "relay-1", "other_command",
                                     request->request_id);
    TEST_ASSERT_FALSE(device_request_complete("relay-1", &ack));
    TEST_ASSERT_EQUAL_INT(-1, device_request_wait(request, 0));
    device_request_release(request);
}

TEST_CASE("stale ACK does not complete the next request (case 6)",
          "[dispatcher]")
{
    TEST_ASSERT_EQUAL_INT(0, device_request_manager_init());

    pending_request_t *a = allocate("relay-1", "set_power");
    uint32_t stale_id = a->request_id;
    device_request_release(a); // simulates timeout of A

    pending_request_t *b = allocate("relay-1", "set_power");
    TEST_ASSERT_NOT_EQUAL(stale_id, b->request_id);

    gw_message_t stale_ack = make_response("device_ack", "relay-1", "set_power",
                                           stale_id);
    TEST_ASSERT_FALSE(device_request_complete("relay-1", &stale_ack));

    gw_message_t fresh_ack = make_response("device_ack", "relay-1", "set_power",
                                           b->request_id);
    TEST_ASSERT_TRUE(device_request_complete("relay-1", &fresh_ack));
    TEST_ASSERT_EQUAL_INT(0, device_request_wait(b, 0));
    device_request_release(b);
}

TEST_CASE("two devices correlate independently (case 7)", "[dispatcher]")
{
    TEST_ASSERT_EQUAL_INT(0, device_request_manager_init());
    pending_request_t *a = allocate("relay-1", "set_power");
    pending_request_t *b = allocate("relay-2", "set_power");

    gw_message_t ack_b = make_response("device_ack", "relay-2", "set_power",
                                       b->request_id);
    TEST_ASSERT_TRUE(device_request_complete("relay-2", &ack_b));
    TEST_ASSERT_EQUAL_INT(0, device_request_wait(b, 0));
    TEST_ASSERT_EQUAL_INT(-1, device_request_wait(a, 0));

    gw_message_t ack_a = make_response("device_ack", "relay-1", "set_power",
                                       a->request_id);
    TEST_ASSERT_TRUE(device_request_complete("relay-1", &ack_a));
    TEST_ASSERT_EQUAL_INT(0, device_request_wait(a, 0));

    device_request_release(a);
    device_request_release(b);
}

TEST_CASE("second command for same device reports busy (case 8)",
          "[dispatcher]")
{
    TEST_ASSERT_EQUAL_INT(0, device_request_manager_init());
    pending_request_t *a = allocate("relay-1", "set_power");
    pending_request_t *b = NULL;
    TEST_ASSERT_EQUAL_INT(-2, device_request_allocate("relay-1", "set_power",
                                                      &b));
    device_request_release(a);

    // Slot reusable after release.
    TEST_ASSERT_EQUAL_INT(0, device_request_allocate("relay-1", "set_power",
                                                     &b));
    device_request_release(b);
}

TEST_CASE("malformed ACKs are rejected", "[dispatcher]")
{
    TEST_ASSERT_EQUAL_INT(0, device_request_manager_init());
    pending_request_t *request = allocate("relay-1", "set_power");

    gw_message_t no_id = make_response("device_ack", "relay-1", "set_power", 0);
    TEST_ASSERT_FALSE(device_request_complete("relay-1", &no_id));

    gw_message_t empty_device =
        make_response("device_ack", "", "set_power", request->request_id);
    TEST_ASSERT_FALSE(device_request_complete("relay-1", &empty_device));
}
