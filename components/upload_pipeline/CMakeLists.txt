idf_component_register(
    SRCS "upload_pipeline.cpp" "sd_maintenance.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES app_core storage_manager gps_module data_logger esp_http_client mbedtls esp_psram esp_timer json fatfs
    EMBED_TXTFILES "ca.crt" "isrgrootx1.pem"
)
set_property(TARGET ${COMPONENT_LIB} PROPERTY CXX_STANDARD 17)
