#pragma once
#include <string>

enum class FileType { Audio, Video, Text };

struct File {
    public:
        std::string id;
        FileType file_type;
};
