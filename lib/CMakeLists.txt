add_library(libdrop STATIC)
target_include_directories(libdrop PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_sources(libdrop PRIVATE options.c tftp.c)
set_target_properties(libdrop PROPERTIES TARGET_PREFIX "")

