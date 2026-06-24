idf_component_register(
    SRCS "network_manager.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES app_core storage_manager sensor_hub esp_wifi esp_netif esp_eth efuse nvs_flash esp_timer
)
set_property(TARGET ${COMPONENT_LIB} PROPERTY CXX_STANDARD 17)
