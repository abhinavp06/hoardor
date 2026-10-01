cmake_minimum_required(VERSION 3.23)

project(
    hoardor
    VERSION 0.1.0
    LANGUAGES C CXX
)

# -----------------------------------------------------------------------------
# Global Settings
# -----------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(HOARDOR_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/output/")

# Third-party code is downloaded at configure time into the build directory.
include(FetchContent)

# -----------------------------------------------------------------------------
# Build Options
# -----------------------------------------------------------------------------

option(BUILD_PLAYGROUND "Build playground executable" ON)
option(BUILD_TESTS "Build unit tests" ON)
option(RUN_TESTS_AFTER_BUILD "Run unit tests after a successful build" ON)
option(BUILD_BENCHMARKS "Build Google Benchmark targets (use a Release build)" OFF)

# -----------------------------------------------------------------------------
# Library
# -----------------------------------------------------------------------------

add_library(hoardor
    # FILE ENGINE
    src/file/settings.cpp
    src/file/text.cpp
    src/file/scanner.cpp
)

target_include_directories(hoardor
    PUBLIC
        include
    PRIVATE
        src
)

if(MSVC)
    target_compile_options(hoardor PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(hoardor PRIVATE -Wall -Wextra -Wpedantic)
endif()

# -----------------------------------------------------------------------------
# Playground
# -----------------------------------------------------------------------------

if(BUILD_PLAYGROUND)
    add_subdirectory(playground)
endif()

# -----------------------------------------------------------------------------
# Unit Tests
# -----------------------------------------------------------------------------

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# -----------------------------------------------------------------------------
# Benchmarks
# -----------------------------------------------------------------------------

if(BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
