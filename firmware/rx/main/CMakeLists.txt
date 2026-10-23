# The transmitter's test pattern (components/ingress) is compiled in for `rx ... ts pattern`;
# the rest of that component is the transmitter's SPI input.
# esp_wifi only for its Kconfig: periph_ctrl's wifi_module_enable() needs ESP_WIFI_ENABLED.
# No Wi-Fi code is called, so none is linked.
idf_component_register(SRCS "main.c" "arena.c" "capture.c" "receive.c" "replay.c"
                            "../../components/ingress/ingress_pattern.c"
                       INCLUDE_DIRS "." "../../components/ingress/include"
                       PRIV_REQUIRES esp_driver_usb_serial_jtag vfs esp_wifi esp_phy esp_psram
                                     esp_timer esp_app_format esp_hw_support heap esp_driver_gptimer dvbs2rx)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-T${CMAKE_CURRENT_LIST_DIR}/banks.ld")
