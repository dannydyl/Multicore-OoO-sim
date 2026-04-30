add_library(casim_coherence STATIC
    agent.cpp
    agent_mesi.cpp
    agent_mi.cpp
    agent_moesif.cpp
    agent_mosi.cpp
    agent_msi.cpp
    coherence_adapter.cpp
    coherence_cache.cpp
    coherence_mode.cpp
    coherence_stats.cpp
    directory.cpp
    directory_mesi.cpp
    directory_mi.cpp
    directory_moesif.cpp
    directory_mosi.cpp
    directory_msi.cpp
    factory.cpp
    fici_cpu.cpp
    message.cpp
    network.cpp
    node.cpp
    settings.cpp
)

target_include_directories(casim_coherence
    PUBLIC ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(casim_coherence
    PUBLIC  casim_common casim_cache
    PRIVATE casim_warnings casim_options
)
