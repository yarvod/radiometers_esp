idf_component_register(
    SRCS "app_state.cpp" "app_utils.cpp" "error_manager.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_http_server sdmmc driver
    PRIV_REQUIRES json nvs_flash esp_timer
)
set_property(TARGET ${COMPONENT_LIB} PROPERTY CXX_STANDARD 17)
