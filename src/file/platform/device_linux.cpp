#include <hoardor/file/mount_points.hpp>

#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>

#include <system_error>

namespace hoardor::file {

namespace {

std::filesystem::path sysfs_block(unsigned major, unsigned minor) {
    return std::filesystem::path("/sys/dev/block") / (std::to_string(major) + ":" + std::to_string(minor));
}

}

// Linux: /sys/dev/block/MAJ:MIN is a link into the device tree; a partition (it has a
// "partition" file) lives inside its disk's folder, so the parent folder names the disk.
// (macOS and others use device_other.cpp.)
std::string device_of(const std::filesystem::path& path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) return {};
    const unsigned maj = major(st.st_dev), min = minor(st.st_dev);
    std::error_code ec;
    const auto link = sysfs_block(maj, min);
    if (maj != 0 && std::filesystem::exists(link, ec)) {
        auto device = std::filesystem::canonical(link, ec);
        if (!ec) {
            if (std::filesystem::exists(device / "partition", ec)) device = device.parent_path();
            return "block:" + device.filename().string();
        }
    }
    // Network shares, FUSE, and systems without /sys: the device id itself.
    return "dev:" + std::to_string(static_cast<unsigned long long>(st.st_dev));
}

}
