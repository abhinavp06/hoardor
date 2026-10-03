cmake_minimum_required(VERSION 3.23)

project(
    hoardor
    VERSION 0.2.0
    LANGUAGES C CXX
)

# -----------------------------------------------------------------------------
# Global Settings
# -----------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(HOARDOR_OUTPUT_DIR "${PROJECT_SOURCE_DIR}/output/")

# Third-party code is downloaded at configure time into the build directory.
include(FetchContent)

# -----------------------------------------------------------------------------
# Build Options
# -----------------------------------------------------------------------------

# PROJECT_IS_TOP_LEVEL is true only when hoardor itself is being built. When an app
# (e.g. TYLI) adds hoardor with add_subdirectory, these default to OFF, so only the
# library is built there. Any of them can still be turned on explicitly.
option(BUILD_PLAYGROUND "Build playground executable" ${PROJECT_IS_TOP_LEVEL})
option(BUILD_TESTS "Build unit tests" ${PROJECT_IS_TOP_LEVEL})
option(RUN_TESTS_AFTER_BUILD "Run unit tests after a successful build" ${PROJECT_IS_TOP_LEVEL})
option(BUILD_BENCHMARKS "Build Google Benchmark targets (use a Release build)" OFF)

# -----------------------------------------------------------------------------
# Library
# -----------------------------------------------------------------------------

add_subdirectory(third_party)

add_library(hoardor
    # CORE (shared infrastructure, built on demand)
    src/core/text.cpp

    # DB ENGINE
    src/db/database.cpp

    # FILE ENGINE
    src/file/settings.cpp
    src/file/text.cpp
    src/file/scanner.cpp
    src/file/library.cpp
    src/file/library_sync.cpp
    src/file/root_marker.cpp

    # MEDIA (internal: the ffmpeg layer and the query builder shared by audio and video)
    src/media/ffmpeg.cpp
    src/media/query.cpp

    # AUDIO ENGINE
    src/audio/read.cpp
    src/audio/library.cpp

    # VIDEO ENGINE
    src/video/sources.cpp
    src/video/read.cpp
    src/video/library.cpp
    src/video/frame.cpp
    src/video/plex.cpp

    # PLAYER ENGINE (libmpv)
    src/player/library.cpp
    src/player/player.cpp

    # MASTER ENGINE
    src/master/sync_worker.cpp
    src/master/playback.cpp
)

# Platform backends (ARCHITECTURE §5): one implementation per OS, chosen here, never by #ifdef in shared code.
if(WIN32)
    target_sources(hoardor PRIVATE
        src/file/platform/device_windows.cpp
        src/file/platform/file_info_windows.cpp
        src/file/platform/mount_points_windows.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(hoardor PRIVATE
        src/file/platform/device_linux.cpp
        src/file/platform/file_info_posix.cpp
        src/file/platform/mount_points_linux.cpp)
else()
    target_sources(hoardor PRIVATE
        src/file/platform/device_other.cpp
        src/file/platform/file_info_posix.cpp
        src/file/platform/mount_points_other.cpp)
endif()

target_include_directories(hoardor
    PUBLIC
        include
    PRIVATE
        src
)

find_package(Threads REQUIRED)
target_link_libraries(hoardor PRIVATE hoardor_sqlite3 hoardor_pugixml hoardor_ffmpeg hoardor_mpv PUBLIC Threads::Threads)

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
