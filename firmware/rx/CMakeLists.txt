cmake_minimum_required(VERSION 3.22)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components/dvbs2rx"
                         "${CMAKE_CURRENT_LIST_DIR}/../components/dvbs2")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Build only the components main asks for.
idf_build_set_property(MINIMAL_BUILD ON)
# Stamp the commit hash so the board's `info` names the checkout.
get_git_head_revision(git_ref git_hash "${CMAKE_CURRENT_LIST_DIR}")
execute_process(COMMAND git rev-parse --short HEAD WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
                OUTPUT_VARIABLE PROJECT_VER OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND git diff --quiet HEAD WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
                RESULT_VARIABLE dirty ERROR_QUIET)
if(dirty)
  set(PROJECT_VER "${PROJECT_VER}-dirty")
endif()
project(esp_dvbs2_rx)
