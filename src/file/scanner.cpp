#include <hoardor/file/scanner.hpp>

#include "file/file_time.hpp"
#include "file/text.hpp"

namespace hoardor::file {

namespace fs = std::filesystem;

std::expected<Scanner, std::error_code> Scanner::open(const fs::path& root, const Settings& settings) {
    std::error_code ec;
    const fs::file_status status = fs::status(root, ec);
    if (status.type() == fs::file_type::not_found) return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory));
    if (ec) return std::unexpected(ec);
    if (!fs::is_directory(status)) return std::unexpected(std::make_error_code(std::errc::not_a_directory));

    fs::directory_iterator it(root, ec);
    if (ec) return std::unexpected(ec);

    Scanner scanner;
    scanner.root_ = root;
    // Prepared once, so per-file lookups are a single hash probe on lowercase keys.
    for (const auto& [extension, kind] : settings.extension_kinds) {
        const std::string key = detail::normalize_extension(extension);
        if (!key.empty()) scanner.kinds_.emplace(key, kind);
    }
    for (const auto& name : settings.ignored_names) scanner.ignored_names_.insert(detail::ascii_lower(name));
    for (const auto& prefix : settings.ignored_prefixes) {
        if (!prefix.empty()) scanner.ignored_prefixes_.push_back(detail::ascii_lower(prefix));
    }
    scanner.stack_.push_back(Level{std::move(it), {}});
    scanner.progress_.directories_visited = 1;
    return scanner;
}

bool Scanner::is_ignored(const std::string& lowered_name) const {
    if (ignored_names_.contains(lowered_name)) return true;
    for (const auto& prefix : ignored_prefixes_) {
        if (lowered_name.starts_with(prefix)) return true;
    }
    return false;
}

bool Scanner::root_reachable() const {
    // Opening the root forces a real read; a cached stat of a dead mount could still succeed.
    std::error_code ec;
    fs::directory_iterator probe(root_, ec);
    return !ec;
}

std::optional<ScanResult> Scanner::on_error(const fs::path& relative, std::error_code code, bool is_directory) {
    const bool root_lost = relative.empty() || !root_reachable();
    if (root_lost) {
        // One error instead of thousands when a drive disappears.
        ++progress_.errors;
        finished_ = true;
        stack_.clear();
        return ScanResult{std::unexpect, ScanError{{}, code, true}};
    }
    // Something deleted while we were scanning is simply gone, not an error.
    if (code == std::errc::no_such_file_or_directory) return std::nullopt;
    ++progress_.errors;
    return ScanResult{std::unexpect, ScanError{detail::to_utf8(relative).text, code, is_directory}};
}

std::optional<ScanResult> Scanner::next() {
    while (!finished_ && !stack_.empty()) {
        Level& level = stack_.back();
        std::error_code ec;

        if (level.consumed) {
            level.consumed = false;
            level.it.increment(ec);
            if (ec) {
                // The rest of this directory can't be listed, so its subtree is unknown.
                const fs::path relative = std::move(level.relative);
                stack_.pop_back();
                if (auto result = on_error(relative, ec, true)) return result;
                continue;
            }
        }
        if (level.it == fs::directory_iterator{}) {
            stack_.pop_back();
            continue;
        }
        level.consumed = true;

        const fs::directory_entry& entry = *level.it;
        const fs::path name = entry.path().filename();
        fs::path relative = level.relative / name;
        const detail::Utf8 utf8_name = detail::to_utf8(name);
        if (is_ignored(detail::ascii_lower(utf8_name.text))) continue;

        // symlink_status: a symlink is reported as a symlink, never as its target.
        const fs::file_type type = entry.symlink_status(ec).type();
        if (ec) {
            if (auto result = on_error(relative, ec, false)) return result;
            continue;
        }

        if (type == fs::file_type::directory) {
            if (!utf8_name.valid) {
                if (auto result = on_error(relative, std::make_error_code(std::errc::illegal_byte_sequence), true)) return result;
                continue;
            }
            fs::directory_iterator child(entry.path(), ec);
            if (ec) {
                if (auto result = on_error(relative, ec, true)) return result;
                continue;
            }
            ++progress_.directories_visited;
            // `level` and `entry` are invalidated by push_back; they're not used after this.
            stack_.push_back(Level{std::move(child), std::move(relative)});
            continue;
        }
        // Symlinks, Windows junctions, FIFOs, sockets, and devices are never media.
        if (type != fs::file_type::regular) continue;

        const auto kind = kinds_.find(detail::normalize_extension(detail::to_utf8(name.extension()).text));
        if (kind == kinds_.end()) continue;

        if (!utf8_name.valid) {
            if (auto result = on_error(relative, std::make_error_code(std::errc::illegal_byte_sequence), false)) return result;
            continue;
        }

        const std::uintmax_t size = entry.file_size(ec);
        if (ec) {
            if (auto result = on_error(relative, ec, false)) return result;
            continue;
        }
        const fs::file_time_type mtime = entry.last_write_time(ec);
        if (ec) {
            if (auto result = on_error(relative, ec, false)) return result;
            continue;
        }

        ++progress_.files_emitted;
        return ScanResult{ScannedFile{detail::to_utf8(relative).text, size, detail::to_unix_ns(mtime), kind->second}};
    }
    finished_ = true;
    stack_.clear();
    return std::nullopt;
}

}
