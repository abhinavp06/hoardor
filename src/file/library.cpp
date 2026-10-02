#include <hoardor/file/library.hpp>

#include "file/library_internal.hpp"
#include "file/root_marker.hpp"
#include "file/text.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <map>

namespace hoardor::file {

namespace fs = std::filesystem;

namespace detail {

Error database_error(const db::Error& error) { return Error{ErrorCode::Database, error.message}; }

bool is_constraint(const db::Error& error) { return (error.code & 0xFF) == 19; }  // SQLITE_CONSTRAINT

std::string path_key(std::string_view relative_path, bool case_sensitive) {
    return case_sensitive ? std::string(relative_path) : ascii_lower(relative_path);
}

std::string_view root_columns() {
    return "id, uuid, category_id, name, path, path_in_volume, use_marker, case_sensitive, status, generation, "
           "last_sync_ns, file_count, held_removals";
}

Root read_root(const db::Statement& st) {
    Root r;
    r.id = st.column_int64(0);
    r.uuid = st.column_text(1);
    r.category_id = st.column_int64(2);
    r.name = st.column_text(3);
    r.path = st.column_text(4);
    r.path_in_volume = st.column_text(5);
    r.use_marker = st.column_int64(6) != 0;
    r.case_sensitive = st.column_int64(7) != 0;
    r.status = static_cast<RootStatus>(st.column_int64(8));
    r.generation = st.column_int64(9);
    r.last_sync_ns = st.column_int64(10);
    r.file_count = static_cast<std::uint64_t>(st.column_int64(11));
    r.held_removals = static_cast<std::uint64_t>(st.column_int64(12));
    return r;
}

std::string_view entry_columns() {
    return "id, root_id, relative_path, size, mtime_ns, kind, unsettled, changed_generation, added_ns";
}

Entry read_entry(const db::Statement& st) {
    Entry e;
    e.id = st.column_int64(0);
    e.root_id = st.column_int64(1);
    e.relative_path = st.column_text(2);
    e.size = static_cast<std::uint64_t>(st.column_int64(3));
    e.mtime_ns = st.column_int64(4);
    e.kind = static_cast<FileKind>(st.column_int64(5));
    e.unsettled = st.column_int64(6) != 0;
    e.changed_generation = st.column_int64(7);
    e.added_ns = st.column_int64(8);
    return e;
}

Result<void> set_root_status(db::Database& db, RootId id, RootStatus status) {
    auto st = db.prepare("UPDATE file_roots SET status = ? WHERE id = ?");
    if (!st) return std::unexpected(database_error(st.error()));
    st->bind(1, static_cast<std::int64_t>(status)).bind(2, id);
    if (auto r = st->run(); !r) return std::unexpected(database_error(r.error()));
    return {};
}

}

using detail::database_error;

namespace {

// ---------------------------------------------------------------- Schema

constexpr std::string_view schema_v1 = R"sql(
CREATE TABLE file_settings (key TEXT PRIMARY KEY, value TEXT NOT NULL);
CREATE TABLE file_categories (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL UNIQUE COLLATE NOCASE,
    kinds TEXT NOT NULL
);
CREATE TABLE file_roots (
    id INTEGER PRIMARY KEY,
    uuid TEXT NOT NULL UNIQUE,
    category_id INTEGER NOT NULL REFERENCES file_categories(id),
    name TEXT NOT NULL,
    path TEXT NOT NULL,
    path_in_volume TEXT NOT NULL,
    use_marker INTEGER NOT NULL,
    case_sensitive INTEGER NOT NULL,
    status INTEGER NOT NULL DEFAULT 0,
    generation INTEGER NOT NULL DEFAULT 0,
    last_sync_ns INTEGER NOT NULL DEFAULT 0,
    file_count INTEGER NOT NULL DEFAULT 0,
    held_removals INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE file_entries (
    id INTEGER PRIMARY KEY,
    root_id INTEGER NOT NULL REFERENCES file_roots(id) ON DELETE CASCADE,
    relative_path TEXT NOT NULL,
    path_key TEXT NOT NULL,
    size INTEGER NOT NULL,
    mtime_ns INTEGER NOT NULL,
    kind INTEGER NOT NULL,
    unsettled INTEGER NOT NULL DEFAULT 0,
    seen_generation INTEGER NOT NULL,
    changed_generation INTEGER NOT NULL,
    UNIQUE (root_id, path_key)
);
CREATE INDEX file_entries_changed ON file_entries (root_id, changed_generation, id);
CREATE TABLE file_scan_errors (
    root_id INTEGER NOT NULL REFERENCES file_roots(id) ON DELETE CASCADE,
    relative_path TEXT NOT NULL,
    is_directory INTEGER NOT NULL,
    message TEXT NOT NULL,
    generation INTEGER NOT NULL
);
CREATE INDEX file_scan_errors_root ON file_scan_errors (root_id, generation);
INSERT INTO file_categories (name, kinds) VALUES
    ('Music', 'audio,image'),
    ('Movies', 'video,subtitle,image'),
    ('Shows', 'video,subtitle,image'),
    ('Books', 'text,image');
)sql";

// Media library v1 (features/media_listing.md §6): when each entry was first found, and the
// .nfo kind. Entries from before have no real "added" time; their mtime is the closest guess.
constexpr std::string_view schema_v2 = R"sql(
ALTER TABLE file_entries ADD COLUMN added_ns INTEGER NOT NULL DEFAULT 0;
UPDATE file_entries SET added_ns = mtime_ns;
CREATE INDEX file_entries_added ON file_entries (added_ns, id);
UPDATE file_categories SET kinds = kinds || ',info'
    WHERE name IN ('Movies', 'Shows') AND (',' || kinds || ',') NOT LIKE '%,info,%';
UPDATE file_settings SET value = value || char(10) || 'nfo=info'
    WHERE key = 'extension_kinds' AND value <> '' AND (char(10) || value) NOT LIKE '%' || char(10) || 'nfo=%';
)sql";

constexpr std::array<db::Migration, 2> migrations{{{1, schema_v1}, {2, schema_v2}}};

// ---------------------------------------------------------------- Settings (de)serialization

std::string join_lines(const std::vector<std::string>& lines) {
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i) out.push_back('\n');
        out += lines[i];
    }
    return out;
}

std::vector<std::string> split_lines(std::string_view text) {
    std::vector<std::string> out;
    if (text.empty()) return out;
    std::size_t start = 0;
    while (true) {
        const std::size_t end = text.find('\n', start);
        out.emplace_back(text.substr(start, end - start));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return out;
}

template <class Int>
bool parse_int(std::string_view text, Int& out, std::int64_t min, std::int64_t max) {
    Int value{};
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size() || value < min || value > max) return false;
    out = value;
    return true;
}

std::vector<std::pair<std::string, std::string>> serialize(const Settings& s) {
    std::map<std::string, FileKind> sorted;
    for (const auto& [ext, kind] : s.extension_kinds) sorted[detail::normalize_extension(ext)] = kind;
    std::vector<std::string> kinds;
    for (const auto& [ext, kind] : sorted) {
        if (!ext.empty()) kinds.push_back(ext + "=" + std::string(to_string(kind)));
    }
    return {
        {"extension_kinds", join_lines(kinds)},
        {"ignored_names", join_lines(s.ignored_names)},
        {"ignored_prefixes", join_lines(s.ignored_prefixes)},
        {"sync_on_startup", s.sync_on_startup ? "true" : "false"},
        {"settle_window_seconds", std::to_string(s.settle_window_seconds)},
        {"mass_removal_threshold_percent", std::to_string(s.mass_removal_threshold_percent)},
        {"batch_max_rows", std::to_string(s.batch_max_rows)},
        {"batch_max_milliseconds", std::to_string(s.batch_max_milliseconds)},
        {"progress_interval_files", std::to_string(s.progress_interval_files)},
        {"relocation_sample_size", std::to_string(s.relocation_sample_size)},
        {"relocation_min_match_percent", std::to_string(s.relocation_min_match_percent)},
    };
}

// Applies one stored value. A value that doesn't parse leaves the default in place.
void apply(Settings& s, const std::string& key, const std::string& value) {
    if (key == "extension_kinds") {
        std::unordered_map<std::string, FileKind> map;
        for (const auto& line : split_lines(value)) {
            const auto eq = line.find('=');
            if (eq == std::string::npos) return;
            const auto kind = file_kind_from_string(std::string_view(line).substr(eq + 1));
            const std::string ext = detail::normalize_extension(std::string_view(line).substr(0, eq));
            if (!kind || ext.empty()) return;
            map[ext] = *kind;
        }
        s.extension_kinds = std::move(map);
    } else if (key == "ignored_names") {
        s.ignored_names = split_lines(value);
    } else if (key == "ignored_prefixes") {
        s.ignored_prefixes = split_lines(value);
    } else if (key == "sync_on_startup") {
        if (value == "true") s.sync_on_startup = true;
        else if (value == "false") s.sync_on_startup = false;
    } else if (key == "settle_window_seconds") {
        parse_int<std::int64_t>(value, s.settle_window_seconds, limits::settle_window_seconds.min,
                                limits::settle_window_seconds.max);
    } else if (key == "mass_removal_threshold_percent") {
        parse_int<int>(value, s.mass_removal_threshold_percent, limits::mass_removal_threshold_percent.min,
                       limits::mass_removal_threshold_percent.max);
    } else if (key == "batch_max_rows") {
        parse_int<int>(value, s.batch_max_rows, limits::batch_max_rows.min, limits::batch_max_rows.max);
    } else if (key == "batch_max_milliseconds") {
        parse_int<int>(value, s.batch_max_milliseconds, limits::batch_max_milliseconds.min,
                       limits::batch_max_milliseconds.max);
    } else if (key == "progress_interval_files") {
        parse_int<int>(value, s.progress_interval_files, limits::progress_interval_files.min,
                       limits::progress_interval_files.max);
    } else if (key == "relocation_sample_size") {
        parse_int<int>(value, s.relocation_sample_size, limits::relocation_sample_size.min,
                       limits::relocation_sample_size.max);
    } else if (key == "relocation_min_match_percent") {
        parse_int<int>(value, s.relocation_min_match_percent, limits::relocation_min_match_percent.min,
                       limits::relocation_min_match_percent.max);
    }
    // Unknown keys (e.g. written by a newer build) are ignored.
}

// ---------------------------------------------------------------- Categories

std::string kinds_text(const std::vector<FileKind>& kinds) {
    std::string out;
    for (FileKind k : kinds) {
        if (!out.empty()) out.push_back(',');
        out += to_string(k);
    }
    return out;
}

std::vector<FileKind> parse_kinds(std::string_view text) {
    std::vector<FileKind> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = std::min(text.find(',', start), text.size());
        if (auto kind = file_kind_from_string(text.substr(start, end - start))) out.push_back(*kind);
        start = end + 1;
    }
    return out;
}

std::string trimmed(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return std::string(text);
}

// ---------------------------------------------------------------- Roots

// "/a/b" and "/a/b/c" overlap, "/a/b" and "/a/bc" don't. ASCII case is ignored,
// which is conservative on case-sensitive drives.
bool overlaps(std::string_view a, std::string_view b) {
    std::string x = detail::ascii_lower(a), y = detail::ascii_lower(b);
    if (!x.ends_with('/')) x.push_back('/');
    if (!y.ends_with('/')) y.push_back('/');
    return x.starts_with(y) || y.starts_with(x);
}

std::string flip_ascii_case(std::string_view name) {
    std::string out(name);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        else if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

// Read-only detection: look a name up with its ASCII case flipped. Same file -> case-insensitive.
bool detect_case_sensitive(const fs::path& directory) {
    std::vector<fs::path> candidates;
    if (fs::exists(directory / detail::marker_file_name)) candidates.emplace_back(detail::marker_file_name);
    std::error_code ec;
    fs::directory_iterator it(directory, ec);
    for (int i = 0; !ec && it != fs::directory_iterator{} && i < 16; ++i, it.increment(ec)) {
        candidates.push_back(it->path().filename());
    }
    for (const auto& name : candidates) {
        const std::string utf8 = detail::to_utf8(name).text;
        const std::string flipped = flip_ascii_case(utf8);
        if (flipped == utf8) continue;
        const fs::path other = directory / detail::from_utf8(flipped);
        if (!fs::exists(other, ec)) return true;
        return !fs::equivalent(directory / name, other, ec);
    }
    return true;
}

Result<fs::path> normalized_directory(const fs::path& path) {
    std::error_code ec;
    const fs::path absolute = fs::absolute(path, ec);
    if (ec) return std::unexpected(Error{ErrorCode::InvalidArgument, ec.message()});
    fs::path canonical = fs::weakly_canonical(absolute, ec);
    if (ec) return std::unexpected(Error{ErrorCode::NotFound, ec.message()});
    if (!fs::exists(canonical, ec)) return std::unexpected(Error{ErrorCode::NotFound, "folder does not exist"});
    if (!fs::is_directory(canonical, ec)) return std::unexpected(Error{ErrorCode::InvalidArgument, "not a folder"});
    if (!detail::to_utf8(canonical).valid) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "the folder's path is not valid UTF-8"});
    }
    return canonical;
}

}

// ---------------------------------------------------------------- Library

Result<Library> Library::open(db::Database& database, MountPointLister mounts) {
    if (auto r = db::migrate(database, "file", migrations); !r) return std::unexpected(database_error(r.error()));
    return Library(database, std::move(mounts));
}

Result<Settings> Library::load_settings() {
    Settings settings = Settings::defaults();
    auto st = db_->prepare("SELECT key, value FROM file_settings");
    if (!st) return std::unexpected(database_error(st.error()));
    while (true) {
        auto row = st->step();
        if (!row) return std::unexpected(database_error(row.error()));
        if (!*row) break;
        apply(settings, st->column_text(0), st->column_text(1));
    }
    return settings;
}

Result<void> Library::save_settings(const Settings& settings) {
    if (const auto problems = validate(settings); !problems.empty()) {
        std::string message;
        for (const auto& p : problems) message += (message.empty() ? "" : "; ") + p;
        return std::unexpected(Error{ErrorCode::InvalidArgument, message});
    }
    auto tx = db::Transaction::begin(*db_);
    if (!tx) return std::unexpected(database_error(tx.error()));
    auto st = db_->prepare("INSERT INTO file_settings (key, value) VALUES (?, ?) "
                           "ON CONFLICT (key) DO UPDATE SET value = excluded.value");
    if (!st) return std::unexpected(database_error(st.error()));
    for (const auto& [key, value] : serialize(settings)) {
        st->bind(1, std::string_view(key)).bind(2, std::string_view(value));
        if (auto r = st->run(); !r) return std::unexpected(database_error(r.error()));
        st->reset();
    }
    if (auto r = tx->commit(); !r) return std::unexpected(database_error(r.error()));
    return {};
}

Result<std::vector<Category>> Library::categories() {
    auto st = db_->prepare("SELECT id, name, kinds FROM file_categories ORDER BY id");
    if (!st) return std::unexpected(database_error(st.error()));
    std::vector<Category> out;
    while (true) {
        auto row = st->step();
        if (!row) return std::unexpected(database_error(row.error()));
        if (!*row) break;
        out.push_back(Category{st->column_int64(0), st->column_text(1), parse_kinds(st->column_text(2))});
    }
    return out;
}

Result<CategoryId> Library::add_category(std::string_view name, const std::vector<FileKind>& kinds) {
    const std::string clean = trimmed(name);
    if (clean.empty()) return std::unexpected(Error{ErrorCode::InvalidArgument, "a category needs a name"});
    auto st = db_->prepare("INSERT INTO file_categories (name, kinds) VALUES (?, ?)");
    if (!st) return std::unexpected(database_error(st.error()));
    st->bind(1, std::string_view(clean)).bind(2, std::string_view(kinds_text(kinds)));
    if (auto r = st->run(); !r) {
        if (detail::is_constraint(r.error())) {
            return std::unexpected(Error{ErrorCode::AlreadyExists, "a category named '" + clean + "' already exists"});
        }
        return std::unexpected(database_error(r.error()));
    }
    return db_->last_insert_id();
}

Result<void> Library::update_category(const Category& category) {
    const std::string clean = trimmed(category.name);
    if (clean.empty()) return std::unexpected(Error{ErrorCode::InvalidArgument, "a category needs a name"});
    auto st = db_->prepare("UPDATE file_categories SET name = ?, kinds = ? WHERE id = ?");
    if (!st) return std::unexpected(database_error(st.error()));
    st->bind(1, std::string_view(clean)).bind(2, std::string_view(kinds_text(category.kinds))).bind(3, category.id);
    if (auto r = st->run(); !r) {
        if (detail::is_constraint(r.error())) {
            return std::unexpected(Error{ErrorCode::AlreadyExists, "a category named '" + clean + "' already exists"});
        }
        return std::unexpected(database_error(r.error()));
    }
    if (db_->changes() == 0) return std::unexpected(Error{ErrorCode::NotFound, "no such category"});
    return {};
}

Result<void> Library::remove_category(CategoryId id) {
    auto in_use = roots(id);
    if (!in_use) return std::unexpected(in_use.error());
    if (!in_use->empty()) return std::unexpected(Error{ErrorCode::InUse, "the category still has folders"});
    auto st = db_->prepare("DELETE FROM file_categories WHERE id = ?");
    if (!st) return std::unexpected(database_error(st.error()));
    st->bind(1, id);
    if (auto r = st->run(); !r) return std::unexpected(database_error(r.error()));
    if (db_->changes() == 0) return std::unexpected(Error{ErrorCode::NotFound, "no such category"});
    return {};
}

Result<std::vector<Root>> Library::roots(std::optional<CategoryId> category) {
    std::string sql = "SELECT " + std::string(detail::root_columns()) + " FROM file_roots";
    if (category) sql += " WHERE category_id = ?";
    sql += " ORDER BY id";
    auto st = db_->prepare(sql);
    if (!st) return std::unexpected(database_error(st.error()));
    if (category) st->bind(1, *category);
    std::vector<Root> out;
    while (true) {
        auto row = st->step();
        if (!row) return std::unexpected(database_error(row.error()));
        if (!*row) break;
        out.push_back(detail::read_root(*st));
    }
    return out;
}

Result<Root> Library::root(RootId id) {
    auto st = db_->prepare("SELECT " + std::string(detail::root_columns()) + " FROM file_roots WHERE id = ?");
    if (!st) return std::unexpected(database_error(st.error()));
    st->bind(1, id);
    auto row = st->step();
    if (!row) return std::unexpected(database_error(row.error()));
    if (!*row) return std::unexpected(Error{ErrorCode::NotFound, "no such folder"});
    return detail::read_root(*st);
}

std::string Library::path_in_volume(const fs::path& path) const {
    // The longest mount point that contains the path; "" when the path is the mount itself.
    std::size_t best = 0;
    std::string result = detail::to_utf8(path.relative_path()).text;
    for (const fs::path& mount : mounts_ ? mounts_() : std::vector<fs::path>{}) {
        const fs::path rel = path.lexically_relative(mount);
        if (rel.empty() || *rel.begin() == "..") continue;
        const std::size_t length = mount.native().size();
        if (length < best) continue;
        best = length;
        result = rel == "." ? std::string() : detail::to_utf8(rel).text;
    }
    return result;
}

Result<void> Library::check_overlap(const fs::path& path, std::optional<RootId> except) {
    auto existing = roots();
    if (!existing) return std::unexpected(existing.error());
    const std::string utf8 = detail::to_utf8(path).text;
    for (const Root& r : *existing) {
        if (except && r.id == *except) continue;
        if (overlaps(utf8, r.path)) {
            return std::unexpected(Error{ErrorCode::Overlap, "overlaps the folder '" + r.path + "' (" + r.name + ")"});
        }
    }
    return {};
}

Result<Root> Library::add_root(CategoryId category, const fs::path& path, std::string_view name, bool use_marker) {
    auto directory = normalized_directory(path);
    if (!directory) return std::unexpected(directory.error());

    auto categories_now = categories();
    if (!categories_now) return std::unexpected(categories_now.error());
    if (std::none_of(categories_now->begin(), categories_now->end(), [&](const Category& c) { return c.id == category; })) {
        return std::unexpected(Error{ErrorCode::NotFound, "no such category"});
    }
    if (auto r = check_overlap(*directory, std::nullopt); !r) return std::unexpected(r.error());

    std::string uuid = detail::new_uuid();
    if (use_marker) {
        if (auto existing = detail::read_marker(*directory)) {
            auto all = roots();
            if (!all) return std::unexpected(all.error());
            for (const Root& r : *all) {
                if (r.uuid == *existing) {
                    return std::unexpected(Error{ErrorCode::AlreadyExists,
                                                 "this folder is already in the library as '" + r.name + "' (" + r.path + ")"});
                }
            }
            uuid = *existing;  // re-adding a folder (e.g. after a lost database) keeps its identity
        } else if (!detail::write_marker(*directory, uuid)) {
            use_marker = false;  // read-only storage: identity falls back to the path
        }
    }

    const std::string utf8_path = detail::to_utf8(*directory).text;
    std::string clean_name = trimmed(name);
    if (clean_name.empty()) clean_name = detail::to_utf8(directory->filename()).text;
    if (clean_name.empty()) clean_name = utf8_path;

    auto st = db_->prepare("INSERT INTO file_roots (uuid, category_id, name, path, path_in_volume, use_marker, "
                           "case_sensitive, status) VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
    if (!st) return std::unexpected(database_error(st.error()));
    st->bind(1, std::string_view(uuid))
        .bind(2, category)
        .bind(3, std::string_view(clean_name))
        .bind(4, std::string_view(utf8_path))
        .bind(5, std::string_view(path_in_volume(*directory)))
        .bind(6, use_marker ? 1 : 0)
        .bind(7, detect_case_sensitive(*directory) ? 1 : 0)
        .bind(8, static_cast<std::int64_t>(RootStatus::Online));
    if (auto r = st->run(); !r) return std::unexpected(database_error(r.error()));
    return root(db_->last_insert_id());
}

Result<void> Library::remove_root(RootId id) {
    auto st = db_->prepare("DELETE FROM file_roots WHERE id = ?");
    if (!st) return std::unexpected(database_error(st.error()));
    st->bind(1, id);
    if (auto r = st->run(); !r) return std::unexpected(database_error(r.error()));
    if (db_->changes() == 0) return std::unexpected(Error{ErrorCode::NotFound, "no such folder"});
    return {};
}

Result<void> Library::set_root_category(RootId id, CategoryId category) {
    auto st = db_->prepare("UPDATE file_roots SET category_id = ? WHERE id = ?");
    if (!st) return std::unexpected(database_error(st.error()));
    st->bind(1, category).bind(2, id);
    if (auto r = st->run(); !r) {
        if (detail::is_constraint(r.error())) return std::unexpected(Error{ErrorCode::NotFound, "no such category"});
        return std::unexpected(database_error(r.error()));
    }
    if (db_->changes() == 0) return std::unexpected(Error{ErrorCode::NotFound, "no such folder"});
    return {};
}

Result<void> Library::set_root_case_sensitive(RootId id, bool case_sensitive) {
    auto tx = db::Transaction::begin(*db_);
    if (!tx) return std::unexpected(database_error(tx.error()));
    auto st = db_->prepare("UPDATE file_roots SET case_sensitive = ? WHERE id = ?");
    if (!st) return std::unexpected(database_error(st.error()));
    st->bind(1, case_sensitive ? 1 : 0).bind(2, id);
    if (auto r = st->run(); !r) return std::unexpected(database_error(r.error()));
    if (db_->changes() == 0) return std::unexpected(Error{ErrorCode::NotFound, "no such folder"});
    // SQLite's lower() folds ASCII only, exactly like detail::ascii_lower.
    auto keys = db_->prepare("UPDATE file_entries SET path_key = CASE WHEN ? THEN relative_path ELSE lower(relative_path) END "
                             "WHERE root_id = ?");
    if (!keys) return std::unexpected(database_error(keys.error()));
    keys->bind(1, case_sensitive ? 1 : 0).bind(2, id);
    if (auto r = keys->run(); !r) {
        if (detail::is_constraint(r.error())) {
            return std::unexpected(Error{ErrorCode::InvalidArgument,
                                         "the folder has names that differ only in case, so it can't be case-insensitive"});
        }
        return std::unexpected(database_error(r.error()));
    }
    if (auto r = tx->commit(); !r) return std::unexpected(database_error(r.error()));
    return {};
}

Result<Library::Resolution> Library::resolve_root(Root& root) {
    const fs::path last_known = detail::from_utf8(root.path);
    if (!root.use_marker) {
        std::error_code ec;
        if (fs::is_directory(last_known, ec)) return Resolution{true, false, {}};
        return Resolution{false, false, "the folder is not available (drive unplugged or folder moved)"};
    }

    const auto marker = detail::read_marker(last_known);
    if (marker == root.uuid) return Resolution{true, false, {}};

    // The marker is missing, unreadable, or belongs to another root: look for ours on every mounted volume.
    if (mounts_) {
        for (const fs::path& mount : mounts_()) {
            const fs::path candidate = root.path_in_volume.empty() ? mount : mount / detail::from_utf8(root.path_in_volume);
            if (candidate == last_known) continue;
            if (detail::read_marker(candidate) != root.uuid) continue;
            const std::string new_path = detail::to_utf8(candidate.lexically_normal()).text;
            auto st = db_->prepare("UPDATE file_roots SET path = ? WHERE id = ?");
            if (!st) return std::unexpected(database_error(st.error()));
            st->bind(1, std::string_view(new_path)).bind(2, root.id);
            if (auto r = st->run(); !r) return std::unexpected(database_error(r.error()));
            root.path = new_path;
            return Resolution{true, true, {}};
        }
    }
    if (marker) return Resolution{false, false, "a different library folder is now at this location"};
    return Resolution{false, false, "the folder is not available (drive unplugged or folder moved)"};
}

Result<Root> Library::relocate_root(RootId id, const fs::path& new_path) {
    auto current = root(id);
    if (!current) return std::unexpected(current.error());
    auto directory = normalized_directory(new_path);
    if (!directory) return std::unexpected(directory.error());
    if (auto r = check_overlap(*directory, id); !r) return std::unexpected(r.error());

    const auto marker = detail::read_marker(*directory);
    if (marker && *marker != current->uuid) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "this folder belongs to a different library folder"});
    }
    if (!marker) {
        // No marker to prove identity: check a sample of known files instead.
        auto settings = load_settings();
        if (!settings) return std::unexpected(settings.error());
        auto st = db_->prepare("SELECT relative_path, size FROM file_entries WHERE root_id = ? ORDER BY random() LIMIT ?");
        if (!st) return std::unexpected(database_error(st.error()));
        st->bind(1, id).bind(2, settings->relocation_sample_size);
        int checked = 0, matched = 0;
        while (true) {
            auto row = st->step();
            if (!row) return std::unexpected(database_error(row.error()));
            if (!*row) break;
            ++checked;
            std::error_code ec;
            const auto size = fs::file_size(*directory / detail::from_utf8(st->column_text(0)), ec);
            if (!ec && size == static_cast<std::uintmax_t>(st->column_int64(1))) ++matched;
        }
        if (checked > 0 && matched * 100 < settings->relocation_min_match_percent * checked) {
            return std::unexpected(Error{ErrorCode::InvalidArgument,
                                         "only " + std::to_string(matched) + " of " + std::to_string(checked) +
                                             " checked files were found there"});
        }
    }

    bool use_marker = current->use_marker;
    if (use_marker && !marker && !detail::write_marker(*directory, current->uuid)) use_marker = false;

    auto st = db_->prepare("UPDATE file_roots SET path = ?, path_in_volume = ?, use_marker = ?, status = ? WHERE id = ?");
    if (!st) return std::unexpected(database_error(st.error()));
    st->bind(1, std::string_view(detail::to_utf8(*directory).text))
        .bind(2, std::string_view(path_in_volume(*directory)))
        .bind(3, use_marker ? 1 : 0)
        .bind(4, static_cast<std::int64_t>(RootStatus::Online))
        .bind(5, id);
    if (auto r = st->run(); !r) return std::unexpected(database_error(r.error()));
    return root(id);
}

Result<std::vector<Entry>> Library::entries(RootId root_id, EntryId after, std::size_t limit) {
    auto st = db_->prepare("SELECT " + std::string(detail::entry_columns()) +
                           " FROM file_entries WHERE root_id = ? AND id > ? ORDER BY id LIMIT ?");
    if (!st) return std::unexpected(database_error(st.error()));
    st->bind(1, root_id).bind(2, after).bind(3, static_cast<std::int64_t>(limit));
    std::vector<Entry> out;
    while (true) {
        auto row = st->step();
        if (!row) return std::unexpected(database_error(row.error()));
        if (!*row) break;
        out.push_back(detail::read_entry(*st));
    }
    return out;
}

Result<fs::path> Library::resolve(EntryId entry_id) {
    auto st = db_->prepare("SELECT root_id, relative_path FROM file_entries WHERE id = ?");
    if (!st) return std::unexpected(database_error(st.error()));
    st->bind(1, entry_id);
    auto row = st->step();
    if (!row) return std::unexpected(database_error(row.error()));
    if (!*row) return std::unexpected(Error{ErrorCode::NotFound, "no such file"});
    const RootId root_id = st->column_int64(0);
    const std::string relative = st->column_text(1);

    auto owner = root(root_id);
    if (!owner) return std::unexpected(owner.error());
    auto resolution = resolve_root(*owner);
    if (!resolution) return std::unexpected(resolution.error());
    if (!resolution->online) {
        (void)detail::set_root_status(*db_, root_id, RootStatus::Offline);
        return std::unexpected(Error{ErrorCode::RootOffline, resolution->reason});
    }
    const fs::path full = detail::from_utf8(owner->path) / detail::from_utf8(relative);
    std::error_code ec;
    if (!fs::is_regular_file(full, ec)) {
        return std::unexpected(Error{ErrorCode::FileMissing, "the file is no longer in its folder"});
    }
    return full;
}

}

namespace hoardor::file {

Result<std::vector<Entry>> Library::companions(EntryId entry_id, int parent_levels, std::size_t limit) {
    auto st = db_->prepare("SELECT root_id, path_key FROM file_entries WHERE id = ?");
    if (!st) return std::unexpected(database_error(st.error()));
    st->bind(1, entry_id);
    auto row = st->step();
    if (!row) return std::unexpected(database_error(row.error()));
    if (!*row) return std::unexpected(Error{ErrorCode::NotFound, "no such file"});
    const RootId root_id = st->column_int64(0);
    std::string folder = st->column_text(1);  // its key; trimmed to the folder below

    // Entries directly in one folder: keys in [folder/, folder0) with no further '/'
    // ('/' is 0x2F and '0' is 0x30, the same trick as the sync's subtree touch).
    auto in_folder = db_->prepare("SELECT " + std::string(detail::entry_columns()) +
                                  " FROM file_entries WHERE root_id = ? AND path_key >= ? AND path_key < ? "
                                  "AND instr(substr(path_key, ?), '/') = 0 AND kind IN (?, ?, ?) AND id <> ? "
                                  "ORDER BY path_key LIMIT ?");
    if (!in_folder) return std::unexpected(database_error(in_folder.error()));

    std::vector<Entry> out;
    for (int level = 0; level <= parent_levels && out.size() < limit; ++level) {
        const auto slash = folder.rfind('/');
        const bool at_root = slash == std::string::npos;
        folder = at_root ? std::string() : folder.substr(0, slash);
        const std::string prefix = at_root ? std::string() : folder + "/";
        const std::string upper = at_root ? std::string("\xF4\x90") : folder + "0";  // above any UTF-8 key
        in_folder->bind(1, root_id)
            .bind(2, std::string_view(prefix))
            .bind(3, std::string_view(upper))
            .bind(4, static_cast<std::int64_t>(prefix.size() + 1))
            .bind(5, static_cast<std::int64_t>(FileKind::Image))
            .bind(6, static_cast<std::int64_t>(FileKind::Subtitle))
            .bind(7, static_cast<std::int64_t>(FileKind::Info))
            .bind(8, entry_id)
            .bind(9, static_cast<std::int64_t>(limit - out.size()));
        while (true) {
            auto next = in_folder->step();
            if (!next) return std::unexpected(database_error(next.error()));
            if (!*next) break;
            out.push_back(detail::read_entry(*in_folder));
        }
        in_folder->reset();
        if (at_root) break;
    }
    return out;
}

}
