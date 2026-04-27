add_library(casim_common STATIC
    cli.cpp
    config.cpp
    log.cpp
)

target_include_directories(casim_common
    PUBLIC ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(casim_common
    PUBLIC  nlohmann_json::nlohmann_json
    PRIVATE CLI11::CLI11 casim_warnings casim_options
)
