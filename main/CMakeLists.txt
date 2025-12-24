idf_component_register(
    SRCS
        "app_main.cpp"
        "app_state.cpp"
        "http_handlers.cpp"
        "web_ui.cpp"
        "ltc2440.cpp"
        "onewire_m1820.cpp"
        "control_actions.cpp"
        "mqtt_bridge.cpp"
       
    PRIV_REQUIRES
        esp_wifi
        esp_http_server
        esp_http_client
        esp_netif
        nvs_flash
        driver
        json
        esp_timer
        esp_rom
        tinyusb
        esp_tinyusb
        sdmmc
        fatfs
        onewire_bus
        mqtt
    INCLUDE_DIRS "."
    EMBED_TXTFILES
        "ca.crt"
)

set_property(TARGET ${COMPONENT_LIB} PROPERTY CXX_STANDARD 17)
