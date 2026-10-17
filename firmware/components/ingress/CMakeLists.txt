idf_component_register(SRCS "ingress.c" "ingress_pattern.c" "ingress_spi.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_spi esp_driver_gpio esp_driver_dma esp_hal_gpspi
                                     esp_hal_dma esp_hal_gpio esp_hw_support esp_rom)
target_compile_definitions(${COMPONENT_LIB} PRIVATE INGRESS_IRAM=1)
