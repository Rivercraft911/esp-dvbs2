get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
if(MOON_HANDOFF)
    set(PROBE "${ROOT}/tests/moon_handoff.c")
else()
    set(PROBE "${ROOT}/tests/moon_fill.c")
endif()
idf_component_register(SRCS "${PROBE}"
                           "${ROOT}/firmware/main/shaper_moon.c"
                           "${ROOT}/firmware/main/shaper_moon_pie.S"
                       INCLUDE_DIRS "${ROOT}/firmware/main")
