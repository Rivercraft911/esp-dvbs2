idf_component_register(SRCS "main.c" "capture.c" "samples.c" "spill.c"
                       INCLUDE_DIRS "." "../../main"
                       LDFRAGMENTS "spill.lf"
                       PRIV_REQUIRES esp_driver_usb_serial_jtag esp_wifi esp_phy esp_psram
                                     esp_timer esp_app_format esp_hw_support heap esp_driver_dma esp_mm)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-T${CMAKE_CURRENT_LIST_DIR}/banks.ld")
if(DEFINED RX_GAIN_REFRESH_CYCLES)
  target_compile_definitions(${COMPONENT_LIB} PRIVATE RX_GAIN_REFRESH_CYCLES=${RX_GAIN_REFRESH_CYCLES})
endif()
