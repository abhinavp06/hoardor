#pragma once

#include <hoardor/file/settings.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hoardor::file {

struct ScannedFile {
    std::string relative_path;  // UTF-8, '/' separators, relative to the root, never normalized
    std::uint64_t size = 0;     // bytes
    std::int64_t mtime_ns = 0;  // nanoseconds since the Unix epoch (UTC)
    FileKind kind{};
};

// A path that could not be read.
// - is_directory = true: the whole subtree is unknown. Sync must NOT treat its files as deleted.
// - relative_path empty: the root itself was lost (e.g. the drive was unplugged). This is
//   always the last item of the scan.
struct ScanError {
    std::string relative_path;  // UTF-8, '/' separators (invalid names are repaired with U+FFFD)
    std::error_code code;
    bool is_directory = false;
};

using ScanResult = std::expected<ScannedFile, ScanError>;

struct ScanProgress {
    std::uint64_t directories_visited = 0;
    std::uint64_t files_emitted = 0;
    std::uint64_t errors = 0;
};

// Streams the media files under one root, one at a time. Memory grows with the
// depth of the tree, never with the number of files. Symlinks are not followed.
class Scanner {
public:
    // Fails if root does not exist, is not a directory, or cannot be opened.
    static std::expected<Scanner, std::error_code> open(const std::filesystem::path& root, const Settings& settings);

    // The next media file or error, or std::nullopt when the scan is complete.
    // To cancel, stop calling next() and destroy the scanner.
    std::optional<ScanResult> next();

    // Cheap counters for progress reporting. A total isn't known up front.
    ScanProgress progress() const { return progress_; }

private:
    struct Level {
        std::filesystem::directory_iterator it;
        std::filesystem::path relative;  // empty for the root
        bool consumed = false;           // the current entry was handled; advance before reading
    };

    Scanner() = default;

    bool is_ignored(const std::string& lowered_name) const;
    bool root_reachable() const;
    // Every read error goes through here: it decides between a local error and "root lost".
    std::optional<ScanResult> on_error(const std::filesystem::path& relative, std::error_code code, bool is_directory);

    std::filesystem::path root_;
    std::unordered_map<std::string, FileKind> kinds_;  // normalized extension -> kind
    std::unordered_set<std::string> ignored_names_;    // lowercased
    std::vector<std::string> ignored_prefixes_;        // lowercased
    std::vector<Level> stack_;
    ScanProgress progress_;
    bool finished_ = false;
};

}
