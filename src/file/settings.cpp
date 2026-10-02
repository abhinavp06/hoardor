#include <hoardor/file/settings.hpp>

#include "file/text.hpp"

#include <array>
#include <utility>

namespace hoardor::file {

namespace {

constexpr std::array<std::pair<FileKind, std::string_view>, 6> kind_names{{
    {FileKind::Audio, "audio"},
    {FileKind::Video, "video"},
    {FileKind::Text, "text"},
    {FileKind::Image, "image"},
    {FileKind::Subtitle, "subtitle"},
    {FileKind::Info, "info"},
}};

}

std::string_view to_string(FileKind kind) {
    for (const auto& [k, name] : kind_names) {
        if (k == kind) return name;
    }
    return "unknown";
}

std::optional<FileKind> file_kind_from_string(std::string_view name) {
    const std::string lowered = detail::ascii_lower(name);
    for (const auto& [kind, kind_name] : kind_names) {
        if (kind_name == lowered) return kind;
    }
    return std::nullopt;
}

Settings Settings::defaults() {
    Settings settings;

    const auto map = [&](FileKind kind, std::initializer_list<const char*> extensions) {
        for (const char* extension : extensions) settings.extension_kinds.emplace(extension, kind);
    };
    map(FileKind::Audio, {"mp3", "flac", "ogg", "oga", "opus", "m4a", "m4b", "aac", "wav", "aiff", "aif",
                          "wma", "ape", "wv", "dsf", "dff", "alac"});
    map(FileKind::Video, {"mkv", "mp4", "m4v", "avi", "mov", "wmv", "webm", "ts", "m2ts", "mpg", "mpeg", "flv"});
    map(FileKind::Text, {"epub", "pdf", "txt", "md", "mobi", "azw3", "cbz", "cbr", "html", "htm"});
    map(FileKind::Image, {"jpg", "jpeg", "png", "webp", "gif", "bmp"});
    map(FileKind::Subtitle, {"srt", "ass", "ssa", "vtt", "sub", "idx"});
    map(FileKind::Info, {"nfo"});

    settings.ignored_names = {
        // Windows
        "$RECYCLE.BIN", "System Volume Information", "Thumbs.db", "desktop.ini", "FOUND.000",
        // macOS
        ".DS_Store", ".Trashes", ".Spotlight-V100", ".fseventsd", ".TemporaryItems", ".DocumentRevisions-V100",
        // Linux
        "lost+found",
        // NAS (Synology and others)
        "@eaDir", "#recycle", "#snapshot", ".snapshot",
        // hoardor's own root marker
        ".hoardor-root",
    };
    settings.ignored_prefixes = {
        "._",      // macOS AppleDouble files on FAT, exFAT, and SMB
        ".Trash-", // Linux per-user trash on removable drives
    };
    return settings;
}

std::vector<std::string> validate(const Settings& s) {
    std::vector<std::string> problems;
    const auto range = [&](std::string_view name, std::int64_t value, SettingRange r) {
        if (value < r.min || value > r.max) {
            problems.push_back(std::string(name) + " must be between " + std::to_string(r.min) + " and " +
                               std::to_string(r.max) + " (got " + std::to_string(value) + ")");
        }
    };
    range("settle window (seconds)", s.settle_window_seconds, limits::settle_window_seconds);
    range("mass-removal threshold (%)", s.mass_removal_threshold_percent, limits::mass_removal_threshold_percent);
    range("batch size (rows)", s.batch_max_rows, limits::batch_max_rows);
    range("batch time (ms)", s.batch_max_milliseconds, limits::batch_max_milliseconds);
    range("progress interval (files)", s.progress_interval_files, limits::progress_interval_files);
    range("relocation sample size", s.relocation_sample_size, limits::relocation_sample_size);
    range("relocation match (%)", s.relocation_min_match_percent, limits::relocation_min_match_percent);

    for (const auto& [extension, kind] : s.extension_kinds) {
        const std::string normalized = detail::normalize_extension(extension);
        if (normalized.empty()) problems.push_back("an extension is empty");
        // Settings are stored one "ext=kind" per line, so these characters can't be part of a value.
        if (normalized.find_first_of("=\n\r") != std::string::npos) {
            problems.push_back("the extension '" + extension + "' contains '=' or a line break");
        }
        if (to_string(kind) == "unknown") problems.push_back("the extension '" + extension + "' has an unknown kind");
    }
    const auto lines = [&](std::string_view what, const std::vector<std::string>& values) {
        for (const auto& value : values) {
            if (value.empty()) problems.push_back(std::string(what) + " contains an empty entry");
            if (value.find_first_of("\n\r") != std::string::npos) {
                problems.push_back(std::string(what) + " entry '" + value + "' contains a line break");
            }
        }
    };
    lines("ignored names", s.ignored_names);
    lines("ignored prefixes", s.ignored_prefixes);
    return problems;
}

std::optional<FileKind> kind_of(const std::filesystem::path& file, const Settings& settings) {
    const std::string extension = detail::normalize_extension(detail::to_utf8(file.extension()).text);
    if (extension.empty()) return std::nullopt;
    for (const auto& [key, kind] : settings.extension_kinds) {
        if (detail::normalize_extension(key) == extension) return kind;
    }
    return std::nullopt;
}

}
