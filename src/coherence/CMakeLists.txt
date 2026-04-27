add_library(casim_coherence STATIC
    dummy.cpp
)

target_include_directories(casim_coherence
    PUBLIC ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(casim_coherence
    PRIVATE casim_warnings casim_options
)
