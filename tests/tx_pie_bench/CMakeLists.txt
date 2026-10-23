cmake_minimum_required(VERSION 3.22)
# The transmitter's PIE kernels on the board against their references (tests/tx_pie_bench.c).
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../firmware/components/dvbs2"
                         "${CMAKE_CURRENT_LIST_DIR}/../../firmware/components/ingress")
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Report the commit, as the transmitter does.
execute_process(COMMAND git rev-parse --short HEAD WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
                OUTPUT_VARIABLE PROJECT_VER OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND git diff --quiet HEAD WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
                RESULT_VARIABLE dirty ERROR_QUIET)
if(dirty)
  set(PROJECT_VER "${PROJECT_VER}-dirty")
endif()
project(tx_pie_bench)
