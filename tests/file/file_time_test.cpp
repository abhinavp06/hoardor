#include "file/file_time.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using hoardor::file::detail::from_unix_ns;
using hoardor::file::detail::to_unix_ns;

namespace {

// The file clock's tick in nanoseconds: 1 with libstdc++, 100 on MSVC.
std::int64_t tick_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(fs::file_time_type::duration{1}).count();
}

}  // namespace

TEST(FileTime, RoundTripIsExactOnTheClocksTick) {
    // Multiples of 100 ns survive on every platform, before and after 1970.
    for (std::int64_t ns : {std::int64_t{0}, std::int64_t{1'700'000'000'123'456'700}, std::int64_t{-1'000'000'000},
                            std::int64_t{946'684'800'000'000'000}}) {
        EXPECT_EQ(to_unix_ns(from_unix_ns(ns)), ns) << ns;
    }
}

TEST(FileTime, FinerThanTheTickRoundsDown) {
    // 150 ns: kept on Linux, 100 ns on Windows. Never rounded up, never off by a tick.
    for (std::int64_t ns : {std::int64_t{150}, std::int64_t{1'700'000'000'000'000'099}, std::int64_t{-150}}) {
        const std::int64_t back = to_unix_ns(from_unix_ns(ns));
        EXPECT_LE(back, ns) << ns;
        EXPECT_LT(ns - back, tick_ns()) << ns;
    }
}

TEST(FileTime, AFilesModificationTimeComesBackUnchanged) {
    const fs::path file = fs::temp_directory_path() / "hoardor_file_time_test.bin";
    std::ofstream(file) << "x";
    const std::int64_t ns = 1'600'000'000'123'456'700;  // 2020, a multiple of 100 ns
    fs::last_write_time(file, from_unix_ns(ns));
    EXPECT_EQ(to_unix_ns(fs::last_write_time(file)), ns);
    fs::remove(file);
}
