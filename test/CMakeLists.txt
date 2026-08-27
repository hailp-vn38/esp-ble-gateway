cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS "../components")
set(TEST_COMPONENTS "device_store;device_capabilities;cbor_codec;command_dispatcher;command_executor;log_buffer;mcp_endpoint;wifi_provisioning;ble_central;web_server;board_io;message_trace" CACHE STRING
    "Gateway components whose unit tests are included")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(esp32_ble_gateway_tests)
