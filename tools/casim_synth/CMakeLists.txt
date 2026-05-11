add_library(casim_synth STATIC
    casim_synth.cpp
)

target_include_directories(casim_synth
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(casim_synth
    PUBLIC  casim_common
    PRIVATE casim_warnings casim_options
)

# Example programs that produce real .casim traces + manifests. Each
# emits its output into <build>/tools/casim_synth/<program_name>/ when
# run, so they can be picked up by tests or hand-driven runs of `sim`.
add_executable(synth_lock_chain    programs/lock_chain.cpp)
add_executable(synth_lock_chain_mem programs/lock_chain_memheavy.cpp)
add_executable(synth_dot_product   programs/dot_product.cpp)
add_executable(synth_prodcon       programs/producer_consumer.cpp)

foreach(prog synth_lock_chain synth_lock_chain_mem synth_dot_product synth_prodcon)
    target_link_libraries(${prog}
        PRIVATE
            casim_synth
            casim_warnings
            casim_options
    )
endforeach()
