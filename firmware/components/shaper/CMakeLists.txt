# Transmit pulse shaping: table builders and the PIE kernels the bank fill runs on core 1.
# shaper.lf and shaper_assert.ld pin the kernels' placement and the 8PSK lock region.
idf_component_register(SRCS "shaper.c" "shaper_axes.c" "shaper_axes_pie.S" "shaper_pie.S"
                            "shaper_qpsk.c" "shaper_qpsk_pie.S"
                            "shaper_short.c" "shaper_short_pie.S"
                            "machine.c" "lofty.c" "lofty_pie.S"
                            "machine_axis_pie.S"
                       INCLUDE_DIRS "include"
                       LDFRAGMENTS "shaper.lf"
                       PRIV_REQUIRES esp_hw_support esp_rom)
target_link_options(${COMPONENT_LIB} INTERFACE "-T${CMAKE_CURRENT_LIST_DIR}/shaper_assert.ld")
set_property(TARGET ${COMPONENT_LIB} APPEND PROPERTY INTERFACE_LINK_DEPENDS
             "${CMAKE_CURRENT_LIST_DIR}/shaper_assert.ld")
