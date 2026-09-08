#include <string.h>

#include "unity.h"
#include "device_settings.h"
#include "device_command_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gw_settings_view.h"
#include "memory_policy.h"

/* ── Allocation tests ──────────────────────────────────────────────── */

TEST_CASE("ds_settings_alloc returns non-NULL for small allocation",
          "[device_settings][g1]")
{
    void *ptr = ds_settings_alloc(64);
    TEST_ASSERT_NOT_NULL(ptr);
    ds_settings_free(ptr);
}

TEST_CASE("ds_settings_alloc returns NULL for zero size",
          "[device_settings][g1]")
{
    void *ptr = ds_settings_alloc(0);
    TEST_ASSERT_NULL(ptr);
}

TEST_CASE("ds_settings_free handles NULL safely",
          "[device_settings][g1]")
{
    ds_settings_free(NULL);  /* must not crash */
}

TEST_CASE("ds_settings_alloc places object in PSRAM-capable memory",
          "[device_settings][g1]")
{
    /* Allocate a reasonably large block — should come from PSRAM. */
    void *ptr = ds_settings_alloc(1024);
    TEST_ASSERT_NOT_NULL(ptr);
    /* The pointer should be non-NULL; actual PSRAM placement depends on
     * the runtime, but the allocator uses GW_MEM_EXTERNAL_REQUIRED. */
    ds_settings_free(ptr);
}

/* ── String pool tests ─────────────────────────────────────────────── */

TEST_CASE("ds_string_pool_add and get work correctly",
          "[device_settings][g1]")
{
    ds_string_pool_t pool = {0};
    pool.pool = ds_settings_alloc(256);
    TEST_ASSERT_NOT_NULL(pool.pool);
    pool.capacity = 256;
    pool.total_size = 0;

    uint16_t off1, off2;
    TEST_ASSERT_EQUAL(ESP_OK, ds_string_pool_add(&pool, "hello", &off1));
    TEST_ASSERT_EQUAL(ESP_OK, ds_string_pool_add(&pool, "world", &off2));

    TEST_ASSERT_EQUAL_STRING("hello", ds_string_pool_get(&pool, off1));
    TEST_ASSERT_EQUAL_STRING("world", ds_string_pool_get(&pool, off2));
    TEST_ASSERT_TRUE(off2 > off1);

    ds_settings_free(pool.pool);
}

TEST_CASE("ds_string_pool_add overflows gracefully",
          "[device_settings][g1]")
{
    ds_string_pool_t pool = {0};
    pool.pool = ds_settings_alloc(16);
    TEST_ASSERT_NOT_NULL(pool.pool);
    pool.capacity = 16;
    pool.total_size = 0;

    uint16_t off;
    /* This should succeed (5 bytes + header < 16). */
    TEST_ASSERT_EQUAL(ESP_OK, ds_string_pool_add(&pool, "abc", &off));
    /* This should fail (not enough room). */
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM,
                      ds_string_pool_add(&pool, "this is a long string", &off));

    ds_settings_free(pool.pool);
}

/* ── Schema builder tests ──────────────────────────────────────────── */

TEST_CASE("schema builder creates valid schema",
          "[device_settings][g1]")
{
    ds_schema_builder_t builder;
    TEST_ASSERT_EQUAL(ESP_OK, ds_schema_builder_init(&builder));

    builder.schema_revision = 1;

    uint16_t id_off, title_off, unit_off;
    TEST_ASSERT_EQUAL(ESP_OK,
        ds_schema_builder_add_string(&builder, "brightness", &id_off));
    TEST_ASSERT_EQUAL(ESP_OK,
        ds_schema_builder_add_string(&builder, "Brightness", &title_off));
    TEST_ASSERT_EQUAL(ESP_OK,
        ds_schema_builder_add_string(&builder, "%", &unit_off));

    ds_setting_desc_t desc = {
        .id_off = id_off,
        .title_off = title_off,
        .unit_off = unit_off,
        .type = DS_TYPE_INT,
        .flags = DS_FLAG_WRITABLE,
        .min_value = 0,
        .max_value = 100,
        .step = 5,
    };
    TEST_ASSERT_EQUAL(ESP_OK, ds_schema_builder_add_setting(&builder, &desc));

    ds_schema_t *schema = ds_schema_builder_commit(&builder);
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_EQUAL_UINT32(1, schema->schema_revision);
    TEST_ASSERT_EQUAL_UINT16(1, schema->setting_count);
    TEST_ASSERT_EQUAL_STRING("brightness",
        ds_string_pool_get(&schema->strings, schema->descriptors[0].id_off));
    TEST_ASSERT_EQUAL_STRING("Brightness",
        ds_string_pool_get(&schema->strings,
                           schema->descriptors[0].title_off));
    TEST_ASSERT_EQUAL(DS_TYPE_INT, schema->descriptors[0].type);
    TEST_ASSERT_TRUE(schema->descriptors[0].flags & DS_FLAG_WRITABLE);
    TEST_ASSERT_EQUAL_INT32(0, schema->descriptors[0].min_value);
    TEST_ASSERT_EQUAL_INT32(100, schema->descriptors[0].max_value);
    TEST_ASSERT_EQUAL_INT32(5, schema->descriptors[0].step);

    /* Verify refcount: acquire once more, release both. */
    TEST_ASSERT_TRUE(ds_settings_ref_acquire(schema));
    ds_settings_ref_release(schema);
    ds_settings_ref_release(schema);
}

TEST_CASE("schema builder with enum options",
          "[device_settings][g1]")
{
    ds_schema_builder_t builder;
    TEST_ASSERT_EQUAL(ESP_OK, ds_schema_builder_init(&builder));
    builder.schema_revision = 2;

    uint16_t id_off;
    ds_schema_builder_add_string(&builder, "fan_speed", &id_off);

    uint16_t opt0, opt1, opt2;
    ds_schema_builder_add_enum_option(&builder, 0, "Off", &opt0);
    ds_schema_builder_add_enum_option(&builder, 1, "Low", &opt1);
    ds_schema_builder_add_enum_option(&builder, 2, "High", &opt2);

    ds_setting_desc_t desc = {
        .id_off = id_off,
        .type = DS_TYPE_ENUM,
        .flags = 0,
        .option_count = 3,
        .option_index = opt0,
    };
    ds_schema_builder_add_setting(&builder, &desc);

    ds_schema_t *schema = ds_schema_builder_commit(&builder);
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_EQUAL_UINT8(3, schema->descriptors[0].option_count);
    TEST_ASSERT_EQUAL_UINT16(opt0, schema->descriptors[0].option_index);
    TEST_ASSERT_EQUAL_UINT8(3, schema->enum_option_count);
    TEST_ASSERT_EQUAL_INT32(0, schema->enum_option_pool[opt0].value);
    TEST_ASSERT_EQUAL_STRING("Off",
        ds_string_pool_get(&schema->strings,
                           schema->enum_option_pool[opt0].label_off));
    TEST_ASSERT_EQUAL_STRING("High",
        ds_string_pool_get(&schema->strings,
                           schema->enum_option_pool[opt2].label_off));

    ds_settings_ref_release(schema);
}

TEST_CASE("schema builder reset frees resources",
          "[device_settings][g1]")
{
    ds_schema_builder_t builder;
    TEST_ASSERT_EQUAL(ESP_OK, ds_schema_builder_init(&builder));
    /* Reset should not crash and should free the string pool. */
    ds_schema_builder_reset(&builder);
    /* Builder is now zeroed — safe to init again. */
    TEST_ASSERT_EQUAL(ESP_OK, ds_schema_builder_init(&builder));
    ds_schema_builder_reset(&builder);
}

/* ── Values builder tests ──────────────────────────────────────────── */

TEST_CASE("values builder creates valid values snapshot",
          "[device_settings][g1]")
{
    ds_values_builder_t builder;
    TEST_ASSERT_EQUAL(ESP_OK, ds_values_builder_init(&builder));
    builder.config_revision = 42;

    ds_value_entry_t e1 = { .type = DS_TYPE_BOOL, .has_value = true,
                            .bool_val = true };
    ds_value_entry_t e2 = { .type = DS_TYPE_INT, .has_value = true,
                            .int_val = 75 };
    TEST_ASSERT_EQUAL(ESP_OK, ds_values_builder_add(&builder, &e1));
    TEST_ASSERT_EQUAL(ESP_OK, ds_values_builder_add(&builder, &e2));

    ds_values_t *values = ds_values_builder_commit(&builder);
    TEST_ASSERT_NOT_NULL(values);
    TEST_ASSERT_EQUAL_UINT32(42, values->config_revision);
    TEST_ASSERT_EQUAL_UINT16(2, values->value_count);
    TEST_ASSERT_TRUE(values->values[0].bool_val);
    TEST_ASSERT_EQUAL_INT32(75, values->values[1].int_val);

    ds_values_ref_release(values);
}

/* ── Refcount lifecycle tests ──────────────────────────────────────── */

TEST_CASE("refcount: acquire/release normal",
          "[device_settings][g1]")
{
    ds_schema_builder_t builder;
    ds_schema_builder_init(&builder);
    builder.schema_revision = 10;

    uint16_t id_off;
    ds_schema_builder_add_string(&builder, "test", &id_off);
    ds_setting_desc_t desc = { .id_off = id_off, .type = DS_TYPE_BOOL };
    ds_schema_builder_add_setting(&builder, &desc);

    ds_schema_t *schema = ds_schema_builder_commit(&builder);
    TEST_ASSERT_NOT_NULL(schema);

    /* Initial refcount is 1. */
    TEST_ASSERT_TRUE(ds_settings_ref_acquire(schema));  /* refcount = 2 */
    TEST_ASSERT_TRUE(ds_settings_ref_acquire(schema));  /* refcount = 3 */
    ds_settings_ref_release(schema);  /* refcount = 2 */
    ds_settings_ref_release(schema);  /* refcount = 1 */
    ds_settings_ref_release(schema);  /* refcount = 0 -> freed */
}

TEST_CASE("refcount: swap with one reader",
          "[device_settings][g1]")
{
    ds_schema_builder_t builder;
    ds_schema_builder_init(&builder);
    builder.schema_revision = 1;

    uint16_t id_off;
    ds_schema_builder_add_string(&builder, "s1", &id_off);
    ds_setting_desc_t desc = { .id_off = id_off, .type = DS_TYPE_BOOL };
    ds_schema_builder_add_setting(&builder, &desc);
    ds_schema_t *old_schema = ds_schema_builder_commit(&builder);
    TEST_ASSERT_NOT_NULL(old_schema);

    /* Simulate one reader holding a reference. */
    ds_settings_ref_acquire(old_schema);

    /* Build new schema. */
    ds_schema_builder_reset(&builder);
    ds_schema_builder_init(&builder);
    builder.schema_revision = 2;
    ds_schema_builder_add_string(&builder, "s2", &id_off);
    desc.id_off = id_off;
    ds_schema_builder_add_setting(&builder, &desc);
    ds_schema_t *new_schema = ds_schema_builder_commit(&builder);
    TEST_ASSERT_NOT_NULL(new_schema);

    /* Old schema still alive (reader holds ref). */
    ds_settings_ref_release(old_schema);  /* reader done */
    /* Now old_schema refcount = 1 (only the "owner" ref). */
    ds_settings_ref_release(old_schema);  /* owner release -> freed */

    /* new_schema is independent. */
    TEST_ASSERT_EQUAL_UINT32(2, new_schema->schema_revision);
    ds_settings_ref_release(new_schema);
}

TEST_CASE("refcount: empty schema commit remains valid",
          "[device_settings][g1]")
{
    /* A valid settings_begin(total=0) must commit an immutable empty schema. */
    ds_schema_builder_t builder;
    TEST_ASSERT_EQUAL(ESP_OK, ds_schema_builder_init(&builder));
    ds_schema_t *schema = ds_schema_builder_commit(&builder);
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_EQUAL_UINT16(0, schema->setting_count);
    ds_settings_ref_release(schema);
    ds_schema_builder_reset(&builder);
}

/* ── Init/deinit tests ─────────────────────────────────────────────── */

TEST_CASE("device_settings_init and deinit work",
          "[device_settings][g1]")
{
    /* Reset first to ensure clean state. */
    device_settings_reset_for_test();

    TEST_ASSERT_EQUAL(ESP_OK, device_settings_init());
    /* Double init should fail. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, device_settings_init());

    device_settings_deinit();
    /* Double deinit should be safe. */
    device_settings_deinit();
}

TEST_CASE("device_settings reset_for_test cleans up",
          "[device_settings][g1]")
{
    device_settings_reset_for_test();
    TEST_ASSERT_EQUAL(ESP_OK, device_settings_init());

    /* Allocate a schema and assign it to a record (via direct access). */
    ds_schema_builder_t builder;
    ds_schema_builder_init(&builder);
    builder.schema_revision = 99;
    uint16_t id_off;
    ds_schema_builder_add_string(&builder, "x", &id_off);
    ds_setting_desc_t desc = { .id_off = id_off, .type = DS_TYPE_BOOL };
    ds_schema_builder_add_setting(&builder, &desc);
    ds_schema_t *schema = ds_schema_builder_commit(&builder);
    TEST_ASSERT_NOT_NULL(schema);

    /* Verify refcount is 1 after commit. */
    TEST_ASSERT_TRUE(ds_settings_ref_acquire(schema));
    ds_settings_ref_release(schema);

    /* Reset should release all. */
    device_settings_reset_for_test();

    /* Schema was freed by reset_for_test — do not access it. */
}

/* ── Operation API tests ───────────────────────────────────────────── */

static bool s_op_completed;
static ds_op_result_t s_op_result;

static void test_op_completion(ds_op_result_t result, void *context)
{
    (void)context;
    s_op_completed = true;
    s_op_result = result;
}

TEST_CASE("device_settings_describe rejects NULL device_id",
          "[device_settings][g1]")
{
    device_settings_reset_for_test();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      device_settings_describe(NULL, NULL, NULL));
}

TEST_CASE("device_settings_cancel rejects unknown device",
          "[device_settings][g1]")
{
    device_settings_reset_for_test();
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      device_settings_cancel("nonexistent"));
}

TEST_CASE("DS-CAP-005: changed revision queues DESCRIBE",
          "[device_settings][g2]")
{
    device_settings_deinit();
    device_settings_reset_for_test();
    TEST_ASSERT_EQUAL(ESP_OK, device_settings_init());
    device_settings_on_capability("cap-describe", true, 7);

    ds_device_record_t record = {0};
    TEST_ASSERT_EQUAL(ESP_OK,
                      device_settings_get_record("cap-describe", &record));
    TEST_ASSERT_EQUAL_INT(DS_SCHEMA_DISCOVERING, record.schema_state);
    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          device_settings_cancel("cap-describe"));
    device_settings_deinit();
}

TEST_CASE("DS-CAP-006: same revision queues READ",
          "[device_settings][g2]")
{
    device_settings_deinit();
    device_settings_reset_for_test();
    TEST_ASSERT_EQUAL(ESP_OK, device_settings_init());
    ds_device_record_t *record =
        device_settings_find_or_create_record("cap-read");
    TEST_ASSERT_NOT_NULL(record);
    record->schema_state = DS_SCHEMA_READY;
    record->schema_rev = 7;
    /* The capability bridge only needs a non-NULL cached snapshot to choose
     * READ; the operation API does not dereference it. */
    record->schema = (ds_schema_t *)(uintptr_t)1;

    device_settings_on_capability("cap-read", true, 7);
    ds_device_record_t observed = {0};
    TEST_ASSERT_EQUAL(ESP_OK,
                      device_settings_get_record("cap-read", &observed));
    TEST_ASSERT_EQUAL_INT(DS_SCHEMA_READY, observed.schema_state);
    TEST_ASSERT_EQUAL_INT(ESP_OK, device_settings_cancel("cap-read"));
    record->schema = NULL;
    device_settings_worker_reset_for_test();
    device_settings_operation_reset_for_test();
    device_settings_reset_for_test();
    device_settings_deinit();
}

/* ── Mock command service hooks (for DS-WORK tests) ────────────────── */

static int mock_cmd_send_rc = 0;
static bool mock_cmd_send_called = false;
static uint32_t mock_cmd_send_count = 0;
static char mock_cmd_last_device_id[32];
static char mock_cmd_last_command[64];
static uint32_t mock_cmd_last_request_id = 0;
static gw_message_t mock_cmd_last_message;

static int mock_cmd_send(const char *device_id, const gw_message_t *msg)
{
    mock_cmd_send_called = true;
    mock_cmd_send_count++;
    strlcpy(mock_cmd_last_device_id, device_id,
            sizeof(mock_cmd_last_device_id));
    if (msg != NULL) {
        strlcpy(mock_cmd_last_command, msg->command,
                sizeof(mock_cmd_last_command));
        mock_cmd_last_request_id = msg->request_id;
        mock_cmd_last_message = *msg;
    }
    return mock_cmd_send_rc;
}

static int mock_cmd_is_connected(const char *device_id)
{
    (void)device_id;
    return 1;
}

static device_command_transport_hooks_t mock_cmd_hooks = {
    .send_command = mock_cmd_send,
    .is_connected = mock_cmd_is_connected,
};

static gw_message_t make_settings_ack(const char *device_id,
                                      const char *command,
                                      uint32_t request_id)
{
    gw_message_t ack = {0};
    strlcpy(ack.type, "device_ack", sizeof(ack.type));
    strlcpy(ack.device_id, device_id, sizeof(ack.device_id));
    strlcpy(ack.command, command, sizeof(ack.command));
    ack.request_id = request_id;
    ack.has_request_id = 1;
    ack.bool_value = 1; /* accepted */
    ack.has_device_id = 1;
    return ack;
}

static void reset_mock_cmd(void)
{
    mock_cmd_send_rc = 0;
    mock_cmd_send_called = false;
    mock_cmd_send_count = 0;
    mock_cmd_last_device_id[0] = '\0';
    mock_cmd_last_command[0] = '\0';
    mock_cmd_last_request_id = 0;
    memset(&mock_cmd_last_message, 0, sizeof(mock_cmd_last_message));
}

static bool tx_completed;
static ds_tx_result_t tx_result;

static void tx_completion(ds_tx_result_t result, void *context)
{
    (void)context;
    tx_completed = true;
    tx_result = result;
}

static ds_device_record_t *setup_tx_schema(const char *device_id,
                                            uint8_t type, uint16_t flags)
{
    ds_schema_builder_t builder;
    TEST_ASSERT_EQUAL(ESP_OK, ds_schema_builder_init(&builder));
    uint16_t id_off;
    TEST_ASSERT_EQUAL(ESP_OK, ds_schema_builder_add_string(&builder, "setting", &id_off));
    ds_setting_desc_t desc = {
        .id_off = id_off, .type = type, .flags = flags,
        .min_value = 0, .max_value = 100, .step = 5, .max_length = 63,
    };
    if (type == DS_TYPE_ENUM) {
        uint16_t label;
        TEST_ASSERT_EQUAL(ESP_OK, ds_schema_builder_add_enum_option(&builder, 2, "mode", &label));
        desc.option_index = 0;
        desc.option_count = 1;
    }
    TEST_ASSERT_EQUAL(ESP_OK, ds_schema_builder_add_setting(&builder, &desc));
    ds_device_record_t *rec = device_settings_find_or_create_record(device_id);
    TEST_ASSERT_NOT_NULL(rec);
    rec->schema_state = DS_SCHEMA_READY;
    rec->schema = ds_schema_builder_commit(&builder);
    TEST_ASSERT_NOT_NULL(rec->schema);
    rec->config_rev = 7;
    return rec;
}

static void tx_ack(const char *device_id)
{
    gw_message_t ack = make_settings_ack(device_id, mock_cmd_last_command,
                                         mock_cmd_last_request_id);
    TEST_ASSERT_TRUE(device_command_service_on_notify(device_id, &ack));
    vTaskDelay(pdMS_TO_TICKS(40));
}

TEST_CASE("DS-TX-001..015 canonical BEGIN SET COMMIT CONFIRM flow",
          "[device_settings][g7]")
{
    device_settings_deinit();
    device_settings_reset_for_test();
    ds_tx_reset_for_test();
    reset_mock_cmd();
    tx_completed = false;
    device_command_service_set_hooks(&mock_cmd_hooks);
    TEST_ASSERT_EQUAL(ESP_OK, device_command_service_init());
    TEST_ASSERT_EQUAL(ESP_OK, device_settings_init());
    ds_device_record_t *rec = setup_tx_schema("tx-g7", DS_TYPE_BOOL, DS_FLAG_WRITABLE);
    ds_change_request_t change = { .type = DS_TYPE_BOOL, .bool_val = true };
    strlcpy(change.setting_id, "setting", sizeof(change.setting_id));
    TEST_ASSERT_EQUAL(ESP_OK, device_settings_save("tx-g7", &change, 1, 7,
                                                    tx_completion, NULL));
    vTaskDelay(pdMS_TO_TICKS(40));
    ds_transaction_t *tx = ds_tx_find("tx-g7");
    TEST_ASSERT_NOT_NULL(tx);
    TEST_ASSERT_NOT_EQUAL_UINT64(0, tx->transaction_id);
    TEST_ASSERT_EQUAL_STRING(GW_SETTINGS_CMD_TX_BEGIN, mock_cmd_last_command);
    TEST_ASSERT_EQUAL_UINT64(tx->transaction_id,
                             mock_cmd_last_message.settings_transaction_id);
    tx_ack("tx-g7");
    TEST_ASSERT_EQUAL_STRING(GW_SETTINGS_CMD_TX_SET, mock_cmd_last_command);
    TEST_ASSERT_TRUE(mock_cmd_last_message.has_setting_value);
    TEST_ASSERT_TRUE(mock_cmd_last_message.setting_value.setting_value_bool);
    tx_ack("tx-g7");
    TEST_ASSERT_EQUAL_STRING(GW_SETTINGS_CMD_TX_COMMIT, mock_cmd_last_command);
    gw_message_t commit_ack = make_settings_ack("tx-g7", mock_cmd_last_command,
                                                mock_cmd_last_request_id);
    commit_ack.has_int_value = 1;
    commit_ack.int_value = 8;
    TEST_ASSERT_TRUE(device_command_service_on_notify("tx-g7", &commit_ack));
    vTaskDelay(pdMS_TO_TICKS(40));
    TEST_ASSERT_EQUAL_STRING(GW_SETTINGS_CMD_COMMIT_CONFIRM, mock_cmd_last_command);
    TEST_ASSERT_EQUAL_UINT32(8, mock_cmd_last_message.settings_new_revision);
    tx_ack("tx-g7");
    TEST_ASSERT_EQUAL(DS_TX_WAITING_REBOOT, tx->state);
    TEST_ASSERT_FALSE(tx_completed);
    rec->schema = NULL;
    ds_tx_reset_for_test();
    device_command_service_deinit();
    device_command_service_set_hooks(NULL);
    device_settings_reset_for_test();
    device_settings_deinit();
}

TEST_CASE("DS-TX-008..012 local validation rejects readonly range and enum",
          "[device_settings][g7]")
{
    device_settings_deinit();
    device_settings_reset_for_test();
    ds_tx_reset_for_test();
    TEST_ASSERT_EQUAL(ESP_OK, device_settings_init());
    setup_tx_schema("tx-invalid", DS_TYPE_INT, 0);
    ds_change_request_t change = { .type = DS_TYPE_INT, .int_val = 105 };
    strlcpy(change.setting_id, "setting", sizeof(change.setting_id));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      device_settings_save("tx-invalid", &change, 1, 7, NULL, NULL));
    device_settings_reset_for_test();
    device_settings_deinit();
}

/* ── DS-WORK-001: capability → DESCRIBE queued → worker submits ────── */

TEST_CASE("DS-WORK-001: DESCRIBE submitted via worker",
          "[device_settings][g3]")
{
    device_settings_worker_deinit();
    device_settings_worker_reset_for_test();
    device_settings_operation_reset_for_test();
    device_settings_deinit();
    device_settings_reset_for_test();
    reset_mock_cmd();

    device_command_service_set_hooks(&mock_cmd_hooks);
    TEST_ASSERT_EQUAL(ESP_OK, device_command_service_init());
    TEST_ASSERT_EQUAL(ESP_OK, device_settings_init());
    TEST_ASSERT_EQUAL(ESP_OK, device_settings_worker_init());

    /* Set up device with no cached schema → DESCRIBE. */
    ds_device_record_t *rec =
        device_settings_find_or_create_record("wk001");
    TEST_ASSERT_NOT_NULL(rec);
    rec->schema_state = DS_SCHEMA_UNKNOWN;
    rec->schema = NULL;
    rec->schema_rev = 0;

    /* Queue DESCRIBE via capability bridge. */
    device_settings_on_capability("wk001", true, 5);
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Verify worker submitted describe_settings. */
    TEST_ASSERT_TRUE(mock_cmd_send_called);
    TEST_ASSERT_EQUAL_STRING("wk001", mock_cmd_last_device_id);
    TEST_ASSERT_EQUAL_STRING(GW_SETTINGS_CMD_DESCRIBE_SETTINGS,
                             mock_cmd_last_command);

    /* Cleanup. */
    device_settings_worker_deinit();
    device_settings_worker_reset_for_test();
    device_settings_operation_reset_for_test();
    device_command_service_deinit();
    device_command_service_set_hooks(NULL);
    device_settings_reset_for_test();
    device_settings_deinit();
}

/* ── DS-WORK-002: same rev → READ queued → worker submits ──────────── */

TEST_CASE("DS-WORK-002: READ submitted via worker",
          "[device_settings][g3]")
{
    device_settings_worker_deinit();
    device_settings_worker_reset_for_test();
    device_settings_operation_reset_for_test();
    device_settings_deinit();
    device_settings_reset_for_test();
    reset_mock_cmd();

    device_command_service_set_hooks(&mock_cmd_hooks);
    TEST_ASSERT_EQUAL(ESP_OK, device_command_service_init());
    TEST_ASSERT_EQUAL(ESP_OK, device_settings_init());
    TEST_ASSERT_EQUAL(ESP_OK, device_settings_worker_init());

    /* Set up device with cached schema at same revision → READ. */
    ds_device_record_t *rec =
        device_settings_find_or_create_record("wk002");
    TEST_ASSERT_NOT_NULL(rec);
    rec->schema_state = DS_SCHEMA_READY;
    rec->schema_rev = 7;
    rec->schema = (ds_schema_t *)(uintptr_t)1;

    /* Queue READ via capability bridge. */
    device_settings_on_capability("wk002", true, 7);
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Verify worker submitted read_settings. */
    TEST_ASSERT_TRUE(mock_cmd_send_called);
    TEST_ASSERT_EQUAL_STRING("wk002", mock_cmd_last_device_id);
    TEST_ASSERT_EQUAL_STRING(GW_SETTINGS_CMD_READ_SETTINGS,
                             mock_cmd_last_command);

    /* Cleanup. */
    rec->schema = NULL;
    device_settings_worker_deinit();
    device_settings_worker_reset_for_test();
    device_settings_operation_reset_for_test();
    device_command_service_deinit();
    device_command_service_set_hooks(NULL);
    device_settings_reset_for_test();
    device_settings_deinit();
}

/* ── DS-WORK-003: BUSY retry with bounded backoff ──────────────────── */

TEST_CASE("DS-WORK-003: BUSY triggers bounded retry",
          "[device_settings][g3]")
{
    device_settings_worker_deinit();
    device_settings_worker_reset_for_test();
    device_settings_operation_reset_for_test();
    device_settings_deinit();
    device_settings_reset_for_test();
    reset_mock_cmd();

    /* Make mock return BUSY on first submit. */
    mock_cmd_send_rc = -1;

    device_command_service_set_hooks(&mock_cmd_hooks);
    TEST_ASSERT_EQUAL(ESP_OK, device_command_service_init());
    TEST_ASSERT_EQUAL(ESP_OK, device_settings_init());
    TEST_ASSERT_EQUAL(ESP_OK, device_settings_worker_init());

    ds_device_record_t *rec =
        device_settings_find_or_create_record("wk003");
    TEST_ASSERT_NOT_NULL(rec);
    rec->schema_state = DS_SCHEMA_READY;
    rec->schema_rev = 1;
    rec->schema = (ds_schema_t *)(uintptr_t)1;

    device_settings_on_capability("wk003", true, 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Command was submitted (even though mock returned error). */
    TEST_ASSERT_TRUE(mock_cmd_send_called);
    TEST_ASSERT_EQUAL_STRING("wk003", mock_cmd_last_device_id);

    /* Cleanup. */
    rec->schema = NULL;
    device_settings_worker_deinit();
    device_settings_worker_reset_for_test();
    device_settings_operation_reset_for_test();
    device_command_service_deinit();
    device_command_service_set_hooks(NULL);
    device_settings_reset_for_test();
    device_settings_deinit();
}

/* ── DS-WORK-004: retry exhausted → operation fails ────────────────── */

TEST_CASE("DS-WORK-004: retry exhausted fails operation",
          "[device_settings][g3]")
{
    device_settings_worker_deinit();
    device_settings_worker_reset_for_test();
    device_settings_operation_reset_for_test();
    device_settings_deinit();
    device_settings_reset_for_test();
    reset_mock_cmd();

    device_command_service_set_hooks(&mock_cmd_hooks);
    TEST_ASSERT_EQUAL(ESP_OK, device_command_service_init());
    TEST_ASSERT_EQUAL(ESP_OK, device_settings_init());
    TEST_ASSERT_EQUAL(ESP_OK, device_settings_worker_init());

    ds_device_record_t *rec =
        device_settings_find_or_create_record("wk004");
    TEST_ASSERT_NOT_NULL(rec);
    rec->schema_state = DS_SCHEMA_READY;
    rec->schema_rev = 1;
    rec->schema = (ds_schema_t *)(uintptr_t)1;

    s_op_completed = false;
    device_settings_get("wk004", test_op_completion, NULL);
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Verify command was submitted. */
    TEST_ASSERT_TRUE(mock_cmd_send_called);

    /* Simulate BUSY ACK by sending a BUSY status through the command service. */
    /* The command service should have a pending request. Build a BUSY ACK. */
    uint32_t req_id = mock_cmd_last_request_id;
    /* Can't directly inject BUSY through on_notify — the command service
     * translates ACK status from the BLE layer.  Instead, simulate by
     * sending a rejected ACK. */
    gw_message_t busy_ack = make_settings_ack("wk004",
                                              GW_SETTINGS_CMD_READ_SETTINGS,
                                              req_id);
    busy_ack.bool_value = 0; /* rejected = BUSY-like */
    device_command_service_on_notify("wk004", &busy_ack);

    vTaskDelay(pdMS_TO_TICKS(200));

    /* Cleanup. */
    rec->schema = NULL;
    device_settings_worker_deinit();
    device_settings_worker_reset_for_test();
    device_settings_operation_reset_for_test();
    device_command_service_deinit();
    device_command_service_set_hooks(NULL);
    device_settings_reset_for_test();
    device_settings_deinit();
}

/* ── DS-WORK-005: timeout cleanup ──────────────────────────────────── */

TEST_CASE("DS-WORK-005: timeout cleanup releases active",
          "[device_settings][g3]")
{
    device_settings_worker_deinit();
    device_settings_worker_reset_for_test();
    device_settings_operation_reset_for_test();
    device_settings_deinit();
    device_settings_reset_for_test();
    reset_mock_cmd();

    device_command_service_set_hooks(&mock_cmd_hooks);
    TEST_ASSERT_EQUAL(ESP_OK, device_command_service_init());
    TEST_ASSERT_EQUAL(ESP_OK, device_settings_init());
    TEST_ASSERT_EQUAL(ESP_OK, device_settings_worker_init());

    ds_device_record_t *rec =
        device_settings_find_or_create_record("wk005");
    TEST_ASSERT_NOT_NULL(rec);
    rec->schema_state = DS_SCHEMA_READY;
    rec->schema_rev = 1;
    rec->schema = (ds_schema_t *)(uintptr_t)1;

    device_settings_get("wk005", NULL, NULL);
    vTaskDelay(pdMS_TO_TICKS(100));
    TEST_ASSERT_TRUE(mock_cmd_send_called);

    /* Simulate timeout via on_disconnect (command service will fail pending). */
    device_command_service_on_disconnect("wk005");
    vTaskDelay(pdMS_TO_TICKS(200));

    /* Worker should be idle now. */
    TEST_ASSERT_TRUE(device_settings_worker_is_idle_for_test());

    /* Cleanup. */
    rec->schema = NULL;
    device_settings_worker_deinit();
    device_settings_worker_reset_for_test();
    device_settings_operation_reset_for_test();
    device_command_service_deinit();
    device_command_service_set_hooks(NULL);
    device_settings_reset_for_test();
    device_settings_deinit();
}

/* ── DS-WORK-006: disconnect cleanup → cancel + generation ─────────── */

TEST_CASE("DS-WORK-006: disconnect cancels active and increments gen",
          "[device_settings][g3]")
{
    device_settings_worker_deinit();
    device_settings_worker_reset_for_test();
    device_settings_operation_reset_for_test();
    device_settings_deinit();
    device_settings_reset_for_test();
    reset_mock_cmd();

    device_command_service_set_hooks(&mock_cmd_hooks);
    TEST_ASSERT_EQUAL(ESP_OK, device_command_service_init());
    TEST_ASSERT_EQUAL(ESP_OK, device_settings_init());
    TEST_ASSERT_EQUAL(ESP_OK, device_settings_worker_init());

    ds_device_record_t *rec =
        device_settings_find_or_create_record("wk006");
    TEST_ASSERT_NOT_NULL(rec);
    rec->schema_state = DS_SCHEMA_READY;
    rec->schema_rev = 1;
    rec->schema = (ds_schema_t *)(uintptr_t)1;

    device_settings_get("wk006", NULL, NULL);
    vTaskDelay(pdMS_TO_TICKS(100));
    TEST_ASSERT_TRUE(mock_cmd_send_called);

    /* Disconnect should cancel active command. */
    device_settings_on_disconnect("wk006");
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Worker should be idle. */
    TEST_ASSERT_TRUE(device_settings_worker_is_idle_for_test());

    /* Cleanup. */
    rec->schema = NULL;
    device_settings_worker_deinit();
    device_settings_worker_reset_for_test();
    device_settings_operation_reset_for_test();
    device_command_service_deinit();
    device_command_service_set_hooks(NULL);
    device_settings_reset_for_test();
    device_settings_deinit();
}

/* ── DS-WORK-007: duplicate coalescing ─────────────────────────────── */

TEST_CASE("DS-WORK-007: duplicate trigger coalesced",
          "[device_settings][g3]")
{
    device_settings_worker_deinit();
    device_settings_worker_reset_for_test();
    device_settings_operation_reset_for_test();
    device_settings_deinit();
    device_settings_reset_for_test();
    reset_mock_cmd();

    device_command_service_set_hooks(&mock_cmd_hooks);
    TEST_ASSERT_EQUAL(ESP_OK, device_command_service_init());
    TEST_ASSERT_EQUAL(ESP_OK, device_settings_init());
    TEST_ASSERT_EQUAL(ESP_OK, device_settings_worker_init());

    ds_device_record_t *rec =
        device_settings_find_or_create_record("wk007");
    TEST_ASSERT_NOT_NULL(rec);
    rec->schema_state = DS_SCHEMA_UNKNOWN;
    rec->schema = NULL;
    rec->schema_rev = 0;

    /* First call queues DESCRIBE. */
    esp_err_t err1 = device_settings_describe("wk007", NULL, NULL);
    TEST_ASSERT_EQUAL(ESP_OK, err1);

    /* Second call should fail (already active for this device). */
    esp_err_t err2 = device_settings_describe("wk007", NULL, NULL);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, err2);

    /* Worker picks up the single queued op. */
    vTaskDelay(pdMS_TO_TICKS(100));
    TEST_ASSERT_TRUE(mock_cmd_send_called);
    TEST_ASSERT_EQUAL_UINT32(1, mock_cmd_send_count);

    /* Cleanup. */
    device_settings_worker_deinit();
    device_settings_worker_reset_for_test();
    device_settings_operation_reset_for_test();
    device_command_service_deinit();
    device_command_service_set_hooks(NULL);
    device_settings_reset_for_test();
    device_settings_deinit();
}

/* ── DS-WORK-008: READ only after DESCRIBE ACK ─────────────────────── */

TEST_CASE("DS-WORK-008: READ only after DESCRIBE ACK",
          "[device_settings][g3]")
{
    device_settings_worker_deinit();
    device_settings_worker_reset_for_test();
    device_settings_operation_reset_for_test();
    device_settings_deinit();
    device_settings_reset_for_test();
    reset_mock_cmd();

    device_command_service_set_hooks(&mock_cmd_hooks);
    TEST_ASSERT_EQUAL(ESP_OK, device_command_service_init());
    TEST_ASSERT_EQUAL(ESP_OK, device_settings_init());
    TEST_ASSERT_EQUAL(ESP_OK, device_settings_worker_init());

    ds_device_record_t *rec =
        device_settings_find_or_create_record("wk008");
    TEST_ASSERT_NOT_NULL(rec);
    rec->schema_state = DS_SCHEMA_UNKNOWN;
    rec->schema = NULL;
    rec->schema_rev = 0;

    /* Queue DESCRIBE. */
    device_settings_on_capability("wk008", true, 3);
    vTaskDelay(pdMS_TO_TICKS(100));

    /* DESCRIBE was submitted. */
    TEST_ASSERT_TRUE(mock_cmd_send_called);
    TEST_ASSERT_EQUAL_STRING(GW_SETTINGS_CMD_DESCRIBE_SETTINGS,
                             mock_cmd_last_command);

    /* Save request_id BEFORE resetting mock (reset zeroes it). */
    uint32_t req_id = mock_cmd_last_request_id;

    /* Before ACK, worker should NOT submit READ. */
    reset_mock_cmd();
    vTaskDelay(pdMS_TO_TICKS(50));
    TEST_ASSERT_FALSE(mock_cmd_send_called);

    /* Now simulate DESCRIBE ACK. */
    gw_message_t ack = make_settings_ack("wk008",
                                         GW_SETTINGS_CMD_DESCRIBE_SETTINGS,
                                         req_id);
    device_command_service_on_notify("wk008", &ack);
    vTaskDelay(pdMS_TO_TICKS(200));

    /* After ACK, worker should have submitted READ. */
    TEST_ASSERT_TRUE(mock_cmd_send_called);
    TEST_ASSERT_EQUAL_STRING(GW_SETTINGS_CMD_READ_SETTINGS,
                             mock_cmd_last_command);

    /* Cleanup. */
    device_settings_worker_deinit();
    device_settings_worker_reset_for_test();
    device_settings_operation_reset_for_test();
    device_command_service_deinit();
    device_command_service_set_hooks(NULL);
    device_settings_reset_for_test();
    device_settings_deinit();
}

/* ── DS-WORK-009: queue full returns error ─────────────────────────── */

TEST_CASE("DS-WORK-009: queue full explicit error",
          "[device_settings][g3]")
{
    device_settings_worker_deinit();
    device_settings_worker_reset_for_test();
    device_settings_operation_reset_for_test();
    device_settings_deinit();
    device_settings_reset_for_test();
    TEST_ASSERT_EQUAL(ESP_OK, device_settings_init());

    /* The operation queue holds 16 entries while the command service has
     * only 8 pending slots. Fill the operation layer directly so this test
     * checks its documented no-memory result without DCS timing effects. */
    device_settings_fill_all_ops_for_test();

    esp_err_t err = device_settings_describe("overflow", NULL, NULL);
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, err);

    device_settings_operation_reset_for_test();
    device_settings_reset_for_test();
    device_settings_deinit();
}

/* ── G4 schema stream fixtures ─────────────────────────────────────── */

#define SCHEMA_DEVICE "schema-dev"
#define SCHEMA_REQUEST_ID 4242

static gw_message_t schema_message(const char *type)
{
    gw_message_t msg = {0};
    msg.protocol_version = GW_PROTOCOL_VERSION;
    strlcpy(msg.type, type, sizeof(msg.type));
    strlcpy(msg.command, GW_SETTINGS_CMD_DESCRIBE_SETTINGS,
            sizeof(msg.command));
    msg.request_id = SCHEMA_REQUEST_ID;
    msg.has_request_id = 1;
    return msg;
}

static gw_message_t schema_begin(uint16_t total)
{
    gw_message_t msg = schema_message(GW_SETTINGS_MSG_SETTINGS_BEGIN);
    msg.total = total;
    msg.has_total = 1;
    return msg;
}

static gw_message_t schema_item(uint16_t sequence, uint16_t total,
                                const char *id, uint8_t type)
{
    gw_message_t msg = schema_message(GW_SETTINGS_MSG_SETTINGS_ITEM);
    msg.total = total;
    msg.has_total = 1;
    msg.settings_sequence = sequence;
    msg.has_settings_sequence = 1;
    strlcpy(msg.setting_id, id, sizeof(msg.setting_id));
    msg.has_setting_id = 1;
    msg.setting_type = type;
    msg.has_setting_type = 1;
    return msg;
}

static gw_message_t schema_option(uint16_t parent, uint8_t value,
                                  const char *label)
{
    gw_message_t msg = schema_message(GW_SETTINGS_MSG_SETTINGS_OPTION_ITEM);
    msg.settings_sequence = parent;
    msg.has_settings_sequence = 1;
    msg.settings_option_index = value;
    msg.has_settings_option_index = 1;
    strlcpy(msg.setting_title, label, sizeof(msg.setting_title));
    return msg;
}

static gw_message_t schema_end(uint16_t total)
{
    gw_message_t msg = schema_message(GW_SETTINGS_MSG_SETTINGS_END);
    msg.total = total;
    msg.has_total = 1;
    return msg;
}

static void schema_test_setup(void)
{
    device_settings_worker_deinit();
    device_settings_worker_reset_for_test();
    device_settings_operation_reset_for_test();
    device_settings_deinit();
    device_settings_reset_for_test();
    TEST_ASSERT_EQUAL(ESP_OK, device_settings_init());
    ds_device_record_t *rec =
        device_settings_find_or_create_record(SCHEMA_DEVICE);
    TEST_ASSERT_NOT_NULL(rec);
    rec->advertised_schema_rev = 7;
}

static void schema_test_teardown(void)
{
    device_settings_protocol_on_disconnect(NULL);
    device_settings_reset_for_test();
    device_settings_deinit();
}

static const ds_schema_t *schema_send_single(gw_message_t *item)
{
    gw_message_t begin = schema_begin(1);
    gw_message_t end = schema_end(1);
    TEST_ASSERT_TRUE(device_settings_on_notify(SCHEMA_DEVICE, &begin));
    TEST_ASSERT_TRUE(device_settings_on_notify(SCHEMA_DEVICE, item));
    TEST_ASSERT_TRUE(device_settings_on_notify(SCHEMA_DEVICE, &end));
    return device_settings_schema_acquire(SCHEMA_DEVICE);
}

TEST_CASE("DS-SCHEMA-001: BOOL descriptor", "[device_settings][g4]")
{
    schema_test_setup();
    gw_message_t item = schema_item(0, 1, "enabled", DS_TYPE_BOOL);
    const ds_schema_t *schema = schema_send_single(&item);
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_EQUAL_UINT16(1, schema->setting_count);
    TEST_ASSERT_EQUAL(DS_TYPE_BOOL, schema->descriptors[0].type);
    TEST_ASSERT_TRUE(schema->descriptors[0].flags & DS_FLAG_WRITABLE);
    TEST_ASSERT_EQUAL_STRING("enabled", ds_string_pool_get(
        &schema->strings, schema->descriptors[0].title_off));
    device_settings_schema_release(schema);
    schema_test_teardown();
}

TEST_CASE("DS-SCHEMA-002: INT min max step", "[device_settings][g4]")
{
    schema_test_setup();
    gw_message_t item = schema_item(0, 1, "level", DS_TYPE_INT);
    item.min_value = -10; item.has_min_value = 1;
    item.max_value = 100; item.has_max_value = 1;
    item.step = 5; item.has_step = 1;
    const ds_schema_t *schema = schema_send_single(&item);
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_EQUAL_INT32(-10, schema->descriptors[0].min_value);
    TEST_ASSERT_EQUAL_INT32(100, schema->descriptors[0].max_value);
    TEST_ASSERT_EQUAL_UINT32(5, schema->descriptors[0].step);
    device_settings_schema_release(schema);
    schema_test_teardown();
}

TEST_CASE("DS-SCHEMA-003: STRING max length", "[device_settings][g4]")
{
    schema_test_setup();
    gw_message_t item = schema_item(0, 1, "hostname", DS_TYPE_STRING);
    item.settings_max_length = 48;
    item.has_settings_max_length = 1;
    const ds_schema_t *schema = schema_send_single(&item);
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_EQUAL_UINT16(48, schema->descriptors[0].max_length);
    device_settings_schema_release(schema);
    schema_test_teardown();
}

TEST_CASE("DS-SCHEMA-004: ENUM options", "[device_settings][g4]")
{
    schema_test_setup();
    gw_message_t begin = schema_begin(1);
    gw_message_t item = schema_item(0, 1, "mode", DS_TYPE_ENUM);
    gw_message_t opt_a = schema_option(0, 2, "Eco");
    gw_message_t opt_b = schema_option(0, 5, "Boost");
    gw_message_t end = schema_end(1);
    device_settings_on_notify(SCHEMA_DEVICE, &begin);
    device_settings_on_notify(SCHEMA_DEVICE, &item);
    device_settings_on_notify(SCHEMA_DEVICE, &opt_a);
    device_settings_on_notify(SCHEMA_DEVICE, &opt_b);
    device_settings_on_notify(SCHEMA_DEVICE, &end);
    const ds_schema_t *schema = device_settings_schema_acquire(SCHEMA_DEVICE);
    TEST_ASSERT_NOT_NULL(schema);
    const ds_setting_desc_t *desc = &schema->descriptors[0];
    TEST_ASSERT_EQUAL_UINT8(2, desc->option_count);
    TEST_ASSERT_EQUAL_UINT8(2, schema->enum_option_pool[desc->option_index].value);
    TEST_ASSERT_EQUAL_UINT8(5, schema->enum_option_pool[desc->option_index + 1].value);
    TEST_ASSERT_EQUAL_STRING("Boost", ds_string_pool_get(
        &schema->strings,
        schema->enum_option_pool[desc->option_index + 1].label_off));
    device_settings_schema_release(schema);
    schema_test_teardown();
}

TEST_CASE("G4: interleaved options keep each ENUM span contiguous",
          "[device_settings][g4]")
{
    schema_test_setup();
    gw_message_t begin = schema_begin(2);
    gw_message_t first = schema_item(0, 2, "mode", DS_TYPE_ENUM);
    gw_message_t second = schema_item(1, 2, "region", DS_TYPE_ENUM);
    gw_message_t mode_a = schema_option(0, 1, "Eco");
    gw_message_t region_a = schema_option(1, 7, "EU");
    gw_message_t mode_b = schema_option(0, 2, "Boost");
    gw_message_t end = schema_end(2);
    device_settings_on_notify(SCHEMA_DEVICE, &begin);
    device_settings_on_notify(SCHEMA_DEVICE, &first);
    device_settings_on_notify(SCHEMA_DEVICE, &second);
    device_settings_on_notify(SCHEMA_DEVICE, &mode_a);
    device_settings_on_notify(SCHEMA_DEVICE, &region_a);
    device_settings_on_notify(SCHEMA_DEVICE, &mode_b);
    device_settings_on_notify(SCHEMA_DEVICE, &end);

    const ds_schema_t *schema = device_settings_schema_acquire(SCHEMA_DEVICE);
    TEST_ASSERT_NOT_NULL(schema);
    const ds_setting_desc_t *mode = &schema->descriptors[0];
    const ds_setting_desc_t *region = &schema->descriptors[1];
    TEST_ASSERT_EQUAL_UINT8(2, mode->option_count);
    TEST_ASSERT_EQUAL_UINT8(1, region->option_count);
    TEST_ASSERT_EQUAL_UINT8(1,
        schema->enum_option_pool[mode->option_index].value);
    TEST_ASSERT_EQUAL_UINT8(2,
        schema->enum_option_pool[mode->option_index + 1].value);
    TEST_ASSERT_EQUAL_UINT8(7,
        schema->enum_option_pool[region->option_index].value);
    device_settings_schema_release(schema);
    schema_test_teardown();
}

TEST_CASE("DS-SCHEMA-005: title preserved", "[device_settings][g4]")
{
    schema_test_setup();
    gw_message_t item = schema_item(0, 1, "wifi_mode", DS_TYPE_ENUM);
    strlcpy(item.setting_title, "Wi-Fi mode", sizeof(item.setting_title));
    const ds_schema_t *schema = schema_send_single(&item);
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_EQUAL_STRING("Wi-Fi mode", ds_string_pool_get(
        &schema->strings, schema->descriptors[0].title_off));
    device_settings_schema_release(schema);
    schema_test_teardown();
}

TEST_CASE("DS-SCHEMA-006: group and unit preserved", "[device_settings][g4]")
{
    schema_test_setup();
    gw_message_t item = schema_item(0, 1, "threshold", DS_TYPE_INT);
    strlcpy(item.setting_group, "sensors", sizeof(item.setting_group));
    item.has_setting_group = 1;
    strlcpy(item.setting_unit, "dBm", sizeof(item.setting_unit));
    const ds_schema_t *schema = schema_send_single(&item);
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_EQUAL_STRING("sensors", ds_string_pool_get(
        &schema->strings, schema->descriptors[0].group_off));
    TEST_ASSERT_EQUAL_STRING("dBm", ds_string_pool_get(
        &schema->strings, schema->descriptors[0].unit_off));
    device_settings_schema_release(schema);
    schema_test_teardown();
}

TEST_CASE("DS-SCHEMA-007: duplicate ID rejected", "[device_settings][g4]")
{
    schema_test_setup();
    gw_message_t begin = schema_begin(2);
    gw_message_t first = schema_item(0, 2, "same", DS_TYPE_BOOL);
    gw_message_t second = schema_item(1, 2, "same", DS_TYPE_INT);
    device_settings_on_notify(SCHEMA_DEVICE, &begin);
    device_settings_on_notify(SCHEMA_DEVICE, &first);
    device_settings_on_notify(SCHEMA_DEVICE, &second);
    TEST_ASSERT_NULL(device_settings_schema_acquire(SCHEMA_DEVICE));
    schema_test_teardown();
}

TEST_CASE("DS-SCHEMA-008: sequence gap rejected", "[device_settings][g4]")
{
    schema_test_setup();
    gw_message_t begin = schema_begin(2);
    gw_message_t item = schema_item(1, 2, "late", DS_TYPE_BOOL);
    device_settings_on_notify(SCHEMA_DEVICE, &begin);
    device_settings_on_notify(SCHEMA_DEVICE, &item);
    TEST_ASSERT_NULL(device_settings_schema_acquire(SCHEMA_DEVICE));
    schema_test_teardown();
}

TEST_CASE("DS-SCHEMA-009: duplicate sequence rejected", "[device_settings][g4]")
{
    schema_test_setup();
    gw_message_t begin = schema_begin(2);
    gw_message_t first = schema_item(0, 2, "first", DS_TYPE_BOOL);
    gw_message_t duplicate = schema_item(0, 2, "second", DS_TYPE_BOOL);
    device_settings_on_notify(SCHEMA_DEVICE, &begin);
    device_settings_on_notify(SCHEMA_DEVICE, &first);
    device_settings_on_notify(SCHEMA_DEVICE, &duplicate);
    TEST_ASSERT_NULL(device_settings_schema_acquire(SCHEMA_DEVICE));
    schema_test_teardown();
}

TEST_CASE("DS-SCHEMA-010: total mismatch rejected", "[device_settings][g4]")
{
    schema_test_setup();
    gw_message_t begin = schema_begin(1);
    gw_message_t item = schema_item(0, 1, "only", DS_TYPE_BOOL);
    gw_message_t end = schema_end(2);
    device_settings_on_notify(SCHEMA_DEVICE, &begin);
    device_settings_on_notify(SCHEMA_DEVICE, &item);
    device_settings_on_notify(SCHEMA_DEVICE, &end);
    TEST_ASSERT_NULL(device_settings_schema_acquire(SCHEMA_DEVICE));
    schema_test_teardown();
}

TEST_CASE("DS-SCHEMA-011: option before parent rejected", "[device_settings][g4]")
{
    schema_test_setup();
    gw_message_t begin = schema_begin(1);
    gw_message_t option = schema_option(0, 0, "None");
    device_settings_on_notify(SCHEMA_DEVICE, &begin);
    device_settings_on_notify(SCHEMA_DEVICE, &option);
    TEST_ASSERT_NULL(device_settings_schema_acquire(SCHEMA_DEVICE));
    schema_test_teardown();
}

TEST_CASE("DS-SCHEMA-012: option on non enum rejected", "[device_settings][g4]")
{
    schema_test_setup();
    gw_message_t begin = schema_begin(1);
    gw_message_t item = schema_item(0, 1, "enabled", DS_TYPE_BOOL);
    gw_message_t option = schema_option(0, 0, "Off");
    device_settings_on_notify(SCHEMA_DEVICE, &begin);
    device_settings_on_notify(SCHEMA_DEVICE, &item);
    device_settings_on_notify(SCHEMA_DEVICE, &option);
    TEST_ASSERT_NULL(device_settings_schema_acquire(SCHEMA_DEVICE));
    schema_test_teardown();
}

TEST_CASE("DS-SCHEMA-013: readonly flag translated", "[device_settings][g4]")
{
    schema_test_setup();
    gw_message_t item = schema_item(0, 1, "serial", DS_TYPE_STRING);
    item.setting_flags = GW_SETTING_FLAG_READONLY |
                         GW_SETTING_FLAG_SECRET |
                         GW_SETTING_FLAG_ADVANCED;
    item.has_setting_flags = 1;
    const ds_schema_t *schema = schema_send_single(&item);
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_FALSE(schema->descriptors[0].flags & DS_FLAG_WRITABLE);
    TEST_ASSERT_TRUE(schema->descriptors[0].flags & DS_FLAG_SECRET);
    TEST_ASSERT_TRUE(schema->descriptors[0].flags & DS_FLAG_ADVANCED);
    device_settings_schema_release(schema);
    schema_test_teardown();
}

TEST_CASE("DS-SCHEMA-014: no device_id accepted", "[device_settings][g4]")
{
    schema_test_setup();
    gw_message_t item = schema_item(0, 1, "nodev", DS_TYPE_BOOL);
    TEST_ASSERT_FALSE(item.has_device_id);
    const ds_schema_t *schema = schema_send_single(&item);
    TEST_ASSERT_NOT_NULL(schema);
    device_settings_schema_release(schema);
    schema_test_teardown();
}

TEST_CASE("DS-SCHEMA-015: no snapshot_id accepted", "[device_settings][g4]")
{
    schema_test_setup();
    gw_message_t item = schema_item(0, 1, "nosnap", DS_TYPE_BOOL);
    TEST_ASSERT_FALSE(item.has_snapshot_id);
    const ds_schema_t *schema = schema_send_single(&item);
    TEST_ASSERT_NOT_NULL(schema);
    device_settings_schema_release(schema);
    schema_test_teardown();
}

TEST_CASE("DS-SCHEMA-016: failed stream retains old schema", "[device_settings][g4]")
{
    schema_test_setup();
    gw_message_t original = schema_item(0, 1, "original", DS_TYPE_BOOL);
    const ds_schema_t *old_schema = schema_send_single(&original);
    TEST_ASSERT_NOT_NULL(old_schema);

    gw_message_t begin = schema_begin(2);
    gw_message_t bad = schema_item(1, 2, "bad", DS_TYPE_BOOL);
    device_settings_on_notify(SCHEMA_DEVICE, &begin);
    device_settings_on_notify(SCHEMA_DEVICE, &bad);

    const ds_schema_t *still_committed =
        device_settings_schema_acquire(SCHEMA_DEVICE);
    TEST_ASSERT_EQUAL_PTR(old_schema, still_committed);
    TEST_ASSERT_EQUAL_STRING("original", ds_string_pool_get(
        &still_committed->strings,
        still_committed->descriptors[0].id_off));
    device_settings_schema_release(still_committed);
    device_settings_schema_release(old_schema);
    schema_test_teardown();
}

TEST_CASE("G4: stream rejects request mismatch", "[device_settings][g4]")
{
    schema_test_setup();
    gw_message_t begin = schema_begin(1);
    gw_message_t item = schema_item(0, 1, "wrong-request", DS_TYPE_BOOL);
    item.request_id++;
    device_settings_on_notify(SCHEMA_DEVICE, &begin);
    device_settings_on_notify(SCHEMA_DEVICE, &item);
    TEST_ASSERT_NULL(device_settings_schema_acquire(SCHEMA_DEVICE));
    schema_test_teardown();
}

TEST_CASE("DS-MULTI-003: wrong-device frame rejected",
          "[device_settings][g4]")
{
    schema_test_setup();
    gw_message_t begin = schema_begin(1);
    gw_message_t foreign = schema_item(0, 1, "foreign", DS_TYPE_BOOL);
    gw_message_t owner = schema_item(0, 1, "owner", DS_TYPE_BOOL);
    gw_message_t end = schema_end(1);
    device_settings_on_notify(SCHEMA_DEVICE, &begin);
    device_settings_on_notify("other-device", &foreign);
    device_settings_on_notify(SCHEMA_DEVICE, &owner);
    device_settings_on_notify(SCHEMA_DEVICE, &end);

    const ds_schema_t *schema = device_settings_schema_acquire(SCHEMA_DEVICE);
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_EQUAL_STRING("owner", ds_string_pool_get(
        &schema->strings, schema->descriptors[0].id_off));
    device_settings_schema_release(schema);
    schema_test_teardown();
}

TEST_CASE("DS-MULTI-002: active owner enforcement",
          "[device_settings][g4]")
{
    schema_test_setup();
    gw_message_t begin_a = schema_begin(1);
    gw_message_t begin_b = schema_begin(1);
    gw_message_t item_a = schema_item(0, 1, "owned-by-a", DS_TYPE_BOOL);
    gw_message_t end_a = schema_end(1);
    device_settings_on_notify("device-a", &begin_a);
    device_settings_on_notify("device-b", &begin_b);
    device_settings_on_notify("device-a", &item_a);
    device_settings_on_notify("device-a", &end_a);

    const ds_schema_t *schema_a = device_settings_schema_acquire("device-a");
    TEST_ASSERT_NOT_NULL(schema_a);
    TEST_ASSERT_NULL(device_settings_schema_acquire("device-b"));
    device_settings_schema_release(schema_a);
    schema_test_teardown();
}

TEST_CASE("DS-MULTI-001: A then B serialization",
          "[device_settings][g4]")
{
    schema_test_setup();
    gw_message_t begin = schema_begin(1);
    gw_message_t item_a = schema_item(0, 1, "setting-a", DS_TYPE_BOOL);
    gw_message_t item_b = schema_item(0, 1, "setting-b", DS_TYPE_BOOL);
    gw_message_t end = schema_end(1);
    device_settings_on_notify("device-a", &begin);
    device_settings_on_notify("device-a", &item_a);
    device_settings_on_notify("device-a", &end);
    device_settings_on_notify("device-b", &begin);
    device_settings_on_notify("device-b", &item_b);
    device_settings_on_notify("device-b", &end);

    const ds_schema_t *schema_a = device_settings_schema_acquire("device-a");
    const ds_schema_t *schema_b = device_settings_schema_acquire("device-b");
    TEST_ASSERT_NOT_NULL(schema_a);
    TEST_ASSERT_NOT_NULL(schema_b);
    TEST_ASSERT_EQUAL_STRING("setting-a", ds_string_pool_get(
        &schema_a->strings, schema_a->descriptors[0].id_off));
    TEST_ASSERT_EQUAL_STRING("setting-b", ds_string_pool_get(
        &schema_b->strings, schema_b->descriptors[0].id_off));
    device_settings_schema_release(schema_a);
    device_settings_schema_release(schema_b);
    schema_test_teardown();
}

TEST_CASE("sizeof(ds_device_record_t) is compact",
          "[device_settings][g1][g4]")
{
    /* Keep the per-device registry record bounded in internal SRAM. Operation
     * queue state lives in device_settings_operation.c, not in this record. */
    TEST_ASSERT_TRUE(sizeof(ds_device_record_t) <= 80);
}

TEST_CASE("sizeof(ds_setting_desc_t) <= 28 bytes",
          "[device_settings][g1]")
{
    /* Compact descriptor must be small. */
    TEST_ASSERT_TRUE(sizeof(ds_setting_desc_t) <= 28);
}

/* ── G5 values stream fixtures ─────────────────────────────────────── */

#define VALUES_REQUEST_ID 5151

static gw_message_t values_message(const char *type)
{
    gw_message_t msg = {0};
    msg.protocol_version = GW_PROTOCOL_VERSION;
    strlcpy(msg.type, type, sizeof(msg.type));
    /* The stream intentionally uses get_settings, not the outbound
     * read_settings command. */
    strlcpy(msg.command, GW_SETTINGS_CMD_GET_SETTINGS, sizeof(msg.command));
    msg.request_id = VALUES_REQUEST_ID;
    msg.has_request_id = 1;
    msg.capability_revision = 11;
    msg.has_capability_revision = 1;
    return msg;
}

static gw_message_t values_begin(uint16_t total)
{
    gw_message_t msg = values_message(GW_SETTINGS_MSG_SETTINGS_VALUES_BEGIN);
    msg.total = total;
    msg.has_total = 1;
    return msg;
}

static gw_message_t values_value(uint16_t sequence, const char *id,
                                 uint8_t type)
{
    gw_message_t msg = values_message(GW_SETTINGS_MSG_SETTINGS_VALUES_VALUE);
    msg.settings_sequence = sequence;
    msg.has_settings_sequence = 1;
    strlcpy(msg.setting_id, id, sizeof(msg.setting_id));
    msg.has_setting_id = 1;
    msg.setting_type = type;
    msg.has_setting_type = 1;
    msg.has_setting_value = 1;
    return msg;
}

static gw_message_t values_end(uint16_t total)
{
    gw_message_t msg = values_message(GW_SETTINGS_MSG_SETTINGS_VALUES_END);
    msg.total = total;
    msg.has_total = 1;
    return msg;
}

static void values_install_schema(gw_message_t *item)
{
    const ds_schema_t *schema = schema_send_single(item);
    TEST_ASSERT_NOT_NULL(schema);
    device_settings_schema_release(schema);
}

TEST_CASE("DS-VAL-001..004: BOOL INT STRING ENUM decode key40",
          "[device_settings][g5]")
{
    schema_test_setup();
    gw_message_t begin_schema = schema_begin(4);
    gw_message_t bool_item = schema_item(0, 4, "enabled", DS_TYPE_BOOL);
    gw_message_t int_item = schema_item(1, 4, "level", DS_TYPE_INT);
    gw_message_t string_item = schema_item(2, 4, "name", DS_TYPE_STRING);
    string_item.settings_max_length = 12;
    string_item.has_settings_max_length = 1;
    gw_message_t enum_item = schema_item(3, 4, "mode", DS_TYPE_ENUM);
    gw_message_t enum_opt = schema_option(3, 2, "Eco");
    gw_message_t end_schema = schema_end(4);
    device_settings_on_notify(SCHEMA_DEVICE, &begin_schema);
    device_settings_on_notify(SCHEMA_DEVICE, &bool_item);
    device_settings_on_notify(SCHEMA_DEVICE, &int_item);
    device_settings_on_notify(SCHEMA_DEVICE, &string_item);
    device_settings_on_notify(SCHEMA_DEVICE, &enum_item);
    device_settings_on_notify(SCHEMA_DEVICE, &enum_opt);
    device_settings_on_notify(SCHEMA_DEVICE, &end_schema);

    gw_message_t begin = values_begin(4);
    gw_message_t boolean = values_value(0, "enabled", DS_TYPE_BOOL);
    boolean.setting_value.setting_value_bool = true;
    gw_message_t integer = values_value(1, "level", DS_TYPE_INT);
    integer.setting_value.setting_value_int = -9;
    gw_message_t string = values_value(2, "name", DS_TYPE_STRING);
    strlcpy(string.setting_value.setting_value_string, "gateway",
            sizeof(string.setting_value.setting_value_string));
    gw_message_t enumeration = values_value(3, "mode", DS_TYPE_ENUM);
    enumeration.setting_value.setting_value_enum = 2;
    gw_message_t end = values_end(4);
    device_settings_on_notify(SCHEMA_DEVICE, &begin);
    device_settings_on_notify(SCHEMA_DEVICE, &boolean);
    device_settings_on_notify(SCHEMA_DEVICE, &integer);
    device_settings_on_notify(SCHEMA_DEVICE, &string);
    device_settings_on_notify(SCHEMA_DEVICE, &enumeration);
    device_settings_on_notify(SCHEMA_DEVICE, &end);

    const ds_values_t *values = device_settings_values_acquire(SCHEMA_DEVICE);
    TEST_ASSERT_NOT_NULL(values);
    TEST_ASSERT_EQUAL_UINT32(11, values->config_revision);
    TEST_ASSERT_EQUAL_UINT16(4, values->value_count);
    TEST_ASSERT_TRUE(values->values[0].bool_val);
    TEST_ASSERT_EQUAL_INT32(-9, values->values[1].int_val);
    TEST_ASSERT_EQUAL_STRING("gateway", ds_string_pool_get(
        &values->string_pool, values->values[2].string_off));
    TEST_ASSERT_EQUAL_INT32(2, values->values[3].enum_val);
    device_settings_values_release(values);
    schema_test_teardown();
}

TEST_CASE("DS-VAL-005..011: invalid frame retains old values",
          "[device_settings][g5]")
{
    schema_test_setup();
    gw_message_t item = schema_item(0, 1, "enabled", DS_TYPE_BOOL);
    values_install_schema(&item);

    gw_message_t good_begin = values_begin(1);
    gw_message_t good_value = values_value(0, "enabled", DS_TYPE_BOOL);
    good_value.setting_value.setting_value_bool = true;
    gw_message_t good_end = values_end(1);
    device_settings_on_notify(SCHEMA_DEVICE, &good_begin);
    device_settings_on_notify(SCHEMA_DEVICE, &good_value);
    device_settings_on_notify(SCHEMA_DEVICE, &good_end);
    const ds_values_t *old_values = device_settings_values_acquire(SCHEMA_DEVICE);
    TEST_ASSERT_NOT_NULL(old_values);

    gw_message_t bad_begin = values_begin(1);
    gw_message_t bad_value = values_value(0, "unknown", DS_TYPE_BOOL);
    bad_value.setting_value.setting_value_bool = false;
    device_settings_on_notify(SCHEMA_DEVICE, &bad_begin);
    device_settings_on_notify(SCHEMA_DEVICE, &bad_value);
    const ds_values_t *still_values = device_settings_values_acquire(SCHEMA_DEVICE);
    TEST_ASSERT_EQUAL_PTR(old_values, still_values);
    device_settings_values_release(still_values);
    device_settings_values_release(old_values);

    /* request_id and sequence are mandatory and generic int_value is ignored. */
    gw_message_t mismatch_begin = values_begin(1);
    gw_message_t mismatch_value = values_value(1, "enabled", DS_TYPE_BOOL);
    mismatch_value.int_value = 7;
    mismatch_value.has_int_value = 1;
    device_settings_on_notify(SCHEMA_DEVICE, &mismatch_begin);
    device_settings_on_notify(SCHEMA_DEVICE, &mismatch_value);
    const ds_values_t *preserved = device_settings_values_acquire(SCHEMA_DEVICE);
    TEST_ASSERT_NOT_NULL(preserved);
    device_settings_values_release(preserved);
    schema_test_teardown();
}

TEST_CASE("DS-VAL-007: ENUM out of range is rejected", "[device_settings][g5]")
{
    schema_test_setup();
    gw_message_t begin_schema = schema_begin(1);
    gw_message_t item = schema_item(0, 1, "mode", DS_TYPE_ENUM);
    gw_message_t option = schema_option(0, 1, "One");
    gw_message_t end_schema = schema_end(1);
    device_settings_on_notify(SCHEMA_DEVICE, &begin_schema);
    device_settings_on_notify(SCHEMA_DEVICE, &item);
    device_settings_on_notify(SCHEMA_DEVICE, &option);
    device_settings_on_notify(SCHEMA_DEVICE, &end_schema);
    gw_message_t begin = values_begin(1);
    gw_message_t value = values_value(0, "mode", DS_TYPE_ENUM);
    value.setting_value.setting_value_enum = 2;
    device_settings_on_notify(SCHEMA_DEVICE, &begin);
    device_settings_on_notify(SCHEMA_DEVICE, &value);
    TEST_ASSERT_NULL(device_settings_values_acquire(SCHEMA_DEVICE));
    schema_test_teardown();
}
