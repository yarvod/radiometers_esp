idf_component_register(
    SRCS "data_logger.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES app_core storage_manager gps_module network_manager sensor_hub esp_timer
)
set_property(TARGET ${COMPONENT_LIB} PROPERTY CXX_STANDARD 17)
