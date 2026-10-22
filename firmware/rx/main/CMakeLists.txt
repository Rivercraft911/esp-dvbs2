idf_component_register(SRCS "main.c" "capture.c" "receive.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_driver_usb_serial_jtag vfs esp_wifi esp_phy esp_psram
                                     esp_timer esp_app_format esp_hw_support heap esp_driver_dma dvbs2rx)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-T${CMAKE_CURRENT_LIST_DIR}/banks.ld")
