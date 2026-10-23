# w5500.c, w5500_settings.c and w5500_gpspi.c are portable (tests/test_w5500.py). The SPI backend
# is chosen by CONFIG_W5500_DIRECT_SPI. Requirements cannot depend on Kconfig, so both drivers
# are listed; only the chosen backend's calls are linked.
set(srcs "w5500.c" "w5500_settings.c" "w5500_gpspi.c" "w5500_eth.c")
if(CONFIG_W5500_DIRECT_SPI)
    list(APPEND srcs "w5500_spi_direct.c")
else()
    list(APPEND srcs "w5500_spi_idf.c")
endif()
idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_spi esp_driver_dma esp_hw_support esp_timer esp_rom
                                     nvs_flash heap)
