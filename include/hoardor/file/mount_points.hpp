#pragma once

#include <filesystem>
#include <functional>
#include <vector>

namespace hoardor::file {

// The OS's mounted volumes (Linux: /proc/self/mountinfo, Windows: drive letters).
// Used to find a root whose drive came back under another letter or mount point.
// Returns nothing on systems without an implementation yet (macOS: phase 4).
std::vector<std::filesystem::path> list_mount_points();

// Anything that lists mount points. Tests pass plain folders to simulate drives.
using MountPointLister = std::function<std::vector<std::filesystem::path>()>;

}
