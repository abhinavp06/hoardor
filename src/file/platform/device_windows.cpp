#include <hoardor/file/mount_points.hpp>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>

#include <string>

namespace hoardor::file {

// The disk number behind the volume that holds `path` (two partitions of one disk share
// it), else the volume's serial number (network shares, unusual volumes).
std::string device_of(const std::filesystem::path& path) {
    wchar_t volume_path[MAX_PATH + 1] = {};
    if (!GetVolumePathNameW(path.c_str(), volume_path, MAX_PATH)) return {};
    std::wstring root(volume_path);                  // "E:\" or a mounted folder "C:\mnt\x\"
    wchar_t volume_name[MAX_PATH + 1] = {};          // "\\?\Volume{GUID}\"
    if (GetVolumeNameForVolumeMountPointW(root.c_str(), volume_name, MAX_PATH)) {
        std::wstring device(volume_name);
        if (!device.empty() && device.back() == L'\\') device.pop_back();  // open the volume, not its root folder
        HANDLE handle = CreateFileW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            VOLUME_DISK_EXTENTS extents{};
            DWORD returned = 0;
            const BOOL ok = DeviceIoControl(handle, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, &extents,
                                            sizeof extents, &returned, nullptr);
            CloseHandle(handle);
            if (ok && extents.NumberOfDiskExtents >= 1) return "disk:" + std::to_string(extents.Extents[0].DiskNumber);
        }
    }
    DWORD serial = 0;
    if (GetVolumeInformationW(root.c_str(), nullptr, 0, &serial, nullptr, nullptr, nullptr, 0)) {
        return "volume:" + std::to_string(serial);
    }
    return {};
}

}
