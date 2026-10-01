#include <hoardor/file/settings.hpp>

#include "file/text.hpp"

#include <array>
#include <utility>

namespace hoardor::file {

namespace {

constexpr std::array<std::pair<FileKind, std::string_view>, 5> kind_names{{
    {FileKind::Audio, "audio"},
    {FileKind::Video, "video"},
    {FileKind::Text, "text"},
    {FileKind::Image, "image"},
    {FileKind::Subtitle, "subtitle"},
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

std::optional<FileKind> kind_of(const std::filesystem::path& file, const Settings& settings) {
    const std::string extension = detail::normalize_extension(detail::to_utf8(file.extension()).text);
    if (extension.empty()) return std::nullopt;
    for (const auto& [key, kind] : settings.extension_kinds) {
        if (detail::normalize_extension(key) == extension) return kind;
    }
    return std::nullopt;
}

}
