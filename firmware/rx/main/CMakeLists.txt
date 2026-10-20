idf_component_register(SRCS "main.c" "capture.c" "samples.c"
                       INCLUDE_DIRS "." "../../main"
                       PRIV_REQUIRES esp_driver_usb_serial_jtag esp_wifi esp_phy esp_psram
                                     esp_timer esp_app_format esp_hw_support heap)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-T${CMAKE_CURRENT_LIST_DIR}/banks.ld")
