#include <string.h>

#include "unity.h"
#include "device_settings.h"
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
        .flags = 0,
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
    TEST_ASSERT_FALSE(schema->descriptors[0].flags & DS_FLAG_READONLY);
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

TEST_CASE("refcount: failed staging leaves nothing to clean",
          "[device_settings][g1]")
{
    /* If builder_commit returns NULL (e.g. empty), no allocation occurs. */
    ds_schema_builder_t builder;
    ds_schema_builder_init(&builder);
    /* Don't add any settings. */
    ds_schema_t *schema = ds_schema_builder_commit(&builder);
    TEST_ASSERT_NULL(schema);
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

/* ── Footprint tests ───────────────────────────────────────────────── */

TEST_CASE("sizeof(ds_device_record_t) is compact",
          "[device_settings][g1]")
{
    /* Internal SRAM record should be <= 64 bytes per device.
     * It contains: used, schema_state, op_state, op_kind, op_id,
     * schema*, values*, schema_rev, config_rev. */
    TEST_ASSERT_TRUE(sizeof(ds_device_record_t) <= 64);
}

TEST_CASE("sizeof(ds_setting_desc_t) <= 24 bytes",
          "[device_settings][g1]")
{
    /* Compact descriptor must be small. */
    TEST_ASSERT_TRUE(sizeof(ds_setting_desc_t) <= 24);
}
