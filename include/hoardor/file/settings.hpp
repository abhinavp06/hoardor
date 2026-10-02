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
    Info = 6,  // sidecar descriptions (.nfo): companions of a video, read for its metadata
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

    // ---- Sync ----

    // master queues a global sync in the background when it starts.
    bool sync_on_startup = false;
    // A file whose mtime is within this many seconds of the sync (either direction)
    // is probably still being copied: it's stored as unsettled until a later sync.
    std::int64_t settle_window_seconds = 10;
    // A sync that would remove more than this share of a root's entries holds the
    // removals for confirmation (Library::apply_held_removals) instead of applying them.
    int mass_removal_threshold_percent = 25;
    // A write transaction commits after this many rows or milliseconds, whichever
    // comes first, so other writers (play counts, ratings) never wait long.
    int batch_max_rows = 2000;
    int batch_max_milliseconds = 50;
    // Progress is reported every this many files.
    int progress_interval_files = 500;
    // Manually relocating a root without a marker checks this many known entries,
    // and needs this share of them to exist with the same size.
    int relocation_sample_size = 20;
    int relocation_min_match_percent = 80;

    static Settings defaults();
};

// The allowed range of each numeric setting. One table, used to validate before saving
// and to decide whether a stored value is usable when loading.
struct SettingRange {
    std::int64_t min;
    std::int64_t max;
};

namespace limits {
inline constexpr SettingRange settle_window_seconds{0, 86'400};
inline constexpr SettingRange mass_removal_threshold_percent{0, 100};
inline constexpr SettingRange batch_max_rows{1, 1'000'000};
inline constexpr SettingRange batch_max_milliseconds{1, 60'000};
inline constexpr SettingRange progress_interval_files{1, 1'000'000};
inline constexpr SettingRange relocation_sample_size{0, 10'000};
inline constexpr SettingRange relocation_min_match_percent{0, 100};
}

// Problems with these settings, in plain words (empty when they're valid).
// Library::save_settings refuses settings that have any.
std::vector<std::string> validate(const Settings& settings);

// The kind of a file, by its extension, or std::nullopt when it isn't media.
// Convenient for one-off checks. The scanner uses its own prepared lookup.
std::optional<FileKind> kind_of(const std::filesystem::path& file, const Settings& settings);

}
