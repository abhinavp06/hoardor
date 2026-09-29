#pragma once

#include <filesystem>
#include <cstdint>
#include <vector>

struct FileEntry {
   std::filesystem::path relative_path;
   std::uintmax_t size = 0;
   std::filesystem::file_time_type mtime{};
};

enum class MediaType { Audio, Video };

std::vector<FileEntry> discover(const std::filesystem::path& root, MediaType media_type); // ask user to explicitly state the video/audio directories
