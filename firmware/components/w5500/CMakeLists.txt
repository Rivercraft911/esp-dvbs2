# w5500.c and w5500_settings.c are portable (tests/test_w5500.py); the rest is the ESP-IDF side.
idf_component_register(SRCS "w5500.c" "w5500_settings.c" "w5500_eth.c" "w5500_spi_idf.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_spi esp_hw_support esp_timer esp_rom nvs_flash heap)
