add_library(casim_ooo STATIC
    dummy.cpp
)

target_include_directories(casim_ooo
    PUBLIC ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(casim_ooo
    PRIVATE casim_warnings casim_options
)
