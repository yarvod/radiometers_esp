idf_component_register(
    SRCS "sensor_hub.cpp" "ltc2440.cpp" "onewire_m1820.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES app_core storage_manager driver onewire_bus esp_timer
)
set_property(TARGET ${COMPONENT_LIB} PROPERTY CXX_STANDARD 17)
