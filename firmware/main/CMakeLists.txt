idf_component_register(SRCS "main.c" "spi_input.c" "usb_input.c" "station_commands.c" "dvbtx.c" "ring.c" "shaper.c" "shaper_axes.c" "shaper_axes_pie.S" "shaper_pie.S"
                       "shaper_qpsk.c" "shaper_qpsk_pie.S"
                       "shaper_short.c" "shaper_short_pie.S"
                       "machine.c" "machine_pie.S" "lofty.c" "lofty_pie.S" "lofty_fused.c" "lofty_fused_pie.S"
                       "machine_axis_pie.S"
                       INCLUDE_DIRS "."
                       LDFRAGMENTS "stream.lf"
                       PRIV_REQUIRES esp_driver_usb_serial_jtag esp_hal_usb vfs esp_psram esp_app_format
                                     esp_hw_support esp_timer ingress esp_driver_spi esp_driver_gpio radio_s3 dvbs2 station_id)
target_link_options(${COMPONENT_LIB} INTERFACE "-T${CMAKE_CURRENT_LIST_DIR}/stream_assert.ld")
