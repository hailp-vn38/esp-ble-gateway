idf_component_register(
    SRCS
        "QCBOR/src/UsefulBuf.c"
        "QCBOR/src/ieee754.c"
        "QCBOR/src/qcbor_decode.c"
        "QCBOR/src/qcbor_encode.c"
        "QCBOR/src/qcbor_err_to_str.c"
    INCLUDE_DIRS "QCBOR/inc"
    PRIV_INCLUDE_DIRS "QCBOR/src"
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC
    USEFULBUF_DISABLE_ALL_FLOAT
    QCBOR_DISABLE_NON_INTEGER_LABELS
    QCBOR_DISABLE_TAGS
    QCBOR_DISABLE_EXP_AND_MANTISSA
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-function
    -Wno-unused-parameter
    -Wno-sign-compare
)
