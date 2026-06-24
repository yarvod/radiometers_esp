idf_component_register(
    SRCS "storage_manager.cpp"
    INCLUDE_DIRS "."
    REQUIRES sdmmc
    PRIV_REQUIRES app_core fatfs spi_flash wear_levelling driver
)
set_property(TARGET ${COMPONENT_LIB} PROPERTY CXX_STANDARD 17)
