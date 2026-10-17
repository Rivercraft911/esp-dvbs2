get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
idf_component_register(SRCS "${ROOT}/tests/radio_qemu.c" PRIV_REQUIRES radio_s3)
