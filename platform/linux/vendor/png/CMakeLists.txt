cmake_minimum_required(VERSION 3.10)
project(png)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_subdirectory(src EXCLUDE_FROM_ALL)

# Ensure PIC is enabled
set_target_properties(png_static PROPERTIES POSITION_INDEPENDENT_CODE ON)