#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>
#include <system_error>

namespace hoardor::test {

// A unique directory under the system temp folder, removed (best effort) on destruction.
class TempDir {
public:
    TempDir() {
        std::random_device rd;
        const auto id = (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
        path_ = std::filesystem::temp_directory_path() / ("hoardor-test-" + std::to_string(id));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

    // Creates parent directories as needed and writes `size` bytes.
    std::filesystem::path write(const std::filesystem::path& relative, std::size_t size = 0) const {
        const auto full = path_ / relative;
        std::filesystem::create_directories(full.parent_path());
        std::ofstream out(full, std::ios::binary);
        const std::string data(size, 'x');
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        return full;
    }

private:
    std::filesystem::path path_;
};

// A path from UTF-8 text, on every OS (std::filesystem::path(std::string) uses the ANSI code page on Windows).
inline std::filesystem::path u8path(std::string_view utf8) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

}
