#include "file/root_marker.hpp"

#include <array>
#include <fstream>
#include <random>

namespace hoardor::file::detail {

namespace {

constexpr std::string_view header = "hoardor-root";

bool looks_like_uuid(std::string_view text) {
    if (text.size() != 36) return false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        const char c = text[i];
        if (dash ? c != '-' : !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

}

std::string new_uuid() {
    std::random_device device;
    std::array<unsigned char, 16> bytes{};
    for (auto& b : bytes) b = static_cast<unsigned char>(device());
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0F) | 0x40);  // version 4
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3F) | 0x80);  // RFC 4122 variant
    constexpr char hex[] = "0123456789abcdef";
    std::string out;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out.push_back('-');
        out.push_back(hex[bytes[i] >> 4]);
        out.push_back(hex[bytes[i] & 0x0F]);
    }
    return out;
}

std::optional<std::string> read_marker(const std::filesystem::path& directory) {
    std::ifstream in(directory / marker_file_name);
    if (!in) return std::nullopt;
    std::string line;
    if (!std::getline(in, line) || line != header) return std::nullopt;
    while (std::getline(in, line)) {
        if (line.starts_with("uuid=")) {
            std::string uuid = line.substr(5);
            if (looks_like_uuid(uuid)) return uuid;
            return std::nullopt;
        }
    }
    return std::nullopt;
}

bool write_marker(const std::filesystem::path& directory, std::string_view uuid) {
    {
        std::ofstream out(directory / marker_file_name, std::ios::trunc);
        if (!out) return false;
        out << header << "\nversion=1\nuuid=" << uuid << "\n";
        out.flush();
        if (!out) return false;
    }
    return read_marker(directory) == uuid;
}

}
