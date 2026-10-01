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

add_subdirectory(third_party)

add_library(hoardor
    # DB ENGINE
    src/db/database.cpp

    # FILE ENGINE
    src/file/settings.cpp
    src/file/text.cpp
    src/file/scanner.cpp
    src/file/library.cpp
    src/file/library_sync.cpp
    src/file/root_marker.cpp
)

# One mount-point backend per OS (the only platform-specific file so far).
if(WIN32)
    target_sources(hoardor PRIVATE src/file/platform/mount_points_windows.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(hoardor PRIVATE src/file/platform/mount_points_linux.cpp)
else()
    target_sources(hoardor PRIVATE src/file/platform/mount_points_other.cpp)
endif()

target_include_directories(hoardor
    PUBLIC
        include
    PRIVATE
        src
)

target_link_libraries(hoardor PRIVATE hoardor_sqlite3)

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
