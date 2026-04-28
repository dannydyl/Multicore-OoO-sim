add_library(casim_proj2_to_champsim STATIC
    proj2_to_champsim.cpp
)

target_include_directories(casim_proj2_to_champsim
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(casim_proj2_to_champsim
    PUBLIC  casim_common
    PRIVATE casim_warnings casim_options
)

add_executable(proj2_to_champsim
    main.cpp
)

target_link_libraries(proj2_to_champsim
    PRIVATE
        casim_proj2_to_champsim
        CLI11::CLI11
        casim_warnings
        casim_options
)
