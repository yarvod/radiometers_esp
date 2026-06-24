idf_component_register(
    SRCS "motion_controller.cpp"
    INCLUDE_DIRS "."
    REQUIRES app_core
    PRIV_REQUIRES storage_manager data_logger sensor_hub upload_pipeline gps_module driver json esp_timer esp_rom
)
set_property(TARGET ${COMPONENT_LIB} PROPERTY CXX_STANDARD 17)
