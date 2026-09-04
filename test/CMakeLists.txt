cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS "../components")
set(TEST_COMPONENTS "device_types;device_store;device_schema;device_state;device_template;cbor_codec;device_command_service;device_management;mcp_endpoint;mcp_tool_exposure;mcp_ws_bridge;wifi_provisioning;ble_central;web_server;board_io;memory_policy;gateway_events" CACHE STRING
    "Gateway components whose unit tests are included")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(esp32_ble_gateway_tests)
