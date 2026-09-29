#pragma once

#include <string>
#include <cstdint>
#include <filesystem>
#include <vector>

enum class MediaType { Music, Podcast, Movie, Show,  };

enum class FileType { Audio, Video, Text };

struct FileEntry {
   std::filesystem::path relative_path;
   std::uintmax_t size = 0;
   std::filesystem::file_time_type mtime{};
};

struct File {
    public:
        std::string id;
        FileType file_type;
};

std::vector<FileEntry> discover(const std::filesystem::path& root, MediaType media_type); // ask user to explicitly state the video/audio directories
