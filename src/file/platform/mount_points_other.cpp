#include <hoardor/file/mount_points.hpp>

namespace hoardor::file {

// No implementation for this OS yet (macOS arrives in file engine phase 4).
// Automatic relocation is unavailable; everything else works.
std::vector<std::filesystem::path> list_mount_points() { return {}; }

}
