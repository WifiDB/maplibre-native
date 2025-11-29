cmake_minimum_required(VERSION 3.10)
project(zlib)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_subdirectory(src EXCLUDE_FROM_ALL)

set_target_properties(zlibstatic PROPERTIES POSITION_INDEPENDENT_CODE ON)
