cmake_minimum_required(VERSION 3.10)

project(drop LANGUAGES C)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_compile_definitions("_GNU_SOURCE")
endif()

add_subdirectory(lib)
add_subdirectory(src)

