#include "file/platform/file_info.hpp"

#include <sys/stat.h>

#include <cerrno>

namespace hoardor::file::detail {

std::expected<FileInfo, std::error_code> file_info(const std::filesystem::directory_entry& entry) {
    struct stat st {};
    if (::stat(entry.path().c_str(), &st) != 0) {
        // generic_category, so callers can compare with std::errc values.
        return std::unexpected(std::error_code(errno, std::generic_category()));
    }
#ifdef __APPLE__
    const auto& mtime = st.st_mtimespec;
#else
    const auto& mtime = st.st_mtim;
#endif
    return FileInfo{static_cast<std::uint64_t>(st.st_size),
                    static_cast<std::int64_t>(mtime.tv_sec) * 1'000'000'000 + static_cast<std::int64_t>(mtime.tv_nsec)};
}

}
