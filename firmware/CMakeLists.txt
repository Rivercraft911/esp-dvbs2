cmake_minimum_required(VERSION 3.22)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Build only the components main asks for.
idf_build_set_property(MINIMAL_BUILD ON)
project(esp_dvbs2)
