add_library(casim_ooo STATIC
    inst.cpp
    rat.cpp
    rob.cpp
    schedq.cpp
    core.cpp
    ooo_mode.cpp
)

target_include_directories(casim_ooo
    PUBLIC ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(casim_ooo
    PUBLIC  casim_common
            casim_cache
            casim_predictor
    PRIVATE casim_warnings casim_options
)
