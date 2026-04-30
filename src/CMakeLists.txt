add_executable(sim
    main.cpp
)

target_link_libraries(sim
    PRIVATE
        casim_common
        casim_cache
        casim_predictor
        casim_ooo
        casim_coherence
        casim_full
        casim_core
        casim_warnings
        casim_options
)
