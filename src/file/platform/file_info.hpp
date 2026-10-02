#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <system_error>

namespace hoardor::file::detail {

struct FileInfo {
    std::uint64_t size = 0;
    std::int64_t mtime_ns = 0;  // Unix nanoseconds
};

// Size and mtime of a regular file in one query (the scanner's hot path):
// POSIX does a single stat(), Windows uses what the directory listing already cached.
// libstdc++'s file_size() + last_write_time() would walk the full path twice.
std::expected<FileInfo, std::error_code> file_info(const std::filesystem::directory_entry& entry);

}
