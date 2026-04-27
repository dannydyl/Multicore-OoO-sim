cmake_minimum_required(VERSION 3.20)

project(multicore_ooo_sim
    VERSION 0.0.1
    DESCRIPTION "Multi-core out-of-order cache-coherent CMP simulator"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel)
endif()

option(CASIM_FAST        "Enable -O2"                       OFF)
option(CASIM_FASTER      "Enable -O3"                       OFF)
option(CASIM_ASAN        "Enable AddressSanitizer"          OFF)
option(CASIM_PROFILE     "Enable gprof (-pg)"               OFF)
option(CASIM_BUILD_TESTS "Build unit tests"                 ON)
option(CASIM_WERROR      "Treat warnings as errors"         OFF)

add_library(casim_warnings INTERFACE)
target_compile_options(casim_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow
    $<$<BOOL:${CASIM_WERROR}>:-Werror>
)

add_library(casim_options INTERFACE)
target_compile_options(casim_options INTERFACE
    $<$<BOOL:${CASIM_FAST}>:-O2>
    $<$<BOOL:${CASIM_FASTER}>:-O3>
    $<$<BOOL:${CASIM_ASAN}>:-fsanitize=address;-fno-omit-frame-pointer;-g>
    $<$<BOOL:${CASIM_PROFILE}>:-pg>
)
target_link_options(casim_options INTERFACE
    $<$<BOOL:${CASIM_ASAN}>:-fsanitize=address>
    $<$<BOOL:${CASIM_PROFILE}>:-pg>
)

include(cmake/Dependencies.cmake)

add_subdirectory(src/common)
add_subdirectory(src/cache)
add_subdirectory(src/predictor)
add_subdirectory(src/ooo)
add_subdirectory(src/coherence)
add_subdirectory(src/core)
add_subdirectory(src)
add_subdirectory(tools)

if(CASIM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

message(STATUS "multicore_ooo_sim ${PROJECT_VERSION}")
message(STATUS "  build type:    ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ standard:  ${CMAKE_CXX_STANDARD}")
message(STATUS "  compiler:      ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  FAST=${CASIM_FAST} FASTER=${CASIM_FASTER} ASAN=${CASIM_ASAN} PROFILE=${CASIM_PROFILE} WERROR=${CASIM_WERROR}")
