add_compile_options("-fno-strict-aliasing")

add_executable(dropd)
target_sources(dropd PRIVATE daemon.c)
target_link_libraries(dropd PRIVATE libdrop)

add_executable(drop)
target_sources(drop PRIVATE client.c)
target_link_libraries(drop PRIVATE libdrop)

install(TARGETS dropd drop RUNTIME DESTINATION "bin")
