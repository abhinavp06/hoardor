FetchContent_Declare(
    googlebenchmark
    URL https://github.com/google/benchmark/archive/refs/tags/v1.9.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googlebenchmark)

add_executable(hoardor_benchmarks
    file/scanner_benchmark.cpp
    file/sync_benchmark.cpp
    audio/query_benchmark.cpp
)
target_link_libraries(hoardor_benchmarks PRIVATE hoardor benchmark::benchmark_main)
target_include_directories(hoardor_benchmarks PRIVATE ${PROJECT_SOURCE_DIR}/tests ${CMAKE_CURRENT_SOURCE_DIR})
