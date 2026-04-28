add_library(casim_predictor STATIC
    always_taken.cpp
    factory.cpp
    hybrid.cpp
    perceptron.cpp
    predictor_mode.cpp
    yeh_patt.cpp
)

target_include_directories(casim_predictor
    PUBLIC ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(casim_predictor
    PUBLIC  casim_common
    PRIVATE casim_warnings casim_options
)
