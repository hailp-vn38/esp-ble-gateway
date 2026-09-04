set(setup_html "${CMAKE_CURRENT_SOURCE_DIR}/www/setup.html")
set(setup_html_gz "${CMAKE_CURRENT_BINARY_DIR}/setup.html.gz")
set(gzip_asset_script "${CMAKE_CURRENT_SOURCE_DIR}/gzip_asset.py")

# Web UI build pipeline: assemble dashboard from www_src/
set(webui_source "${CMAKE_CURRENT_SOURCE_DIR}/www_src")
set(webui_builder "${CMAKE_CURRENT_SOURCE_DIR}/tools/build_webui.py")
set(dashboard_generated "${CMAKE_CURRENT_BINARY_DIR}/dashboard.html")
set(dashboard_generated_gz "${CMAKE_CURRENT_BINARY_DIR}/dashboard.html.gz")

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    idf_build_get_property(python PYTHON)

    # Compress setup.html for provisioning
    add_custom_command(
        OUTPUT "${setup_html_gz}"
        COMMAND "${python}" "${gzip_asset_script}" "${setup_html}" "${setup_html_gz}"
        DEPENDS "${setup_html}" "${gzip_asset_script}"
        COMMENT "Compressing setup.html for the provisioning web server"
        VERBATIM
    )
    set_source_files_properties("${setup_html_gz}" PROPERTIES GENERATED TRUE)

    # Build dashboard from modular sources
    file(GLOB_RECURSE WEBUI_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/www_src/*.html"
        "${CMAKE_CURRENT_SOURCE_DIR}/www_src/*.js"
    )

    add_custom_command(
        OUTPUT "${dashboard_generated}"
        COMMAND "${python}" "${webui_builder}"
                --source "${webui_source}"
                --dashboard-out "${dashboard_generated}"
        DEPENDS ${WEBUI_SOURCES} "${webui_builder}"
        COMMENT "Building web UI assets from www_src/"
        VERBATIM
    )

    add_custom_command(
        OUTPUT "${dashboard_generated_gz}"
        COMMAND "${python}" "${gzip_asset_script}"
                "${dashboard_generated}" "${dashboard_generated_gz}"
        DEPENDS "${dashboard_generated}" "${gzip_asset_script}"
        COMMENT "Compressing dashboard web assets"
        VERBATIM
    )

    set_source_files_properties(
        "${dashboard_generated}"
        "${dashboard_generated_gz}"
        PROPERTIES GENERATED TRUE
    )
endif()

idf_component_register(
    SRCS "web_server.c"
         "web_http.c"
         "web_assets.c"
         "web_gateway_api.c"
         "web_device_api.c"
         "web_command_api.c"
         "web_device_schema_api.c"
         "web_exposure_api.c"
         "web_system_api.c"
         "web_wifi_api.c"
         "web_ble_api.c"
         "web_mcp_token_api.c"
         "web_settings_api.c"
         "web_event_ws.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server cbor_codec command_dispatcher command_executor
             device_command_service device_management device_store gateway_status wifi_provisioning
             ble_central device_schema device_template device_state
             mcp_tool_exposure mcp_ws_bridge gateway_events
             espressif__cjson esp_system esp_timer freertos
             esp_wifi esp_app_format nvs_flash
    EMBED_FILES
             "${dashboard_generated_gz}"
             "${setup_html_gz}"
             "www/dashboard.css"
             "www/icons.css"
             "www/assets/Phosphor.woff2"
)
