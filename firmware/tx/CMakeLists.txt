cmake_minimum_required(VERSION 3.22)
# Components shared with the receiver (dvbs2) and the transmitter's own (the rest).
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components/dvbs2"
                         "${CMAKE_CURRENT_LIST_DIR}/../components/shaper"
                         "${CMAKE_CURRENT_LIST_DIR}/../components/radio_s3"
                         "${CMAKE_CURRENT_LIST_DIR}/../components/station_id"
                         "${CMAKE_CURRENT_LIST_DIR}/../components/ingress")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Build only the components main asks for.
idf_build_set_property(MINIMAL_BUILD ON)
# Stamp the commit hash, not the nearest tag, so the board's `info` names the checkout.
# Track HEAD and its ref even though PROJECT_VER overrides IDF's git describe.
get_git_head_revision(git_ref git_hash "${CMAKE_CURRENT_LIST_DIR}")
execute_process(COMMAND git rev-parse --short HEAD WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
                OUTPUT_VARIABLE PROJECT_VER OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND git diff --quiet HEAD WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
                RESULT_VARIABLE dirty ERROR_QUIET)
if(dirty)
  set(PROJECT_VER "${PROJECT_VER}-dirty")
endif()
project(esp_dvbs2)
