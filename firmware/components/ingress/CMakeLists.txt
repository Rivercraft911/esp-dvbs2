set(srcs "ingress.c" "ingress_pattern.c" "ingress_spi.c" "ingress_spi_pv.c")
if(IDF_TARGET STREQUAL "esp32s3")
    list(APPEND srcs "ingress_pie.S")
endif()
idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_spi esp_driver_gpio esp_driver_dma esp_hal_gpspi
                                     esp_hal_dma esp_hal_gpio esp_hw_support esp_rom)
target_compile_definitions(${COMPONENT_LIB} PRIVATE INGRESS_IRAM=1)
if(IDF_TARGET STREQUAL "esp32s3")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE INGRESS_PIE=1)
endif()
