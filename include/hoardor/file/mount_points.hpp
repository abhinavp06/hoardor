#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace hoardor::file {

// The OS's mounted volumes (Linux: /proc/self/mountinfo, Windows: drive letters).
// Used to find a root whose drive came back under another letter or mount point.
// Returns nothing on systems without an implementation yet (macOS: phase 4).
std::vector<std::filesystem::path> list_mount_points();

// Anything that lists mount points. Tests pass plain folders to simulate drives.
using MountPointLister = std::function<std::vector<std::filesystem::path>()>;

}

namespace hoardor::file {

// Which physical device holds `path`: a name equal for every folder on the same disk (two
// partitions of one HDD give the same name) and different across disks. Sync and metadata
// reading run one worker per device: two readers on one spinning disk thrash its head
// (ARCHITECTURE §6). Touches the drive (a stat). "" if it can't be told.
// Linux: the parent block device from /sys/dev/block; Windows: the disk number behind the
// volume; elsewhere: the file system's device id.
std::string device_of(const std::filesystem::path& path);

// Anything that names a path's device. Tests use it to put folders on separate "drives".
using DeviceLookup = std::function<std::string(const std::filesystem::path&)>;

}
