# SQLite amalgamation (one C file), built as a static library on every OS.
FetchContent_Declare(
    sqlite3
    URL https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip
    URL_HASH SHA3_256=af6aae8d3eccc608857c63cf56efbadc70da48b5c719446b353ed88dded1e288
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(sqlite3)

add_library(hoardor_sqlite3 STATIC ${sqlite3_SOURCE_DIR}/sqlite3.c)
target_include_directories(hoardor_sqlite3 PUBLIC ${sqlite3_SOURCE_DIR})
target_compile_definitions(hoardor_sqlite3 PRIVATE
    SQLITE_DQS=0
    SQLITE_DEFAULT_MEMSTATUS=0
    SQLITE_OMIT_LOAD_EXTENSION
)
find_package(Threads REQUIRED)
target_link_libraries(hoardor_sqlite3 PUBLIC Threads::Threads)
set_target_properties(hoardor_sqlite3 PROPERTIES POSITION_INDEPENDENT_CODE ON)
