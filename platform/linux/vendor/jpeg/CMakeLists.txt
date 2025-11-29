cmake_minimum_required(VERSION 3.10)
project(jpeg)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_subdirectory(jpeg-9e EXCLUDE_FROM_ALL)

set_target_properties(jpeg PROPERTIES POSITION_INDEPENDENT_CODE ON)
