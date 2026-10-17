idf_component_register(SRCS "main.c" "station_commands.c" "dvbtx.c" "ring.c" "membench.c" "fecbench.c" "shaper.c" "shaper_pie.S"
                       "shaper_qpsk.c" "shaper_qpsk_pie.S"
                       "shaper_short.c" "shaper_short_pie.S"
                       INCLUDE_DIRS "."
                       LDFRAGMENTS "stream.lf"
                       PRIV_REQUIRES esp_driver_usb_serial_jtag vfs esp_psram esp_app_format
                                     esp_hw_support esp_timer radio_s3 dvbs2 station_id)
target_link_options(${COMPONENT_LIB} INTERFACE "-T${CMAKE_CURRENT_LIST_DIR}/stream_assert.ld")
