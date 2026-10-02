#include <hoardor/file/mount_points.hpp>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <string>

namespace hoardor::file {

std::vector<std::filesystem::path> list_mount_points() {
    // "C:\\\0E:\\\0\0": a list of drive roots, each NUL-terminated, ending with an empty string.
    std::wstring buffer(512, L'\0');
    const DWORD length = GetLogicalDriveStringsW(static_cast<DWORD>(buffer.size()), buffer.data());
    std::vector<std::filesystem::path> mounts;
    if (length == 0 || length > buffer.size()) return mounts;
    for (const wchar_t* drive = buffer.data(); *drive != L'\0'; drive += wcslen(drive) + 1) {
        mounts.emplace_back(drive);
    }
    return mounts;
}

}
