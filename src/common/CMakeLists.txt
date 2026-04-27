add_library(casim_common STATIC
    log.cpp
)

target_include_directories(casim_common
    PUBLIC ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(casim_common
    PUBLIC  nlohmann_json::nlohmann_json
    PRIVATE casim_warnings casim_options
)
