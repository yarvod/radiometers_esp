idf_component_register(
    SRCS "gps_module.cpp" "gps_unicore.cpp"
    INCLUDE_DIRS "."
    REQUIRES app_core
    PRIV_REQUIRES storage_manager network_manager upload_pipeline driver esp_timer
)
set_property(TARGET ${COMPONENT_LIB} PROPERTY CXX_STANDARD 17)
