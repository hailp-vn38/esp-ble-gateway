#include <stdio.h>
#include <string.h>

#include "device_management.h"
#include "device_management_internal.h"
#include "device_store.h"
#include "esp_timer.h"
#include "unity.h"

static int s_connect_rc;
static int s_forget_rc;
static esp_err_t s_schema_forget_rc;
static bool s_store_delete_fails;
static ble_central_device_status_t s_runtime;
static device_schema_snapshot_t s_schema;
static unsigned s_cancel_count;
static unsigned s_state_forget_count;
static unsigned s_publish_count;
static gateway_event_t s_last_event;

static int mock_connect(const char *id, const uint8_t *addr, uint8_t type)
{
    (void)id; (void)addr; (void)type;
    return s_connect_rc;
}

static ble_central_err_t mock_get_status(
    const char *id, ble_central_device_status_t *out)
{
    (void)id;
    *out = s_runtime;
    return BLE_CENTRAL_OK;
}

static int mock_forget(const char *id, const uint8_t *addr, uint8_t type,
                       bool has_addr)
{
    (void)id; (void)addr; (void)type; (void)has_addr;
    return s_forget_rc;
}

static esp_err_t mock_schema_get(const char *id,
                                 device_schema_snapshot_t *out)
{
    (void)id;
    *out = s_schema;
    return ESP_OK;
}

static esp_err_t mock_schema_forget(const char *id)
{
    (void)id;
    return s_schema_forget_rc;
}

static void mock_state_forget(const char *id)
{
    (void)id;
    s_state_forget_count++;
}

static esp_err_t mock_cancel(const char *id)
{
    (void)id;
    s_cancel_count++;
    return ESP_OK;
}

static device_store_result_t mock_store_delete(const char *id)
{
    return s_store_delete_fails ? DEVICE_STORE_ERR_PERSISTENCE
                                : device_store_delete(id);
}

static void mock_publish(gateway_event_t *event)
{
    s_publish_count++;
    s_last_event = *event;
}

static const device_management_hooks_t s_test_hooks = {
    .connect = mock_connect,
    .get_status = mock_get_status,
    .forget_peer = mock_forget,
    .schema_get = mock_schema_get,
    .schema_forget = mock_schema_forget,
    .state_forget = mock_state_forget,
    .cancel_commands = mock_cancel,
    .store_delete = mock_store_delete,
    .publish = mock_publish,
};

static void reset_test(void)
{
    device_entry_t entries[DEVICE_STORE_MAX_DEVICES];
    size_t count = 0;
    TEST_ASSERT_EQUAL(DEVICE_STORE_OK, device_store_snapshot(
                                               entries,
                                               DEVICE_STORE_MAX_DEVICES,
                                               &count));
    for (size_t i = 0; i < count; i++) {
        TEST_ASSERT_EQUAL(DEVICE_STORE_OK,
                          device_store_delete(entries[i].device_id));
    }
    s_connect_rc = BLE_CENTRAL_OK;
    s_forget_rc = BLE_CENTRAL_OK;
    s_schema_forget_rc = ESP_OK;
    s_store_delete_fails = false;
    memset(&s_runtime, 0, sizeof(s_runtime));
    memset(&s_schema, 0, sizeof(s_schema));
    s_cancel_count = 0;
    s_state_forget_count = 0;
    s_publish_count = 0;
    memset(&s_last_event, 0, sizeof(s_last_event));
    device_management_set_hooks(&s_test_hooks);
}

static device_mgmt_add_request_t make_add(const char *id, const char *name)
{
    device_mgmt_add_request_t request = {0};
    strlcpy(request.device_id, id, sizeof(request.device_id));
    if (name != NULL) strlcpy(request.name, name, sizeof(request.name));
    return request;
}

TEST_CASE("device management adds typed device and rejects duplicate id",
          "[device_management][phase3]")
{
    reset_test();
    device_mgmt_add_request_t request = make_add("dev1", "Lamp");
    device_mgmt_add_result_t result = device_management_add(&request);
    TEST_ASSERT_EQUAL(DEVICE_MGMT_OK, result.status);
    TEST_ASSERT_TRUE(result.persisted);
    TEST_ASSERT_FALSE(result.connect_requested);
    TEST_ASSERT_EQUAL(GW_EVENT_DEVICE_ADDED, s_last_event.type);
    TEST_ASSERT_EQUAL(DEVICE_MGMT_CONFLICT,
                      device_management_add(&request).status);
}

TEST_CASE("device management rejects duplicate BLE identity",
          "[device_management][phase3]")
{
    reset_test();
    device_mgmt_add_request_t first = make_add("dev1", "One");
    first.has_ble_identity = true;
    first.ble_addr[0] = 0xAA;
    first.ble_addr_type = 1;
    TEST_ASSERT_EQUAL(DEVICE_MGMT_OK, device_management_add(&first).status);
    device_mgmt_add_request_t second = make_add("dev2", "Two");
    second.has_ble_identity = true;
    memcpy(second.ble_addr, first.ble_addr, sizeof(second.ble_addr));
    second.ble_addr_type = first.ble_addr_type;
    TEST_ASSERT_EQUAL(DEVICE_MGMT_CONFLICT,
                      device_management_add(&second).status);
    device_entry_t entry;
    TEST_ASSERT_EQUAL(DEVICE_STORE_ERR_NOT_FOUND,
                      device_store_get("dev2", &entry));
}

TEST_CASE("device management reports store capacity",
          "[device_management][phase3]")
{
    reset_test();
    for (size_t i = 0; i < DEVICE_STORE_MAX_DEVICES; i++) {
        device_mgmt_add_request_t request = {0};
        snprintf(request.device_id, sizeof(request.device_id), "dev-%u",
                 (unsigned)i);
        TEST_ASSERT_EQUAL(DEVICE_MGMT_OK,
                          device_management_add(&request).status);
    }
    device_mgmt_add_request_t overflow = make_add("overflow", NULL);
    TEST_ASSERT_EQUAL(DEVICE_MGMT_CAPACITY,
                      device_management_add(&overflow).status);
}

TEST_CASE("device management edits typed device and reports missing",
          "[device_management][phase3]")
{
    reset_test();
    device_mgmt_add_request_t add = make_add("dev1", "Old");
    device_mgmt_add_result_t add_result = device_management_add(&add);
    TEST_ASSERT_EQUAL(DEVICE_MGMT_OK, add_result.status);
    TEST_ASSERT_FALSE(add_result.connect_requested);
    device_mgmt_edit_request_t edit = {0};
    strlcpy(edit.device_id, "dev1", sizeof(edit.device_id));
    strlcpy(edit.name, "New", sizeof(edit.name));
    device_mgmt_edit_result_t result = device_management_edit(&edit);
    TEST_ASSERT_EQUAL(DEVICE_MGMT_OK, result.status);
    TEST_ASSERT_TRUE(result.updated);
    TEST_ASSERT_EQUAL(GW_EVENT_DEVICE_RENAMED, s_last_event.type);
    strlcpy(edit.device_id, "missing", sizeof(edit.device_id));
    TEST_ASSERT_EQUAL(DEVICE_MGMT_NOT_FOUND,
                      device_management_edit(&edit).status);
}

TEST_CASE("inventory combines store BLE and schema snapshots",
          "[device_management][phase3]")
{
    reset_test();
    device_mgmt_add_request_t add = make_add("dev1", "Lamp");
    add.has_ble_identity = true;
    add.ble_addr[0] = 0x42;
    add.ble_addr_type = 1;
    device_mgmt_add_result_t add_result = device_management_add(&add);
    TEST_ASSERT_EQUAL(DEVICE_MGMT_OK, add_result.status);
    TEST_ASSERT_TRUE(add_result.connect_requested);
    s_runtime.connected = true;
    s_runtime.ready = true;
    s_schema.has_committed = true;
    s_schema.state = DEVICE_SCHEMA_STATE_READY;
    s_schema.revision = 7;
    s_schema.tool_count = 2;
    s_schema.feature_count = 3;
    s_schema.features[0].writable_tool_index = 0;
    s_schema.features[1].writable_tool_index = 1;
    s_schema.features[2].writable_tool_index = -1;

    device_inventory_entry_t entries[DEVICE_STORE_MAX_DEVICES];
    size_t count = 0;
    TEST_ASSERT_EQUAL(DEVICE_MGMT_OK, device_management_snapshot(
                                          entries, DEVICE_STORE_MAX_DEVICES,
                                          &count));
    TEST_ASSERT_EQUAL_UINT32(1, count);
    TEST_ASSERT_EQUAL_STRING("dev1", entries[0].device_id);
    TEST_ASSERT_TRUE(entries[0].connected);
    TEST_ASSERT_TRUE(entries[0].ready);
    TEST_ASSERT_TRUE(entries[0].schema_available);
    TEST_ASSERT_EQUAL_UINT32(7, entries[0].schema_revision);
    TEST_ASSERT_EQUAL_UINT8(3, entries[0].feature_count);
    TEST_ASSERT_EQUAL_UINT8(2, entries[0].writable_feature_count);
}

TEST_CASE("delete aborts after schema failure",
          "[device_management][phase3]")
{
    reset_test();
    device_mgmt_add_request_t add = make_add("dev1", NULL);
    TEST_ASSERT_EQUAL(DEVICE_MGMT_OK, device_management_add(&add).status);
    s_schema_forget_rc = ESP_FAIL;
    device_mgmt_delete_result_t result = device_management_delete("dev1");
    TEST_ASSERT_EQUAL(DEVICE_MGMT_INTERNAL, result.status);
    TEST_ASSERT_TRUE(result.command_cancel_requested);
    TEST_ASSERT_FALSE(result.schema_forgotten);
    TEST_ASSERT_EQUAL_UINT32(0, s_state_forget_count);
    device_entry_t entry;
    TEST_ASSERT_EQUAL(DEVICE_STORE_OK, device_store_get("dev1", &entry));
}

TEST_CASE("delete reports degraded BLE and store cleanup",
          "[device_management][phase3]")
{
    reset_test();
    device_mgmt_add_request_t add = make_add("dev1", NULL);
    TEST_ASSERT_EQUAL(DEVICE_MGMT_OK, device_management_add(&add).status);
    s_forget_rc = BLE_CENTRAL_ERR_STACK;
    device_mgmt_delete_result_t result = device_management_delete("dev1");
    TEST_ASSERT_EQUAL(DEVICE_MGMT_DEGRADED, result.status);
    TEST_ASSERT_FALSE(result.ble_peer_forgotten);
    TEST_ASSERT_TRUE(result.store_deleted);

    reset_test();
    TEST_ASSERT_EQUAL(DEVICE_MGMT_OK, device_management_add(&add).status);
    s_store_delete_fails = true;
    result = device_management_delete("dev1");
    TEST_ASSERT_EQUAL(DEVICE_MGMT_DEGRADED, result.status);
    TEST_ASSERT_TRUE(result.ble_peer_forgotten);
    TEST_ASSERT_FALSE(result.store_deleted);
    TEST_ASSERT_EQUAL(GW_EVENT_DEVICE_CHANGED, s_last_event.type);
}

TEST_CASE("delete cancels commands publishes lifecycle and is idempotent",
          "[device_management][phase3]")
{
    reset_test();
    device_mgmt_add_request_t add = make_add("dev1", NULL);
    TEST_ASSERT_EQUAL(DEVICE_MGMT_OK, device_management_add(&add).status);
    s_publish_count = 0;
    device_mgmt_delete_result_t result = device_management_delete("dev1");
    TEST_ASSERT_EQUAL(DEVICE_MGMT_OK, result.status);
    TEST_ASSERT_EQUAL_UINT32(1, s_cancel_count);
    TEST_ASSERT_TRUE(result.command_cancel_requested);
    TEST_ASSERT_TRUE(result.schema_forgotten);
    TEST_ASSERT_TRUE(result.state_forgotten);
    TEST_ASSERT_TRUE(result.ble_peer_forgotten);
    TEST_ASSERT_TRUE(result.store_deleted);
    TEST_ASSERT_EQUAL_UINT32(1, s_publish_count);
    TEST_ASSERT_EQUAL(GW_EVENT_DEVICE_REMOVED, s_last_event.type);
    TEST_ASSERT_EQUAL(DEVICE_MGMT_NOT_FOUND,
                      device_management_delete("dev1").status);
}

TEST_CASE("device CRUD returns without BLE wait",
          "[device_management][phase3]")
{
    reset_test();
    device_mgmt_add_request_t add = make_add("dev1", NULL);
    add.has_ble_identity = true;
    add.ble_addr[0] = 1;
    int64_t started = esp_timer_get_time();
    TEST_ASSERT_EQUAL(DEVICE_MGMT_OK, device_management_add(&add).status);
    device_mgmt_edit_request_t edit = {0};
    strlcpy(edit.device_id, "dev1", sizeof(edit.device_id));
    strlcpy(edit.name, "Renamed", sizeof(edit.name));
    TEST_ASSERT_EQUAL(DEVICE_MGMT_OK, device_management_edit(&edit).status);
    TEST_ASSERT_EQUAL(DEVICE_MGMT_OK, device_management_delete("dev1").status);
    TEST_ASSERT_LESS_THAN_INT64(500000, esp_timer_get_time() - started);
}
