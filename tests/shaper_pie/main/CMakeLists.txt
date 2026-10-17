get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
idf_component_register(SRCS "${ROOT}/tests/shaper_pie.c" "${ROOT}/tests/qpsk_fill.c"
                           "${ROOT}/firmware/main/shaper_qpsk.c"
                           "${ROOT}/firmware/main/shaper_qpsk_pie.S" "${ROOT}/tests/tx_samples.c"
                           "${ROOT}/firmware/main/shaper.c"
                           "${ROOT}/firmware/main/shaper_pie.S"
                           "${ROOT}/firmware/main/ring.c"
                           "${ROOT}/firmware/components/dvbs2/pie.S"
                       INCLUDE_DIRS "${ROOT}/firmware/main")
