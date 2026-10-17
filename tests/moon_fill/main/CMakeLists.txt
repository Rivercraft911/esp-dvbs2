get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
idf_component_register(SRCS "${ROOT}/tests/moon_fill.c"
                           "${ROOT}/firmware/main/shaper_moon.c"
                           "${ROOT}/firmware/main/shaper_moon_pie.S"
                       INCLUDE_DIRS "${ROOT}/firmware/main")
