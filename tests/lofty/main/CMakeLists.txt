get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
idf_component_register(SRCS "${ROOT}/tests/lofty.c"
                           "${ROOT}/tests/sign_mapper.c"
                           "${ROOT}/firmware/main/machine.c"
                           "${ROOT}/firmware/main/machine_pie.S"
                           "${ROOT}/firmware/main/lofty.c"
                           "${ROOT}/firmware/main/lofty_pie.S"
                       INCLUDE_DIRS "${ROOT}/firmware/main"
                       PRIV_REQUIRES dvbs2)
if(LOFTY_TRACE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LOFTY_TRACE=1)
endif()
