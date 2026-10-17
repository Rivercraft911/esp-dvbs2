get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
idf_component_register(SRCS "${ROOT}/tests/shaper_pie.c" "${ROOT}/tests/tx_samples.c"
                           "${ROOT}/firmware/main/shaper.c"
                           "${ROOT}/firmware/main/shaper_pie.S"
                       INCLUDE_DIRS "${ROOT}/firmware/main")
