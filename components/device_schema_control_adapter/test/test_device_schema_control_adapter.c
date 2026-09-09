#include "device_schema_control_adapter.h"

#include "unity.h"

/* Keeps the adapter reachable in the MINIMAL_BUILD test image without
 * overwriting the mock submitter owned by device_schema's existing tests. */
TEST_CASE("schema control adapter is linked independently", "[device_schema_control_adapter]")
{
    TEST_ASSERT_NOT_NULL(device_schema_control_adapter_init);
}
