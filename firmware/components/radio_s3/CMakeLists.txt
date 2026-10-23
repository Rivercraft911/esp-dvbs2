idf_component_register(SRCS "radio_s3.c" "radio_signals.c" "radio_filter.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_wifi esp_phy esp_hw_support esp_pm esp_timer heap station_id)
# esp_wifi only for its Kconfig: periph_ctrl's wifi_module_enable() needs ESP_WIFI_ENABLED.
# radio_s3_init never starts Wi-Fi, so no Wi-Fi code is linked.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-T${CMAKE_CURRENT_LIST_DIR}/radio_banks.ld")
