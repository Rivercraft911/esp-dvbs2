idf_component_register(
    SRCS "bench.c"
         "../../../firmware/rx/dsp/rx_filter.c"
         "../../../firmware/rx/dsp/rx_filter_pie.S"
         "../../../firmware/rx/fec/rx_mc6.c"
         "../../../firmware/rx/fec/mc6_pie.S"
    INCLUDE_DIRS "../../../firmware/main" "../../../firmware/rx/dsp" "../../../firmware/rx/fec"
    PRIV_REQUIRES dvbs2 esp_driver_usb_serial_jtag esp_psram esp_timer esp_app_format esp_hw_support heap vfs)
target_compile_definitions(${COMPONENT_LIB} PRIVATE RX_FILTER_BENCH=1 RX_FILTER_BATCH=1 RX_MC6_TEST_HOOKS=1)
