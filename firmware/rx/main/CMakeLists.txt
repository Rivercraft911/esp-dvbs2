# The transmitter's test pattern (components/ingress) is compiled in for `rx ... ts pattern`;
# the rest of that component is the transmitter's SPI input.
idf_component_register(SRCS "main.c" "arena.c" "capture.c" "eth_egress.c" "mon_pack.c" "receive.c" "replay.c"
                            "../../components/ingress/ingress_pattern.c"
                       INCLUDE_DIRS "." "../../components/ingress/include"
                       PRIV_REQUIRES esp_driver_usb_serial_jtag vfs esp_phy esp_psram
                                     esp_timer esp_app_format esp_hw_support heap esp_driver_gptimer dvbs2rx platform
                                     w5500)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-T${CMAKE_CURRENT_LIST_DIR}/banks.ld")
