idf_component_register(SRCS "dvbs2.c" INCLUDE_DIRS "include")
target_compile_definitions(${COMPONENT_LIB} PRIVATE DVBS2_IRAM=1)
