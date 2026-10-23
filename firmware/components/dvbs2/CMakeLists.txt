set(srcs "dvbs2.c" "fast.c")
if(IDF_TARGET STREQUAL "esp32s3")
    list(APPEND srcs "pie.S" "qpsk_signs.S" "ts_pie.S")
endif()
idf_component_register(SRCS ${srcs} INCLUDE_DIRS "include")
# The plain oracle is never used by the live transmitter. Keep it in flash.
set_source_files_properties(fast.c PROPERTIES COMPILE_DEFINITIONS DVBS2_IRAM=1)
# Scheduling before register allocation hoists table loads and spills them on Xtensa's
# 16-register window (bch_c: 126 instead of 51 loads and stores per loop).
set_source_files_properties(fast.c PROPERTIES COMPILE_OPTIONS "-fno-schedule-insns")
if(IDF_TARGET STREQUAL "esp32s3")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DVBS2_PIE=1)
endif()
