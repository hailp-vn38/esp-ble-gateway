cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS "../components")
set(TEST_COMPONENTS "device_store;cbor_codec;command_dispatcher" CACHE STRING
    "Gateway components whose unit tests are included")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(esp32_ble_gateway_tests)
