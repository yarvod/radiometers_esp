idf_component_register(
    SRCS
        "app_main.cpp"
        "ltc2440.cpp"
        "onewire_ds18b20.cpp"
       
    PRIV_REQUIRES
        esp_wifi
        esp_http_server
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
        ds18b20
        onewire_bus
    INCLUDE_DIRS "."
)

set_property(TARGET ${COMPONENT_LIB} PROPERTY CXX_STANDARD 17)
