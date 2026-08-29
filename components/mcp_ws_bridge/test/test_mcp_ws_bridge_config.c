#include "mcp_ws_bridge.h"

#include <string.h>

#include "nvs_flash.h"
#include "unity.h"

TEST_CASE("mcp_ws_bridge_is_supported returns true", "[mcp_ws_bridge]")
{
    TEST_ASSERT_TRUE(mcp_ws_bridge_is_supported());
}

TEST_CASE("mcp_ws_bridge_state_name covers all states", "[mcp_ws_bridge]")
{
    TEST_ASSERT_EQUAL_STRING("disabled", mcp_ws_bridge_state_name(MCP_WS_DISABLED));
    TEST_ASSERT_EQUAL_STRING("wait_network", mcp_ws_bridge_state_name(MCP_WS_WAIT_NETWORK));
    TEST_ASSERT_EQUAL_STRING("connecting", mcp_ws_bridge_state_name(MCP_WS_CONNECTING));
    TEST_ASSERT_EQUAL_STRING("handshaking", mcp_ws_bridge_state_name(MCP_WS_HANDSHAKING));
    TEST_ASSERT_EQUAL_STRING("connected", mcp_ws_bridge_state_name(MCP_WS_READY));
    TEST_ASSERT_EQUAL_STRING("backoff", mcp_ws_bridge_state_name(MCP_WS_BACKOFF));
    TEST_ASSERT_EQUAL_STRING("error", mcp_ws_bridge_state_name(MCP_WS_ERROR));
}

TEST_CASE("mcp_ws_bridge_config_load with empty NVS uses default", "[mcp_ws_bridge]")
{
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());

    mcp_ws_config_t config = {0};
    esp_err_t result = mcp_ws_bridge_config_load(&config);
    TEST_ASSERT_EQUAL(ESP_OK, result);
    TEST_ASSERT_FALSE(config.enabled);
    TEST_ASSERT_EQUAL('\0', config.endpoint[0]);
}

TEST_CASE("mcp_ws_bridge_config_load reads stored values", "[mcp_ws_bridge]")
{
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());

    nvs_handle_t nvs;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_open("mcp_ws", NVS_READWRITE, &nvs));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_u8(nvs, "enabled", 1));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_str(nvs, "endpoint", "wss://test.example.com/mcp"));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_commit(nvs));
    nvs_close(nvs);

    mcp_ws_config_t config = {0};
    esp_err_t result = mcp_ws_bridge_config_load(&config);
    TEST_ASSERT_EQUAL(ESP_OK, result);
    TEST_ASSERT_TRUE(config.enabled);
    TEST_ASSERT_EQUAL_STRING("wss://test.example.com/mcp", config.endpoint);
}

TEST_CASE("mcp_ws_bridge_config_load invalid endpoint clears config", "[mcp_ws_bridge]")
{
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());

    nvs_handle_t nvs;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_open("mcp_ws", NVS_READWRITE, &nvs));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_u8(nvs, "enabled", 1));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_str(nvs, "endpoint", "invalid-endpoint"));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_commit(nvs));
    nvs_close(nvs);

    mcp_ws_config_t config = {0};
    esp_err_t result = mcp_ws_bridge_config_load(&config);
    TEST_ASSERT_EQUAL(ESP_OK, result);
    TEST_ASSERT_FALSE(config.enabled);
    TEST_ASSERT_EQUAL('\0', config.endpoint[0]);
}

TEST_CASE("mcp_ws_bridge_config_load erases endpoint when empty", "[mcp_ws_bridge]")
{
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());

    nvs_handle_t nvs;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_open("mcp_ws", NVS_READWRITE, &nvs));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_u8(nvs, "enabled", 1));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_commit(nvs));
    nvs_close(nvs);

    mcp_ws_config_t config = {0};
    esp_err_t result = mcp_ws_bridge_config_load(&config);
    TEST_ASSERT_EQUAL(ESP_OK, result);
    TEST_ASSERT_TRUE(config.enabled);
    TEST_ASSERT_EQUAL('\0', config.endpoint[0]);
}
