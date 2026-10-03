#pragma once

#include <hoardor/db/database.hpp>
#include <hoardor/file/mount_points.hpp>
#include <hoardor/file/scanner.hpp>
#include <hoardor/file/settings.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace hoardor::file {

using CategoryId = std::int64_t;
using RootId = std::int64_t;
using EntryId = std::int64_t;

enum class ErrorCode {
    NotFound,         // no such category, root, entry, or path
    InvalidArgument,  // e.g. an empty name, a path that isn't a directory, a marker mismatch
    AlreadyExists,    // a duplicate category name, or a folder that is already a root
    Overlap,          // a root inside another root, or containing one
    InUse,            // a category that still has roots
    RootOffline,      // the root's storage isn't available
    FileMissing,      // the root is online but the file is gone
    Database,         // SQLite failed
};

struct Error {
    ErrorCode code;
    std::string message;
};

template <class T>
using Result = std::expected<T, Error>;

// A section of the library (Music, Movies, ...): user configuration, not code.
struct Category {
    CategoryId id = 0;
    std::string name;
    std::vector<FileKind> kinds;  // the file kinds this category accepts
};

enum class RootStatus : std::uint8_t { Unknown = 0, Online = 1, Offline = 2 };

// A directory the user assigned to a category (engines/file.md §2.2).
struct Root {
    RootId id = 0;
    std::string uuid;
    CategoryId category_id = 0;
    std::string name;
    std::string path;            // last-known absolute path (UTF-8, '/' separators)
    std::string path_in_volume;  // the same folder relative to its mount point; "" = the whole volume
    bool use_marker = true;
    bool case_sensitive = true;
    RootStatus status = RootStatus::Unknown;
    std::int64_t generation = 0;  // the last completed sync
    std::int64_t last_sync_ns = 0;
    std::uint64_t file_count = 0;
    std::uint64_t held_removals = 0;  // removals waiting for apply_held_removals
};

struct Entry {
    EntryId id = 0;
    RootId root_id = 0;
    std::string relative_path;  // exact name from disk (UTF-8, '/' separators)
    std::uint64_t size = 0;
    std::int64_t mtime_ns = 0;
    FileKind kind{};
    bool unsettled = false;  // probably still being copied; metadata engines skip it
    std::int64_t changed_generation = 0;
    std::int64_t added_ns = 0;  // when a sync first found it (entries from before v0.2.0: their mtime)
};

struct ScanErrorRecord {
    RootId root_id = 0;
    std::string relative_path;  // empty: the whole root was lost during the sync
    bool is_directory = false;
    std::string message;
    std::int64_t generation = 0;
};

enum class RootSyncOutcome {
    Synced,     // scanned and reconciled (removals may still be held, see removals_held)
    Offline,    // the storage wasn't there, was lost, or looked unmounted: nothing was removed
    Cancelled,  // stopped by request: what was seen is saved, nothing was removed
    Failed,     // something unexpected (e.g. a database error): see message
};

struct RootSyncReport {
    RootId root_id = 0;
    RootSyncOutcome outcome = RootSyncOutcome::Failed;
    std::int64_t generation = 0;  // page through changed_entries(root_id, generation)
    std::uint64_t added = 0;
    std::uint64_t modified = 0;  // includes files that just became settled
    std::uint64_t removed = 0;
    std::uint64_t unchanged = 0;
    std::uint64_t unsettled = 0;
    std::uint64_t errors = 0;  // see scan_errors(root_id)
    bool relocated = false;    // the root was found at a new location
    bool removals_held = false;
    std::uint64_t held_removals = 0;
    std::string message;  // why it's offline, cancelled, or failed, in plain words
};

struct SyncReport {
    std::vector<RootSyncReport> roots;
    bool cancelled = false;
};

struct SyncProgress {
    std::size_t root_index = 0;  // 0-based position of the root being synced
    std::size_t root_count = 0;
    RootId root_id = 0;
    ScanProgress scan;  // the scanner's counters for this root so far
};

using ProgressCallback = std::function<void(const SyncProgress&)>;

// The file engine's repository and logic, on one database connection.
// Use a Library from one thread at a time (its Database's thread).
class Library {
public:
    // Runs the file engine's migrations. `database` must outlive the Library.
    static Result<Library> open(db::Database& database, MountPointLister mounts = list_mount_points);

    // ---- Settings ----

    // Settings::defaults() overlaid with every stored value that parses.
    Result<Settings> load_settings();
    Result<void> save_settings(const Settings& settings);

    // ---- Categories ----

    Result<std::vector<Category>> categories();
    Result<CategoryId> add_category(std::string_view name, const std::vector<FileKind>& kinds);
    Result<void> update_category(const Category& category);
    Result<void> remove_category(CategoryId id);  // InUse while roots belong to it

    // ---- Roots ----

    Result<std::vector<Root>> roots(std::optional<CategoryId> category = std::nullopt);
    Result<Root> root(RootId id);
    // Writes the .hoardor-root marker unless use_marker is false; falls back to
    // use_marker = false when the storage is read-only.
    Result<Root> add_root(CategoryId category, const std::filesystem::path& path, std::string_view name = {},
                          bool use_marker = true);
    Result<void> remove_root(RootId id);  // deletes its entries; the marker file stays on disk
    Result<void> set_root_category(RootId id, CategoryId category);
    Result<void> set_root_case_sensitive(RootId id, bool case_sensitive);
    // Points a root at its new location, verified by its marker (or, without one, by a sample of entries).
    Result<Root> relocate_root(RootId id, const std::filesystem::path& new_path);

    // ---- Sync (features/file_sync.md §4.6) ----

    // Syncs every root of one category, or every root, one at a time. Blocking:
    // run it on a background thread (master::SyncWorker does). The progress
    // callback runs on the calling thread, never inside a write transaction.
    SyncReport sync(std::optional<CategoryId> category = std::nullopt, std::stop_token stop = {},
                    const ProgressCallback& progress = {});
    RootSyncReport sync_root(RootId id, std::stop_token stop = {}, const ProgressCallback& progress = {});
    // Applies removals that a sync held back by the mass-removal guard. Returns how many were removed.
    Result<std::uint64_t> apply_held_removals(RootId id);

    // ---- Reads ----

    // One entry by id (NotFound if it's gone). Reads SQLite only.
    Result<Entry> entry(EntryId id);
    // Paged by id, ascending, so consumers never load everything.
    Result<std::vector<Entry>> entries(RootId root, EntryId after = 0, std::size_t limit = 500);
    // The entries added or changed by one sync (RootSyncReport::generation), paged by id.
    Result<std::vector<Entry>> changed_entries(RootId root, std::int64_t generation, EntryId after = 0,
                                               std::size_t limit = 500);
    // What the last sync of this root couldn't read (up to `limit`).
    Result<std::vector<ScanErrorRecord>> scan_errors(RootId root, std::size_t limit = 500);
    // The image, subtitle, and info (.nfo) entries in the same folder as `entry`, and, with
    // parent_levels > 0, those directly in up to that many parent folders (a show's tvshow.nfo
    // and poster). Reads SQLite only. Up to `limit` entries, nearest folder first.
    // With `prefixes`, only the files whose name starts with one of them ("Arrival (2016).",
    // "poster."): one index lookup per prefix instead of the whole folder, so a flat folder of
    // a thousand movies and their sidecars never crowds out the ones that matter. A prefix
    // matches as given and in lowercase, Capitalized, and UPPERCASE ("Poster.jpg").
    Result<std::vector<Entry>> companions(EntryId entry, int parent_levels = 0, std::size_t limit = 200,
                                          std::span<const std::string> prefixes = {});
    // For playback: the file's current absolute path, or RootOffline / FileMissing.
    // Touches the drive (resolves the root), so never call it just to display the library.
    Result<std::filesystem::path> resolve(EntryId entry);

private:
    struct Resolution {
        bool online = false;
        bool relocated = false;
        std::string reason;  // why it's offline
    };

    Library(db::Database& database, MountPointLister mounts) : db_(&database), mounts_(std::move(mounts)) {}

    std::string path_in_volume(const std::filesystem::path& path) const;
    Result<void> check_overlap(const std::filesystem::path& path, std::optional<RootId> except);
    // Finds the root's storage (engines/file.md §2.3) and records a relocation.
    Result<Resolution> resolve_root(Root& root);
    RootSyncReport sync_one(RootId id, std::stop_token stop, const ProgressCallback& progress, std::size_t index,
                            std::size_t count);

    db::Database* db_;
    MountPointLister mounts_;
};

}
