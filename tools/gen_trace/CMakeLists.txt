add_library(casim_gen_trace STATIC
    gen_trace.cpp
)

target_include_directories(casim_gen_trace
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(casim_gen_trace
    PUBLIC  casim_common
    PRIVATE casim_warnings casim_options
)

add_executable(gen_trace
    main.cpp
)

target_link_libraries(gen_trace
    PRIVATE
        casim_gen_trace
        CLI11::CLI11
        casim_warnings
        casim_options
)
