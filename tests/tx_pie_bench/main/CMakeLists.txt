# The shaper sources build here instead of through their component: its link script asserts the
# transmitter's own code layout. dvbs2 comes as a component, with its PIE build flags.
set(fw "../../../firmware")
idf_component_register(SRCS "../../tx_pie_bench.c" "../../qpsk_axes.c" "../../psk8_kernels.c"
                            "../../shaper_axes.c"
                            "${fw}/components/shaper/shaper.c" "${fw}/components/shaper/shaper_pie.S"
                            "${fw}/components/shaper/shaper_axes.c"
                            "${fw}/components/shaper/shaper_axes_pie.S"
                            "${fw}/components/shaper/shaper_qpsk.c"
                            "${fw}/components/shaper/shaper_qpsk_pie.S"
                            "${fw}/components/shaper/shaper_short.c"
                            "${fw}/components/shaper/shaper_short_pie.S"
                            "${fw}/components/shaper/machine.c"
                            "${fw}/components/shaper/machine_axis_pie.S"
                            "${fw}/components/shaper/lofty.c" "${fw}/components/shaper/lofty_pie.S"
                            "${fw}/tx/main/ring.c" "${fw}/tx/main/payload_pie.S"
                       INCLUDE_DIRS "." "${fw}/components/shaper/include" "${fw}/tx/main"
                       LDFRAGMENTS "bench.lf" "${fw}/components/shaper/shaper.lf"
                       PRIV_REQUIRES dvbs2 ingress esp_driver_usb_serial_jtag vfs esp_hw_support esp_rom
                                     esp_timer heap soc esp_app_format)
# Host-only checks, compiled for the board: rename their entry points, and declare the machine
# oracles that machine.c builds for the host only (tx_pie_bench.c defines them).
set_source_files_properties("../../psk8_kernels.c" PROPERTIES
                            COMPILE_DEFINITIONS "main=psk8_kernels_main"
                            COMPILE_OPTIONS "-include;${CMAKE_CURRENT_LIST_DIR}/oracles.h")
set_source_files_properties("../../qpsk_axes.c" PROPERTIES
                            COMPILE_OPTIONS "-include;${CMAKE_CURRENT_LIST_DIR}/oracles.h")
set_source_files_properties("../../shaper_axes.c" PROPERTIES
                            COMPILE_DEFINITIONS "app_main=shaper_axes_main")
