idf_component_register(SRCS "radio_s3.c" "radio_signals.c" "radio_filter.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_phy esp_hw_support esp_hal_systimer esp_pm esp_timer heap station_id platform)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-T${CMAKE_CURRENT_LIST_DIR}/radio_banks.ld")
