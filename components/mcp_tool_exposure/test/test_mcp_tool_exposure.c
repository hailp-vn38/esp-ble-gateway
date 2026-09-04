#include <string.h>

#include "../mcp_tool_exposure_internal.h"
#include "mcp_tool_exposure.h"
#include "unity.h"

TEST_CASE("exposure init succeeds and snapshot is empty after boot",
          "[mcp_tool_exposure]")
{
    TEST_ASSERT_EQUAL(ESP_OK, mcp_tool_exposure_init());

    mcp_tool_exposure_t buf[4];
    size_t count = 0;
    TEST_ASSERT_EQUAL(ESP_OK,
        mcp_tool_exposure_snapshot(buf, 4, &count));
    TEST_ASSERT_EQUAL_UINT(0, count);
}

TEST_CASE("exposure capacity reports correct limits",
          "[mcp_tool_exposure]")
{
    TEST_ASSERT_EQUAL(ESP_OK, mcp_tool_exposure_init());

    mcp_exposure_capacity_t cap = {0};
    TEST_ASSERT_EQUAL(ESP_OK, mcp_tool_exposure_get_capacity(&cap));
    TEST_ASSERT_EQUAL_UINT(0, cap.enabled);
}
