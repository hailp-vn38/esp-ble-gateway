#include <string.h>

#include "../mcp_tool_exposure_internal.h"
#include "mcp_tool_exposure.h"
#include "unity.h"

TEST_CASE("exposure init creates the executable MCP catalog",
          "[mcp_tool_exposure]")
{
    TEST_ASSERT_EQUAL(ESP_OK, mcp_tool_exposure_init());

    mcp_tool_binding_t binding = {0};
    strlcpy(binding.tool_name, "catalog_init_regression",
            sizeof(binding.tool_name));
    strlcpy(binding.device_id, "catalog-test-device",
            sizeof(binding.device_id));
    strlcpy(binding.command, "set_state", sizeof(binding.command));
    strlcpy(binding.capability.command, binding.command,
            sizeof(binding.capability.command));

    TEST_ASSERT_EQUAL(ESP_OK, mcp_tool_catalog_add(&binding));

    const mcp_tool_binding_t *published =
        mcp_tool_catalog_find_ptr(binding.tool_name);
    TEST_ASSERT_NOT_NULL(published);
    TEST_ASSERT_EQUAL_STRING(binding.device_id, published->device_id);
    TEST_ASSERT_EQUAL_STRING(binding.command, published->command);

    TEST_ASSERT_EQUAL(ESP_OK, mcp_tool_catalog_remove(binding.tool_name));
    TEST_ASSERT_NULL(mcp_tool_catalog_find_ptr(binding.tool_name));
}
