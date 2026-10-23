idf_component_register(SRCS "main.c" "spi_input.c" "usb_input.c" "station_commands.c" "dvbtx.c" "fecbench.c" "ring.c"
                       INCLUDE_DIRS "."
                       LDFRAGMENTS "stream.lf"
                       PRIV_REQUIRES esp_driver_usb_serial_jtag esp_hal_usb vfs esp_psram esp_app_format
                                     esp_hw_support esp_timer ingress esp_driver_spi esp_driver_gpio radio_s3 dvbs2 station_id
                                     shaper)
target_link_options(${COMPONENT_LIB} INTERFACE "-T${CMAKE_CURRENT_LIST_DIR}/stream_assert.ld")
# INSERT scripts must precede IDF's complete sections script on the link line.
idf_build_set_property(LINK_OPTIONS "-T${CMAKE_CURRENT_LIST_DIR}/vcm_iram.ld" APPEND)
set_property(TARGET ${COMPONENT_LIB} APPEND PROPERTY INTERFACE_LINK_DEPENDS
             "${CMAKE_CURRENT_LIST_DIR}/vcm_iram.ld" "${CMAKE_CURRENT_LIST_DIR}/stream_assert.ld")
