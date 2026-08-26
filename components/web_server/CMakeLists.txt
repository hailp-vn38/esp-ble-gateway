set(setup_html "${CMAKE_CURRENT_SOURCE_DIR}/www/setup.html")
set(setup_html_gz "${CMAKE_CURRENT_BINARY_DIR}/setup.html.gz")
set(gzip_asset_script "${CMAKE_CURRENT_SOURCE_DIR}/gzip_asset.py")

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    idf_build_get_property(python PYTHON)
    add_custom_command(
        OUTPUT "${setup_html_gz}"
        COMMAND "${python}" "${gzip_asset_script}" "${setup_html}" "${setup_html_gz}"
        DEPENDS "${setup_html}" "${gzip_asset_script}"
        COMMENT "Compressing setup.html for the provisioning web server"
        VERBATIM
    )
    set_source_files_properties("${setup_html_gz}" PROPERTIES GENERATED TRUE)
endif()

idf_component_register(
    SRCS "web_server.c"
         "web_http.c"
         "web_assets.c"
         "web_gateway_api.c"
         "web_system_api.c"
         "web_wifi_api.c"
         "web_ble_api.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server cbor_codec command_dispatcher command_executor
             device_store gateway_status log_buffer wifi_provisioning
             ble_central espressif__cjson esp_system esp_timer freertos
             esp_wifi esp_app_format
    EMBED_FILES
             "www/dashboard.html"
             "${setup_html_gz}"
             "www/dashboard.css"
             "www/icons.css"
             "www/assets/Phosphor.woff2"
)
