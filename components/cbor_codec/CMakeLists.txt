idf_component_register(
    SRCS "cbor_codec.c"
    INCLUDE_DIRS "include"
    REQUIRES qcbor_lib espressif__cjson device_types
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-function)
