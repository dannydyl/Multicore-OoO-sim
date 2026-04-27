add_library(casim_cache STATIC
    cache.cpp
    cache_mode.cpp
    main_memory.cpp
    prefetcher_hybrid.cpp
    prefetcher_markov.cpp
    prefetcher_plus_one.cpp
    replacement.cpp
    write_policy.cpp
)

target_include_directories(casim_cache
    PUBLIC ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(casim_cache
    PUBLIC  casim_common
    PRIVATE casim_warnings casim_options
)
