#include "file/platform/file_info.hpp"

#include "file/file_time.hpp"

namespace hoardor::file::detail {

std::expected<FileInfo, std::error_code> file_info(const std::filesystem::directory_entry& entry) {
    // MSVC's directory_entry caches size and mtime from FindNextFileW: no system call here.
    std::error_code ec;
    const auto size = entry.file_size(ec);
    if (ec) return std::unexpected(ec);
    const auto mtime = entry.last_write_time(ec);
    if (ec) return std::unexpected(ec);
    return FileInfo{static_cast<std::uint64_t>(size), to_unix_ns(mtime)};
}

}
