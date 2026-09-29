cmake_minimum_required(VERSION 3.23)

project(
    hoardor
    VERSION 0.1.0
    LANGUAGES CXX
)

# -----------------------------------------------------------------------------
# Global Settings
# -----------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(HOARDOR_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/output/")

# -----------------------------------------------------------------------------
# Build Options
# -----------------------------------------------------------------------------

option(BUILD_PLAYGROUND "Build playground executable" ON)
option(BUILD_TESTS "Build unit tests" ON)
option(RUN_TESTS_AFTER_BUILD "Run unit tests after a successful build" ON)

# -----------------------------------------------------------------------------
# Library
# -----------------------------------------------------------------------------

add_library (hoardor
    # FILE ENGINE
    src/file/file_engine.cpp
)

target_include_directories(hoardor
    PUBLIC
        include

    PRIVATE
        tools
)
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
