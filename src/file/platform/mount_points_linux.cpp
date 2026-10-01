#include <hoardor/file/mount_points.hpp>

#include <fstream>
#include <sstream>
#include <string>

namespace hoardor::file {

namespace {

// mountinfo escapes space, tab, newline, and backslash as \040, \011, \012, \134.
std::string unescape(const std::string& field) {
    std::string out;
    for (std::size_t i = 0; i < field.size(); ++i) {
        if (field[i] == '\\' && i + 3 < field.size()) {
            const std::string octal = field.substr(i + 1, 3);
            if (octal.find_first_not_of("01234567") == std::string::npos) {
                out.push_back(static_cast<char>(std::stoi(octal, nullptr, 8)));
                i += 3;
                continue;
            }
        }
        out.push_back(field[i]);
    }
    return out;
}

}

std::vector<std::filesystem::path> list_mount_points() {
    std::vector<std::filesystem::path> mounts;
    std::ifstream in("/proc/self/mountinfo");
    std::string line;
    while (std::getline(in, line)) {
        // Fields: id parent major:minor root mount-point options ...
        std::istringstream fields(line);
        std::string skip, mount_point;
        if (fields >> skip >> skip >> skip >> skip >> mount_point) mounts.emplace_back(unescape(mount_point));
    }
    return mounts;
}

}
