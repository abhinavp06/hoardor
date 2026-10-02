#include <hoardor/file/mount_points.hpp>

#include <sys/stat.h>

namespace hoardor::file {

// macOS and others: the file system's device id. Partitions of one disk get different ids
// here (a known limitation until a DiskArbitration backend exists, file engine phase 4).
std::string device_of(const std::filesystem::path& path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) return {};
    return "dev:" + std::to_string(static_cast<unsigned long long>(st.st_dev));
}

}
