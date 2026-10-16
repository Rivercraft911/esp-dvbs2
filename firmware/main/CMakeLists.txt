idf_component_register(SRCS "main.c" "membench.c" "shaper.c" "shaper_pie.S"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_driver_usb_serial_jtag vfs esp_psram esp_app_format
                                     esp_hw_support esp_timer)
