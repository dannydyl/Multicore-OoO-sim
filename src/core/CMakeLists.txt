add_library(casim_core STATIC
    dummy.cpp
)

target_include_directories(casim_core
    PUBLIC ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(casim_core
    PRIVATE casim_warnings casim_options
)
