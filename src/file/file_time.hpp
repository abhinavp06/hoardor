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

inline std::filesystem::file_time_type from_unix_ns(std::int64_t ns) {
    const std::chrono::sys_time<std::chrono::nanoseconds> system{std::chrono::nanoseconds{ns}};
    return std::chrono::clock_cast<std::chrono::file_clock>(system);
}

}
