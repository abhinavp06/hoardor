#pragma once

#include "support/temp_dir.hpp"

#include <cstdlib>
#include <string>

namespace hoardor::bench {

// How many files the generated library has: HOARDOR_BENCH_FILES (default 50,000).
// Example: HOARDOR_BENCH_FILES=500000 ./hoardor_benchmarks
inline int file_count() {
    static const int count = [] {
        const char* env = std::getenv("HOARDOR_BENCH_FILES");
        const int n = env ? std::atoi(env) : 0;
        return n > 0 ? n : 50'000;
    }();
    return count;
}

// <temp>/Music/Artist N/Album M/Track K.flac: folders of 100 empty files, 10 albums per artist.
// Empty files cost the same as real ones here: scanning and syncing only stat them.
// Built once per process and shared by every benchmark.
inline const test::TempDir& library_tree() {
    static const test::TempDir dir;
    static const bool built = [] {
        const int files = file_count();
        for (int i = 0; i < files; ++i) {
            const int album = i / 100;
            dir.write(std::filesystem::path("Music") / ("Artist " + std::to_string(album / 10)) /
                      ("Album " + std::to_string(album)) / ("Track " + std::to_string(i % 100) + ".flac"));
        }
        return true;
    }();
    (void)built;
    return dir;
}

}
