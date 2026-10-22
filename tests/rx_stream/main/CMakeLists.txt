idf_component_register(
    SRCS "main.c" "collector.c" "dma.c" "rrc.c" "rrc.S" "controls.c" "controls.S"
         "../../../firmware/rx/dsp/rx_halfband.c"
         "../../../firmware/rx/dsp/rx_halfband.S"
         "../../../firmware/rx/stream/rx_queue.c"
         "../../../firmware/rx/main/samples.c"
    INCLUDE_DIRS "." "../../../firmware/main" "../../../firmware/rx/main"
                 "../../../firmware/rx/dsp" "../../../firmware/rx/stream"
    LDFRAGMENTS "../../../firmware/rx/main/spill.lf"
    PRIV_REQUIRES esp_driver_usb_serial_jtag esp_wifi esp_phy esp_psram
                  esp_timer esp_app_format esp_hw_support heap esp_driver_dma esp_mm)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-T${CMAKE_CURRENT_LIST_DIR}/../../../firmware/rx/main/banks.ld")
target_compile_definitions(${COMPONENT_LIB} PRIVATE scan8_pie=rx_pack_scan8)
target_compile_options(${COMPONENT_LIB} PRIVATE -fstack-usage)
if(DEFINED RX_STREAM_UNITS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE RX_STREAM_UNITS=${RX_STREAM_UNITS})
endif()
