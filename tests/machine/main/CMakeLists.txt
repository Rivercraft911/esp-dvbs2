get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
idf_component_register(SRCS "${ROOT}/tests/machine.c"
                           "${ROOT}/firmware/main/machine.c"
                           "${ROOT}/firmware/main/machine_pie.S"
                       INCLUDE_DIRS "${ROOT}/firmware/main")
if(MACHINE_TRACE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MACHINE_TRACE=1)
endif()
