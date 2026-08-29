#include "mcp_ws_bridge.h"

#include <string.h>

#include "nvs_flash.h"
#include "unity.h"

static void erase_and_init_nvs(void)
{
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
}

TEST_CASE("get_status when bridge not initialized reads NVS", "[mcp_ws_bridge]")
{
    erase_and_init_nvs();

    mcp_ws_status_t status = {0};
    esp_err_t result = mcp_ws_bridge_get_status(&status);
    TEST_ASSERT_EQUAL(ESP_OK, result);
    TEST_ASSERT_FALSE(status.enabled);
    TEST_ASSERT_FALSE(status.runtime_enabled);
    TEST_ASSERT_FALSE(status.restart_required);
    TEST_ASSERT_FALSE(status.endpoint_configured);
    TEST_ASSERT_EQUAL(MCP_WS_DISABLED, status.state);
}

TEST_CASE("get_status reflects persisted enabled state", "[mcp_ws_bridge]")
{
    erase_and_init_nvs();

    nvs_handle_t nvs;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_open("mcp_ws", NVS_READWRITE, &nvs));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_u8(nvs, "enabled", 1));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_str(nvs, "endpoint", "wss://test.example.com/mcp"));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_commit(nvs));
    nvs_close(nvs);

    mcp_ws_status_t status = {0};
    esp_err_t result = mcp_ws_bridge_get_status(&status);
    TEST_ASSERT_EQUAL(ESP_OK, result);
    TEST_ASSERT_TRUE(status.enabled);
    TEST_ASSERT_FALSE(status.runtime_enabled);
    TEST_ASSERT_TRUE(status.restart_required);
    TEST_ASSERT_TRUE(status.endpoint_configured);
    TEST_ASSERT_EQUAL(MCP_WS_DISABLED, status.state);
}

TEST_CASE("get_status null arg returns invalid arg", "[mcp_ws_bridge]")
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, mcp_ws_bridge_get_status(NULL));
}

TEST_CASE("config_get_public when bridge not initialized", "[mcp_ws_bridge]")
{
    erase_and_init_nvs();

    mcp_ws_public_config_t config = {0};
    esp_err_t result = mcp_ws_bridge_config_get_public(&config);
    TEST_ASSERT_EQUAL(ESP_OK, result);
    TEST_ASSERT_FALSE(config.enabled);
    TEST_ASSERT_FALSE(config.endpoint_configured);
    TEST_ASSERT_EQUAL('\0', config.endpoint_display[0]);
}

TEST_CASE("config_get_public reads stored endpoint display", "[mcp_ws_bridge]")
{
    erase_and_init_nvs();

    nvs_handle_t nvs;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_open("mcp_ws", NVS_READWRITE, &nvs));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_u8(nvs, "enabled", 1));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_str(nvs, "endpoint",
                                           "wss://api.xiaozhi.me/mcp/?token=abc123secret"));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_commit(nvs));
    nvs_close(nvs);

    mcp_ws_public_config_t config = {0};
    esp_err_t result = mcp_ws_bridge_config_get_public(&config);
    TEST_ASSERT_EQUAL(ESP_OK, result);
    TEST_ASSERT_TRUE(config.enabled);
    TEST_ASSERT_TRUE(config.endpoint_configured);
    TEST_ASSERT_NOT_EQUAL('\0', config.endpoint_display[0]);
    TEST_ASSERT_FALSE(strstr(config.endpoint_display, "abc123secret") != NULL);
}

TEST_CASE("config_get_public null returns invalid arg", "[mcp_ws_bridge]")
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, mcp_ws_bridge_config_get_public(NULL));
}

TEST_CASE("config_load public API works", "[mcp_ws_bridge]")
{
    erase_and_init_nvs();

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

TEST_CASE("config_load null returns invalid arg", "[mcp_ws_bridge]")
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, mcp_ws_bridge_config_load(NULL));
}

TEST_CASE("config_update persists without bridge init", "[mcp_ws_bridge]")
{
    erase_and_init_nvs();

    esp_err_t result = mcp_ws_bridge_config_update(
        true, true, true, "wss://new.example.com/mcp");
    TEST_ASSERT_EQUAL(ESP_OK, result);

    mcp_ws_config_t config = {0};
    result = mcp_ws_bridge_config_load(&config);
    TEST_ASSERT_EQUAL(ESP_OK, result);
    TEST_ASSERT_TRUE(config.enabled);
    TEST_ASSERT_EQUAL_STRING("wss://new.example.com/mcp", config.endpoint);
}

TEST_CASE("config_update endpoint only persists correctly", "[mcp_ws_bridge]")
{
    erase_and_init_nvs();

    esp_err_t result = mcp_ws_bridge_config_update(
        true, true, true, "wss://first.example.com/mcp");
    TEST_ASSERT_EQUAL(ESP_OK, result);

    result = mcp_ws_bridge_config_update(
        false, false, true, "wss://second.example.com/mcp");
    TEST_ASSERT_EQUAL(ESP_OK, result);

    mcp_ws_config_t config = {0};
    result = mcp_ws_bridge_config_load(&config);
    TEST_ASSERT_EQUAL(ESP_OK, result);
    TEST_ASSERT_TRUE(config.enabled);
    TEST_ASSERT_EQUAL_STRING("wss://second.example.com/mcp", config.endpoint);
}

TEST_CASE("config_update invalid endpoint returns invalid arg", "[mcp_ws_bridge]")
{
    erase_and_init_nvs();

    esp_err_t result = mcp_ws_bridge_config_update(
        false, false, true, "not-a-valid-endpoint");
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, result);
}

TEST_CASE("restart_required false when enabled matches runtime after init", "[mcp_ws_bridge]")
{
    erase_and_init_nvs();

    mcp_ws_status_t status = {0};
    esp_err_t result = mcp_ws_bridge_get_status(&status);
    TEST_ASSERT_EQUAL(ESP_OK, result);
    TEST_ASSERT_FALSE(status.enabled);
    TEST_ASSERT_FALSE(status.runtime_enabled);
    TEST_ASSERT_FALSE(status.restart_required);
}
