idf_component_register(
    SRCS "wn90lp.cpp"
    INCLUDE_DIRS "."
    REQUIRES app_core
    PRIV_REQUIRES storage_manager driver esp_timer gps_module
)
set_property(TARGET ${COMPONENT_LIB} PROPERTY CXX_STANDARD 17)
