#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace hoardor::file {

// The values are stored in the database, so they must never be renumbered.
enum class FileKind : std::uint8_t {
    Audio = 1,
    Video = 2,
    Text = 3,
    Image = 4,
    Subtitle = 5,
};

std::string_view to_string(FileKind kind);
// Accepts the names produced by to_string ("audio", "video", ...), ignoring case.
std::optional<FileKind> file_kind_from_string(std::string_view name);

// Every tunable of the file engine, as plain data. The defaults live in code
// (defaults()). The database stores the user's values, and a value missing
// from the database falls back to the default.
struct Settings {
    // ---- Scanning ----

    // Extension -> kind. Keys may use any case, with or without a leading dot.
    // Files with an unmapped extension are not media and are skipped.
    std::unordered_map<std::string, FileKind> extension_kinds;
    // Exact file or directory names to skip, matched ignoring (ASCII) case.
    // A skipped directory is not descended into.
    std::vector<std::string> ignored_names;
    // Name prefixes to skip, matched ignoring (ASCII) case.
    std::vector<std::string> ignored_prefixes;

    static Settings defaults();
};

// The kind of a file, by its extension, or std::nullopt when it isn't media.
// Convenient for one-off checks. The scanner uses its own prepared lookup.
std::optional<FileKind> kind_of(const std::filesystem::path& file, const Settings& settings);

}
