set(srcs "kernels.c" "demod.c" "rx_fec.c")
if(IDF_TARGET STREQUAL "esp32s3")
    list(APPEND srcs "kernels_pie.S" "rx_fec_pie.S")
endif()
idf_component_register(SRCS ${srcs} INCLUDE_DIRS "include" "."
                       LDFRAGMENTS "linker.lf"
                       PRIV_REQUIRES esp_hw_support dvbs2)
if(IDF_TARGET STREQUAL "esp32s3")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DVBS2RX_PIE=1)
endif()
