cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../firmware/components/dvbs2rx"
                         "${CMAKE_CURRENT_LIST_DIR}/../../firmware/components/dvbs2")
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rx_fec_bench)
