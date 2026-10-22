set(srcs "kernels.c" "demod.c")
if(IDF_TARGET STREQUAL "esp32s3")
    list(APPEND srcs "kernels_pie.S")
endif()
idf_component_register(SRCS ${srcs} INCLUDE_DIRS "include" "."
                       LDFRAGMENTS "linker.lf"
                       PRIV_REQUIRES esp_hw_support)
if(IDF_TARGET STREQUAL "esp32s3")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DVBS2RX_PIE=1)
endif()
