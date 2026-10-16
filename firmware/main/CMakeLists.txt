idf_component_register(SRCS "main.c" "station_commands.c" "dvbtx.c" "ring.c" "membench.c" "fecbench.c" "shaper.c" "shaper_pie.S"
                       "shaper_apsk.c" "shaper_apsk_pie.S"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_driver_usb_serial_jtag vfs esp_psram esp_app_format
                                     esp_hw_support esp_timer radio_s3 dvbs2 station_id)
