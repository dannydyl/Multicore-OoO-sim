add_library(casim_cache STATIC
    dummy.cpp
)

target_include_directories(casim_cache
    PUBLIC ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(casim_cache
    PRIVATE casim_warnings casim_options
)
