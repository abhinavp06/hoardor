FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
# On Windows, link GoogleTest against the same C runtime as the tests (MSVC).
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(hoardor_tests
    db/database_test.cpp
    file/settings_test.cpp
    file/text_test.cpp
    file/scanner_test.cpp
    file/library_test.cpp
    file/sync_test.cpp
)

target_link_libraries(hoardor_tests PRIVATE hoardor GTest::gtest_main)
# Tests may include internal headers from src/ to test private helpers directly.
target_include_directories(hoardor_tests PRIVATE ${PROJECT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})

include(GoogleTest)
gtest_discover_tests(hoardor_tests DISCOVERY_TIMEOUT 60)

if(RUN_TESTS_AFTER_BUILD)
    # Part of the default build: runs the whole suite once hoardor_tests is (re)built.
    add_custom_target(run_tests ALL
        COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR} --output-on-failure
        DEPENDS hoardor_tests
        COMMENT "Running hoardor tests"
        VERBATIM
    )
endif()
