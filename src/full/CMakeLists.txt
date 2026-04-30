add_library(casim_full STATIC
    full_mode.cpp
)

target_include_directories(casim_full
    PUBLIC ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(casim_full
    PUBLIC  casim_common casim_cache casim_predictor casim_ooo casim_coherence
    PRIVATE casim_warnings casim_options
)
