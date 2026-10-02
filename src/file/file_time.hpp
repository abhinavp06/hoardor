#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>

namespace hoardor::file::detail {

// file_time_type has a platform-specific epoch (1601 on Windows, 2174 in libstdc++),
// so every mtime goes through these two functions and is stored as Unix nanoseconds.
// The conversion is exact: the same file always yields the same number.
inline std::int64_t to_unix_ns(std::filesystem::file_time_type time) {
    const auto system = std::chrono::clock_cast<std::chrono::system_clock>(time);
    return std::chrono::duration_cast<std::chrono::nanoseconds>(system.time_since_epoch()).count();
}

// file_time_type's tick is 1 ns in libstdc++ but 100 ns on MSVC, where clock_cast keeps
// the nanoseconds and won't narrow implicitly: floor to the clock's own tick (a no-op
// on Linux; on Windows it drops the digits NTFS can't store anyway).
inline std::filesystem::file_time_type from_unix_ns(std::int64_t ns) {
    const std::chrono::sys_time<std::chrono::nanoseconds> system{std::chrono::nanoseconds{ns}};
    return std::chrono::floor<std::filesystem::file_time_type::duration>(
        std::chrono::clock_cast<std::chrono::file_clock>(system));
}

}
