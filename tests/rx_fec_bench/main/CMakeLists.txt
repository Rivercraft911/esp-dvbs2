idf_component_register(SRCS "../../rx_fec_bench.c"
                       PRIV_REQUIRES dvbs2rx dvbs2 esp_driver_usb_serial_jtag vfs esp_hw_support
                                     esp_rom heap)
