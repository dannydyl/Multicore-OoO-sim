add_library(casim_cache STATIC
    cache.cpp
    main_memory.cpp
    replacement.cpp
    write_policy.cpp
)

target_include_directories(casim_cache
    PUBLIC ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(casim_cache
    PRIVATE casim_warnings casim_options
)
