idf_component_register(
    SRCS
        "app_main.cpp"
        "http_handlers.cpp"
        "web_ui.cpp"
        "control_actions.cpp"
        "mqtt_bridge.cpp"
        "wn90lp.cpp"

    REQUIRES
        app_core
        storage_manager
        config_loader
        network_manager
        sensor_hub
        upload_pipeline
        data_logger
        gps_module
        motion_controller

    PRIV_REQUIRES
        esp_wifi
        esp_http_server
        esp_http_client
        esp_netif
        esp_eth
        nvs_flash
        driver
        json
        esp_timer
        esp_rom
        esp_psram
        tinyusb
        esp_tinyusb
        sdmmc
        fatfs
        spi_flash
        wear_levelling
        onewire_bus
        efuse
        mqtt
    INCLUDE_DIRS "."
)

set_property(TARGET ${COMPONENT_LIB} PROPERTY CXX_STANDARD 17)
