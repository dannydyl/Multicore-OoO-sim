include(Catch)

add_executable(test_config
    common/test_config.cpp
)

target_link_libraries(test_config
    PRIVATE
        casim_common
        Catch2::Catch2WithMain
        casim_warnings
        casim_options
)

target_compile_definitions(test_config
    PRIVATE
        BASELINE_CONFIG_PATH="${CMAKE_SOURCE_DIR}/configs/baseline.json"
)

catch_discover_tests(test_config)

add_executable(test_trace
    common/test_trace.cpp
)

target_link_libraries(test_trace
    PRIVATE
        casim_common
        Catch2::Catch2WithMain
        casim_warnings
        casim_options
)

catch_discover_tests(test_trace)

add_executable(test_gen_trace
    tools/test_gen_trace.cpp
)

target_link_libraries(test_gen_trace
    PRIVATE
        casim_gen_trace
        Catch2::Catch2WithMain
        casim_warnings
        casim_options
)

catch_discover_tests(test_gen_trace)
