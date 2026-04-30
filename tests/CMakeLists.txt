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

add_executable(test_cache
    cache/test_cache_basic.cpp
    cache/test_coherence_sink.cpp
    cache/test_replacement.cpp
    cache/test_l1_l2.cpp
    cache/test_l2_wbwa.cpp
    cache/test_mshr.cpp
    cache/test_prefetcher_plus_one.cpp
    cache/test_prefetcher_markov.cpp
    cache/test_prefetcher_hybrid.cpp
    cache/test_proj1_regression.cpp
)

target_link_libraries(test_cache
    PRIVATE
        casim_cache
        Catch2::Catch2WithMain
        casim_warnings
        casim_options
)

target_compile_definitions(test_cache
    PRIVATE
        PROJ1_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/cache/fixtures/proj1"
)

catch_discover_tests(test_cache)

add_executable(test_predictor
    predictor/test_predictors.cpp
    predictor/test_proj2_regression.cpp
)

target_link_libraries(test_predictor
    PRIVATE
        casim_predictor
        Catch2::Catch2WithMain
        casim_warnings
        casim_options
)

target_compile_definitions(test_predictor
    PRIVATE
        PROJ2_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/predictor/fixtures/proj2"
)

catch_discover_tests(test_predictor)

add_executable(test_ooo
    ooo/test_ooo_basic.cpp
)

target_link_libraries(test_ooo
    PRIVATE
        casim_ooo
        Catch2::Catch2WithMain
        casim_warnings
        casim_options
)

catch_discover_tests(test_ooo)

add_executable(test_proj2_to_champsim
    tools/test_proj2_to_champsim.cpp
)

target_link_libraries(test_proj2_to_champsim
    PRIVATE
        casim_proj2_to_champsim
        Catch2::Catch2WithMain
        casim_warnings
        casim_options
)

catch_discover_tests(test_proj2_to_champsim)

add_executable(test_coherence
    coherence/test_coherence_stats.cpp
    coherence/test_fici_cpu.cpp
    coherence/test_mi.cpp
    coherence/test_network_basic.cpp
    coherence/test_proj3_regression.cpp
    coherence/test_writeback.cpp
)

target_link_libraries(test_coherence
    PRIVATE
        casim_coherence
        Catch2::Catch2WithMain
        casim_warnings
        casim_options
)

target_compile_definitions(test_coherence
    PRIVATE
        PROJ3_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/coherence/fixtures/proj3"
)

catch_discover_tests(test_coherence)

add_executable(test_full
    full/test_cli_dispatch.cpp
    full/test_full_mode.cpp
)

target_link_libraries(test_full
    PRIVATE
        casim_full
        Catch2::Catch2WithMain
        casim_warnings
        casim_options
)

target_compile_definitions(test_full
    PRIVATE
        BASELINE_CONFIG_PATH="${CMAKE_SOURCE_DIR}/configs/baseline.json"
)

catch_discover_tests(test_full)
