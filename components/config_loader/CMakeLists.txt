idf_component_register(
    SRCS "config_loader.cpp"
    INCLUDE_DIRS "."
    REQUIRES app_core
    PRIV_REQUIRES storage_manager nvs_flash sdmmc driver fatfs
)
set_property(TARGET ${COMPONENT_LIB} PROPERTY CXX_STANDARD 17)
