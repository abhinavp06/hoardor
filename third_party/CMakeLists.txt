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

# pugixml (MIT): reads .nfo sidecar files (XML). Built from its one source file, like SQLite.
# SOURCE_SUBDIR points at a folder without a CMakeLists.txt, so only the download happens.
FetchContent_Declare(
    pugixml
    URL https://github.com/zeux/pugixml/releases/download/v1.14/pugixml-1.14.tar.gz
    URL_HASH SHA256=2f10e276870c64b1db6809050a75e11a897a8d7456c4be5c6b2e35a11168a015
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR do-not-add
)
FetchContent_MakeAvailable(pugixml)

add_library(hoardor_pugixml STATIC ${pugixml_SOURCE_DIR}/src/pugixml.cpp)
target_include_directories(hoardor_pugixml PUBLIC ${pugixml_SOURCE_DIR}/src)
set_target_properties(hoardor_pugixml PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ffmpeg (LGPL, shared libraries): reads tags and stream information (features/media_listing.md).
# Linux and macOS: the system's development packages, found with pkg-config
#   (Ubuntu: libavformat-dev libavcodec-dev libavutil-dev; macOS: brew install ffmpeg).
# Windows: a shared build (e.g. BtbN's "lgpl-shared"), pointed to with -DFFMPEG_ROOT=<folder with include/ lib/ bin/>.
set(FFMPEG_ROOT "" CACHE PATH "ffmpeg folder with include/, lib/ and bin/ (Windows)")
add_library(hoardor_ffmpeg INTERFACE)
if(FFMPEG_ROOT)
    find_path(FFMPEG_INCLUDE_DIR libavformat/avformat.h HINTS ${FFMPEG_ROOT}/include REQUIRED)
    foreach(lib avformat avcodec avutil)
        find_library(FFMPEG_${lib}_LIBRARY ${lib} HINTS ${FFMPEG_ROOT}/lib REQUIRED)
        target_link_libraries(hoardor_ffmpeg INTERFACE ${FFMPEG_${lib}_LIBRARY})
    endforeach()
    # SYSTEM: warnings inside ffmpeg's headers are not ours (MSVC /external, GCC -isystem).
    target_include_directories(hoardor_ffmpeg SYSTEM INTERFACE ${FFMPEG_INCLUDE_DIR})
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat>=60 libavcodec>=60 libavutil>=58)
    target_link_libraries(hoardor_ffmpeg INTERFACE PkgConfig::FFMPEG)
endif()
