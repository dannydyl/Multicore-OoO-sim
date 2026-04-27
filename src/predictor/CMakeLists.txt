add_library(casim_predictor STATIC
    dummy.cpp
)

target_include_directories(casim_predictor
    PUBLIC ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(casim_predictor
    PRIVATE casim_warnings casim_options
)
