# Feature: File Sync (v1)

| | |
|---|---|
| Status | **Shipped in `v0.1.0`** (2026-10-02): approved on 2026-10-01, phases 1 and 2 built on 2026-10-01, first built with MSVC and tested by the user on Windows on 2026-10-02. It lands on `master` with the PR from `abhinavp06/FILE_SCANNER_INIT`; the user's code review happens in that PR |
| Branch | `abhinavp06/FILE_SCANNER_INIT` (one PR) |
| Ships in | `v0.1.0` |
| Engines involved | `file` (scanner, roots, reconciliation), `db` (SQLite mechanics, phase 2), `master` (background sync worker, phase 2) |
| Engine reference | `engines/file.md`: responsibilities, storage model, roadmap |

This doc is the plan for one feature: goals, phases, API detail, edge cases, tests, decisions, and open items. Long-lived facts about the file engine (what it owns, the storage model, root identity, safety rules) live in `engines/file.md`, and this doc builds on them. When the feature ships, the engine doc is updated to match what was built, and this doc's status becomes **Shipped (`v0.1.0`)**.

## 1. Overview

**What the user gets:**
- In the app's configuration, the user sets up categories (Music, Movies, Shows, Books, …) and the directories that belong to each. A category can have several directories, on different drives.
- Pressing **Sync** in a section of the UI syncs every directory of that section's category. A global Sync, if the UI ever offers one, syncs every directory.
- Sync runs in the background, so the app stays snappy. A sync at startup is an opt-in setting.
- The library is kept in SQLite. Unplugged drives show as offline, never as deleted. Every tunable is a setting.

**What it isn't:** realtime watching, move and rename detection, and per-OS volume support. Those are later items on the engine roadmap (`engines/file.md` §4).

## 2. Phases

| Phase | Scope | Introduces |
|---|---|---|
| **1. Scanning** | Streaming `Scanner` with progress, `file::Settings` (every tunable, with defaults), build and test wiring, benchmark | GoogleTest, Google Benchmark |
| 2. Roots, persistence, reconciliation | Settings persisted in SQLite, library roots and their identity (marker plus last-known path), resolution and relocation, mount-point listing (Linux and Windows), the `file_*` tables, `sync()` for one category or all of them, safety rules, unsettled files, the background sync worker in `master` | SQLite, a minimal `hoardor::db`, the first slice of `master` |

Each phase is designed in this doc, then built and tested, before the next one starts.

## 3. Phase 1: Scanning

### 3.1 Goal

Walk one directory tree and stream every media file in it, with relative path, size, mtime, and kind, using memory that does not grow with the number of files. Report progress while it runs, and fail cleanly when the storage disappears. Nothing is persisted. Phase 2 feeds this stream into the database.

### 3.2 Public API

The existing skeleton (`file_engine.hpp`: `discover()`, `MediaType`, `FileType`, `File`, `FileEntry`) is replaced:
- `discover()` collects everything into a `std::vector`, which breaks the memory rule.
- `MediaType` hard-codes categories, which ARCHITECTURE §6 says are configuration.

```cpp
// include/hoardor/file/settings.hpp
namespace hoardor::file {

enum class FileKind : std::uint8_t { Audio, Video, Text, Image, Subtitle };

// Every tunable of the file engine, as plain data. The defaults live in code.
// Phase 2 persists this struct in SQLite. A value missing from the database
// falls back to the default here. Phase 2 also adds the sync settings
// (sync_on_startup, settle window, mass-removal threshold) to this struct.
struct Settings {
    // Extension (any case, with or without a leading dot) -> kind.
    // Files with an unmapped extension are not media and are skipped.
    std::unordered_map<std::string, FileKind> extension_kinds;
    // Exact file or directory names to skip, matched without regard to case.
    // A skipped directory is not descended into.
    std::vector<std::string> ignored_names;
    // Name prefixes to skip, e.g. "._" for macOS AppleDouble files.
    std::vector<std::string> ignored_prefixes;

    static Settings defaults();  // see §3.4
};

// The kind of a file by its extension, or std::nullopt when it isn't media.
std::optional<FileKind> kind_of(const std::filesystem::path& file, const Settings& settings);

}
```

```cpp
// include/hoardor/file/scanner.hpp
namespace hoardor::file {

struct ScannedFile {
    std::string   relative_path;  // UTF-8, '/' separators, relative to the root, not normalized
    std::uint64_t size = 0;       // bytes
    std::int64_t  mtime_ns = 0;   // nanoseconds since the Unix epoch (UTC)
    FileKind      kind{};
};

// A path that could not be read.
// - is_directory = true: the whole subtree is unknown. Phase 2 must NOT treat its files as deleted.
// - relative_path empty: the root itself was lost (e.g. the drive was unplugged). This is
//   always the last item of the scan.
struct ScanError {
    std::string     relative_path;  // UTF-8, '/' separators
    std::error_code code;
    bool            is_directory = false;
};

using ScanResult = std::expected<ScannedFile, ScanError>;

struct ScanProgress {
    std::uint64_t directories_visited = 0;
    std::uint64_t files_emitted = 0;
    std::uint64_t errors = 0;
};

class Scanner {
public:
    // Fails if root does not exist, is not a directory, or cannot be opened.
    static std::expected<Scanner, std::error_code>
    open(const std::filesystem::path& root, const Settings& settings);

    // The next media file or error, or std::nullopt when the scan is complete.
    std::optional<ScanResult> next();

    // Cheap counters for progress reporting. A total isn't known up front.
    // Phase 2 estimates it from the previous scan's file count.
    ScanProgress progress() const;
};

}
```

Usage:

```cpp
auto scanner = hoardor::file::Scanner::open(root, hoardor::file::Settings::defaults());
if (!scanner) { /* scanner.error() */ }
while (auto result = scanner->next()) {
    if (*result) use(**result);       // a ScannedFile
    else         log(result->error()); // a ScanError
}
```

**Why so few types:** phase 1 has exactly two things with behavior: the `Scanner` (it holds the walk's state) and `kind_of` (a lookup, so a free function, not a class). Everything else is plain data. `Settings` is a struct, not a class with setters: the caller edits the map and vectors directly, and phase 2 can save and load it field by field. `Scanner::open` makes its own lowercased copy of the extensions and ignore lists. That way, user-entered values like `"FLAC"` or `".flac"` just work, and lookups stay fast.

**Why a pull-based `next()` rather than a callback or a returned vector:**
- **Vector:** rejected because memory grows with the library.
- **Callback:** works, but the caller can't easily stop early, and can't batch rows into one database transaction per N files (phase 2) without buffering inside the callback.
- **`std::generator` (C++23):** would be the nicest syntax, but GCC 13 (this VM) and Apple Clang don't ship it yet.
- **`next()` wins:** the caller controls the pace, batches naturally, and cancels a user-started scan by simply stopping and destroying the scanner.

**Why `std::expected` (C++23):** each item is either a file or an error, and the type makes the caller handle both. Errors are part of the stream, in order, so phase 2 knows exactly which subtree is unknown. Collecting errors into a list would grow memory without bound on a badly broken drive.

### 3.3 Behavior

- **Emitted:** regular files with a mapped extension only. Directories, symlinks, FIFOs, sockets, and devices are never emitted.
- **Symlinks are not followed.** This avoids cycles and double-counting. A user who wants linked content adds its target as a separate root.
- **Ordering** is file-system order, with no sorting. Sorting would need the whole listing in memory.
- **Memory:** the scanner keeps its own stack of `std::filesystem::directory_iterator`s, one open directory handle per level of depth, so memory is proportional to tree depth, not file count.
  - **Why not `recursive_directory_iterator`:** in libstdc++, the first error on any subdirectory (permission denied, an I/O error) ends the *whole* walk. Error handling has to stay local to each directory, so the scanner manages the stack itself.
- **Stat-only:** the scanner never opens files.
  - **Type checks** (symlink, directory, regular file) use the type the directory listing already returned (`d_type` on Linux), so they cost no system call.
  - **Size and mtime** come from one platform call, `detail::file_info()` in `src/file/platform/`:
    - **POSIX:** a single `stat()`.
    - **Windows:** the values the directory listing already cached, so no system call.
  - *History:* libstdc++'s `file_size()` plus `last_write_time()` walked the full path twice, and `perf` showed 70% of sync time in kernel path lookups.
- **Root loss:** on any read error, the scanner checks whether the root itself is still reachable, by opening it. Opening forces a real read, whereas a cached `stat` of a dead mount can still succeed. If the root is unreachable, the scanner emits a single root `ScanError` and ends, instead of thousands of per-directory errors from an unplugged drive. An error while listing the root itself is always a root error.
- **Deleted mid-scan:** a file *or folder* that disappears between the listing and its `stat` or open is skipped silently, as long as the root is still reachable. It's really gone, so a later sync may remove it.
- **Paths** are converted by hoardor's own non-throwing converter (`src/file/text.cpp`): UTF-16 on Windows, bytes on POSIX, producing UTF-8 with `/` separators on every OS.
  - `path::string()` isn't used: on Windows it converts to the ANSI code page and mangles non-Latin names.
  - `path::u8string()` isn't used either: MSVC throws on an unpaired surrogate, and libstdc++ passes invalid bytes through unchecked.
  - A name that can't be represented as valid UTF-8 (an unpaired UTF-16 surrogate on Windows, or invalid bytes on Linux) is reported as a `ScanError` with `std::errc::illegal_byte_sequence`. It is not emitted, so the "paths are UTF-8" rule holds everywhere.
- **mtime:** `std::filesystem::file_time_type` has a platform-specific epoch, so it is converted once, in one helper (`src/file/file_time.hpp`), to `int64` nanoseconds since the Unix epoch, with `std::chrono::clock_cast`. The conversion is exact, so the same file always yields the same value.
- **Ignored names** match case-insensitively for ASCII letters only (`@EADIR` matches `@eaDir`). Non-ASCII letters must match exactly.
- **Single-threaded.** Scanning several roots in parallel is the caller's decision. Phase 2 scans one root at a time, which is safe for HDDs.

### 3.4 Defaults (configuration, not constants)

These are the values `Settings::defaults()` returns. Every one of them can be changed by the user. Settings start as configurable as possible, and the ones nobody uses get pruned later, based on real use.

`extension_kinds`:

| Kind | Extensions |
|---|---|
| Audio | mp3, flac, ogg, oga, opus, m4a, m4b, aac, wav, aiff, aif, wma, ape, wv, dsf, dff, alac |
| Video | mkv, mp4, m4v, avi, mov, wmv, webm, ts, m2ts, mpg, mpeg, flv |
| Text | epub, pdf, txt, md, mobi, azw3, cbz, cbr, html, htm |
| Image | jpg, jpeg, png, webp, gif, bmp |
| Subtitle | srt, ass, ssa, vtt, sub, idx |

`ignored_names` and `ignored_prefixes` default to OS and NAS litter found on external drives and shares:

| Setting | Default | Where it comes from |
|---|---|---|
| `ignored_names` | `$RECYCLE.BIN`, `System Volume Information`, `Thumbs.db`, `desktop.ini`, `FOUND.000` | Windows |
| | `.DS_Store`, `.Trashes`, `.Spotlight-V100`, `.fseventsd`, `.TemporaryItems`, `.DocumentRevisions-V100` | macOS |
| | `lost+found` | Linux |
| | `@eaDir`, `#recycle`, `#snapshot`, `.snapshot` | NAS (Synology and others). `@eaDir` is full of generated thumbnail `.jpg` files that would otherwise be emitted as images |
| | `.hoardor-root` | hoardor's own marker (`engines/file.md` §2.3) |
| `ignored_prefixes` | `._` | macOS AppleDouble files on FAT, exFAT, and SMB |
| | `.Trash-` | Linux per-user trash on removable drives |

`System Volume Information` also returns "access denied" on Windows, which would otherwise show up as a `ScanError` on every scan.

### 3.5 Edge cases

| Case | Expected behavior |
|---|---|
| Root does not exist, or is a file | `Scanner::open` returns an error |
| Root is empty | `next()` returns `std::nullopt` immediately. Deciding that "empty" means "unmounted" is phase 2's job (`engines/file.md` §2.4) |
| Root is a whole volume (`E:\`, `/media/user/Drive`) | Works. Volume litter is skipped by the ignore defaults |
| Root removed mid-scan (drive unplugged) | One root `ScanError` (empty `relative_path`), then `std::nullopt` |
| Unmapped extension, or no extension | Skipped |
| Uppercase or mixed-case extension (`SONG.FLAC`) | Classified normally |
| Name that is only an extension (`.mp3`) | Skipped, because `std::filesystem` treats it as a stem with no extension |
| Multiple dots (`a.live.flac`) | Classified by the last extension |
| Non-ASCII names (CJK, emoji, combining accents, NFD names written by macOS) | `relative_path` is valid UTF-8, byte-for-byte what the file system returned, and opens the same file |
| Name that can't be represented as UTF-8 | `ScanError` with `illegal_byte_sequence`, and the scan continues |
| Deep nesting / total path over 260 characters | Works on Linux. Windows needs long-path awareness in the host app, verified by hand until CI exists |
| Unreadable subdirectory | One `ScanError` with `is_directory = true`, then the scan continues with its siblings |
| File removed between listing and `stat` | Silently skipped. A vanished file is not an error |
| Ignored name or prefix (file or directory) | Skipped. An ignored directory is not descended into |
| Symlink to a file or directory | Not emitted, not followed |
| FIFO, socket, or device | Not emitted |
| Scanner destroyed mid-scan | No leak, no crash |
| Two names differing only in case (Linux) | Both emitted. Case-sensitivity per root is a phase 2 concern |

### 3.6 Files

- `include/hoardor/file/settings.hpp`, `src/file/settings.cpp`: `FileKind`, `Settings`, `Settings::defaults()`, `kind_of()`
- `include/hoardor/file/scanner.hpp`, `src/file/scanner.cpp`
- `include/hoardor/file/file_engine.hpp` and `src/file/file_engine.cpp`: removed
- `tests/file/settings_test.cpp`, `tests/file/scanner_test.cpp`
- `benchmarks/file/scanner_benchmark.cpp`
- `playground/file/file_engine_playground.cpp`: rewritten to scan a path from the command line and print progress, counts, and timing, for manual runs against a real external drive
- `CMakeLists.txt`, `tests/CMakeLists.txt`, `playground/CMakeLists.txt`, `benchmarks/CMakeLists.txt`: real targets. GoogleTest and Google Benchmark come in through `FetchContent`. A new `BUILD_BENCHMARKS` option defaults to OFF. `RUN_TESTS_AFTER_BUILD` is actually wired up.

### 3.7 Test plan

GoogleTest. Each test builds its own tree in a unique temporary directory and removes it afterwards.

- **`Settings` and `kind_of`:**
  - defaults are complete
  - lookups ignore case
  - user-provided keys with a leading dot or uppercase letters still match
  - an edited map (an extension added, changed, or removed) is respected
  - no extension, a dot-only name, multiple dots
  - empty settings: nothing is media, so nothing is emitted
- **`Scanner`:** every row in §3.5, plus every default ignore entry.
  - **Root loss:** delete the root directory partway through iteration.
  - **Unreadable directory:** uses `chmod 000`. Skipped when running as root (as on this VM), because root ignores permissions. It runs when the suite is run as a normal user.
  - **FIFO:** Linux and macOS only.
  - **Invalid UTF-8 name:** Linux only, since Linux allows arbitrary bytes in names.
  - **mtime:** sets a known time with `last_write_time` and checks the exact nanosecond value.
  - **Progress:** counters match the generated tree.

### 3.8 Performance

- **Target:** a full scan of a 50k-file tree in under 1 s with a warm OS cache (ARCHITECTURE §7).
- **Benchmark:** Google Benchmark scans a generated tree of 50k empty files, spread across nested directories. Empty files cost the same as real ones here, because the scanner only calls `stat`.
- **Result (Release build, this Linux VM, warm cache): 317 ms for 50k files (≈158k files/s)**, within the target.
  - The first version took 709 ms. It used `symlink_status()`, an uncached `lstat` per entry in libstdc++.
  - The second took 427 ms, with two path-walking `stat` calls per media file. One `stat` gives 317 ms.
  - Syscall profile for a real tree: one `openat` and `getdents64` per directory, and `newfstatat` only for media files.
- **Cold external HDD:** measured by hand with the playground against the user's real drive, after a reboot or a drive power cycle so the cache is cold. Recorded in the changelog. This is the number that matters most for how long a Sync takes.
- **Memory:** peak RSS stays flat between a 5k-file tree and a 50k-file tree. Checked by hand with the playground and `/usr/bin/time -v`. **Result: 4,224 KB for both** (the whole process).
- **Running as non-root:** the permission test (unreadable folder) is skipped as root. It was run and passes as user `nobody`.

### 3.9 Known limitations

- Symlinks are never followed. Following them needs cycle detection, and it can be added if a real library needs it.
- The ignore list matches exact names and prefixes only, with no glob patterns.
- Cancellation happens between items. A single `stat` blocked on a slow or hung drive (a spinning-up HDD, a hung NFS mount) can't be interrupted. Scans therefore always run off the UI thread.
- CI is deferred. Windows behavior (UTF-8 conversion, mtime epoch, long paths) is verified by hand on Windows.
- **Windows junctions: to verify by hand.** The cached type check relies on the standard library reporting a junction as a symlink (or at least not as a plain directory). If MSVC reports junctions as directories, the scanner would descend into them, which risks a cycle on a junction loop. Media drives rarely contain junctions.


## 4. Phase 2: Roots, persistence, reconciliation

Part of File Sync v1. Detailed on 2026-10-01 after phase 1 was built. The DB mechanics are in `engines/db.md`, and the background worker in `engines/master.md`.

### 4.1 Overview

| Piece | Where | What it does |
|---|---|---|
| `db::Database`, `db::Statement`, `db::Transaction`, `db::migrate` | `hoardor::db` | One SQLite connection, prepared statements, RAII transactions, versioned migrations per engine |
| `file::Library` | `hoardor::file` | The file engine's repository and logic: settings, categories, roots, sync, queries, `resolve()` |
| `master::SyncWorker` | `hoardor::master` | Runs `file::Library::sync()` on a hoardor-owned background thread |
| `list_mount_points()` | `src/file/platform/` | The OS's mounted volumes, for finding a relocated root |

Following the "keep it simple" rule, that's four classes (`Database`, `Statement`, `Transaction`, `Library`) plus the worker. Everything else is a plain struct or a free function.

**Connections and threads:**
- Each thread uses its **own** `db::Database` connection: the UI or caller has one, and the sync worker has one.
- SQLite's WAL mode lets readers run while the worker writes.
- The worker's write transactions are short (§4.6), and `busy_timeout` makes another writer wait briefly instead of failing.
- This replaces the earlier "single writer thread" idea in ARCHITECTURE §3: it gives the same behavior with no queue or thread inside `db`.

### 4.2 Public API (`include/hoardor/file/library.hpp`)

```cpp
namespace hoardor::file {

using CategoryId = std::int64_t;
using RootId = std::int64_t;
using EntryId = std::int64_t;

enum class ErrorCode { NotFound, InvalidArgument, AlreadyExists, Overlap, InUse, RootOffline, FileMissing, Io, Database };
struct Error { ErrorCode code; std::string message; };
template <class T> using Result = std::expected<T, Error>;

struct Category { CategoryId id; std::string name; std::vector<FileKind> kinds; };

enum class RootStatus : std::uint8_t { Unknown = 0, Online = 1, Offline = 2 };
struct Root {
    RootId id; std::string uuid; CategoryId category_id; std::string name;
    std::string path;            // last-known absolute path (UTF-8)
    std::string path_in_volume;  // the same folder relative to its mount point; "" = the whole volume
    bool use_marker; bool case_sensitive; RootStatus status;
    std::int64_t generation;     // the last completed sync
    std::int64_t last_sync_ns; std::uint64_t file_count; std::uint64_t held_removals;
};
struct Entry {
    EntryId id; RootId root_id; std::string relative_path; std::uint64_t size; std::int64_t mtime_ns;
    FileKind kind; bool unsettled; std::int64_t changed_generation;
};
struct ScanErrorRecord { RootId root_id; std::string relative_path; bool is_directory; std::string message; std::int64_t generation; };

enum class RootSyncOutcome { Synced, Offline, Cancelled, Failed };
struct RootSyncReport {
    RootId root_id; RootSyncOutcome outcome; std::int64_t generation;
    std::uint64_t added, modified, removed, unchanged, unsettled, errors;
    bool relocated; bool removals_held; std::uint64_t held_removals;
    std::string message;  // why it's offline or failed, in plain words
};
struct SyncReport { std::vector<RootSyncReport> roots; bool cancelled; };
struct SyncProgress { std::size_t root_index; std::size_t root_count; RootId root_id; ScanProgress scan; };
using ProgressCallback = std::function<void(const SyncProgress&)>;
using MountPointLister = std::function<std::vector<std::filesystem::path>()>;

class Library {
public:
    // Runs the file engine's migrations. The Database must outlive the Library.
    static Result<Library> open(db::Database& database, MountPointLister mounts = list_mount_points);

    Result<Settings> load_settings();                 // the defaults overlaid with the stored values
    Result<void> save_settings(const Settings& settings);

    Result<std::vector<Category>> categories();
    Result<CategoryId> add_category(std::string_view name, std::vector<FileKind> kinds);
    Result<void> update_category(const Category& category);
    Result<void> remove_category(CategoryId id);      // InUse while roots belong to it

    Result<std::vector<Root>> roots(std::optional<CategoryId> category = {});
    Result<Root> root(RootId id);
    Result<Root> add_root(CategoryId category, const std::filesystem::path& path, std::string_view name = {}, bool use_marker = true);
    Result<void> remove_root(RootId id);              // deletes its entries; the marker file stays
    Result<void> set_root_category(RootId id, CategoryId category);
    Result<void> set_root_case_sensitive(RootId id, bool case_sensitive);
    Result<Root> relocate_root(RootId id, const std::filesystem::path& new_path);

    SyncReport sync(std::optional<CategoryId> category = {}, std::stop_token stop = {}, const ProgressCallback& progress = {});
    RootSyncReport sync_root(RootId id, std::stop_token stop = {}, const ProgressCallback& progress = {});
    Result<std::uint64_t> apply_held_removals(RootId id);

    // Paged reads (by id, ascending), for consumers that never load everything.
    Result<std::vector<Entry>> entries(RootId root, EntryId after = 0, std::size_t limit = 500);
    Result<std::vector<Entry>> changed_entries(RootId root, std::int64_t generation, EntryId after = 0, std::size_t limit = 500);
    Result<std::vector<ScanErrorRecord>> scan_errors(RootId root);
    Result<std::filesystem::path> resolve(EntryId entry);  // for playback: RootOffline / FileMissing
};

}
```

- **Callers react to the report, not to lists.** A sync never returns lists of changed files, which would grow with the library. It returns counts and a `generation`. Consumers page through `changed_entries(root, generation)`.
- **Removed files clean up after themselves.** Other engines' tables reference `file_entries(id)` with `ON DELETE CASCADE`, so their data goes away with the file.

### 4.3 Schema (file engine migration 1)

```sql
CREATE TABLE file_settings   (key TEXT PRIMARY KEY, value TEXT NOT NULL);
CREATE TABLE file_categories (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE COLLATE NOCASE,
                              kinds TEXT NOT NULL);                           -- "audio,image"
CREATE TABLE file_roots (
    id INTEGER PRIMARY KEY, uuid TEXT NOT NULL UNIQUE,
    category_id INTEGER NOT NULL REFERENCES file_categories(id),
    name TEXT NOT NULL, path TEXT NOT NULL, path_in_volume TEXT NOT NULL,
    use_marker INTEGER NOT NULL, case_sensitive INTEGER NOT NULL,
    status INTEGER NOT NULL DEFAULT 0, generation INTEGER NOT NULL DEFAULT 0,
    last_sync_ns INTEGER NOT NULL DEFAULT 0, file_count INTEGER NOT NULL DEFAULT 0,
    held_removals INTEGER NOT NULL DEFAULT 0);
CREATE TABLE file_entries (
    id INTEGER PRIMARY KEY,
    root_id INTEGER NOT NULL REFERENCES file_roots(id) ON DELETE CASCADE,
    relative_path TEXT NOT NULL,            -- exact bytes from disk, for opening the file
    path_key TEXT NOT NULL,                 -- relative_path, ASCII-lowercased on case-insensitive roots
    size INTEGER NOT NULL, mtime_ns INTEGER NOT NULL, kind INTEGER NOT NULL,
    unsettled INTEGER NOT NULL DEFAULT 0,
    seen_generation INTEGER NOT NULL,       -- the last sync that saw it
    changed_generation INTEGER NOT NULL,    -- the last sync that added or changed it
    UNIQUE (root_id, path_key));
CREATE INDEX file_entries_changed ON file_entries (root_id, changed_generation, id);
CREATE TABLE file_scan_errors (
    root_id INTEGER NOT NULL REFERENCES file_roots(id) ON DELETE CASCADE,
    relative_path TEXT NOT NULL, is_directory INTEGER NOT NULL, message TEXT NOT NULL,
    generation INTEGER NOT NULL);
-- Seeded default categories: Music (audio,image), Movies (video,subtitle,image),
-- Shows (video,subtitle,image), Books (text,image). The user can edit or delete them.
```

- **`path_key`** makes a case-only rename on a case-insensitive drive (`song.mp3` → `Song.mp3` on NTFS or exFAT) update the same row, so the file keeps its id. Only ASCII letters are folded (a known limitation).
- **Sweeping by generation (mark and sweep):** each sync stamps every file it sees with the new generation. After a complete scan, rows with an older `seen_generation` are the removed files.

### 4.4 Settings persistence

- **Table:** `file_settings` holds one row per field.
- **Format:** integers are written in decimal, booleans as `true` or `false`, lists as newline-separated lines, and `extension_kinds` as `ext=kind` lines.
- **Saving:** `save_settings` first runs `validate()`. Any problem (a number out of range, an empty extension, a line break or `=` that would corrupt the line-based storage) refuses the whole save with `InvalidArgument` and a plain-words message, and nothing is stored. The ranges live in one table, `file::limits`, used by both saving and loading. *(Added 2026-10-01 for TYLI's Settings screen.)*
- **Loading:** start from `Settings::defaults()` and overlay every stored value that parses. A missing key, or a value that fails to parse, keeps the default. Unknown keys are ignored, so a newer database still opens in an older build.
- **New fields in `file::Settings`:**

| Field | Default | Meaning |
|---|---|---|
| `sync_on_startup` | `false` | `master` queues a global sync when it starts |
| `settle_window_seconds` | `10` | A file modified within this window of the sync (in either direction) is unsettled |
| `mass_removal_threshold_percent` | `25` | Above this share of a root's entries, removals are held for confirmation |
| `batch_max_rows` / `batch_max_milliseconds` | `2000` / `50` | Commit the write transaction after this many rows or this much time |
| `progress_interval_files` | `500` | Report progress every this many files |
| `relocation_sample_size` / `relocation_min_match_percent` | `20` / `80` | Manually relocating a root without a marker checks this many known entries and needs this share to match |

### 4.5 Roots

**Adding a root** (`add_root`):
1. **Check the path:** it must exist and be a directory. It's stored as an absolute, normalized UTF-8 path.
2. **Check the category:** it must exist.
3. **Check for overlap:** a root inside another root, or containing one, is rejected (`Overlap`). The comparison is component-wise and ignores ASCII case, so `E:\Music` and `e:\music\Rock` count as overlapping.
4. **Marker:**
   - If a `.hoardor-root` is already there with a UUID that another root uses, the folder is already a root (reached through another path), so it's rejected (`AlreadyExists`).
   - If the UUID isn't in use, it's **adopted**, so re-adding a folder after a lost database keeps its identity.
   - Otherwise a new UUID is written.
   - If writing fails (read-only storage), the root falls back to `use_marker = false`, which isn't an error.
5. **Path within its volume:** computed from the longest mount point that contains the path.
6. **Case sensitivity:** detected without writing anything. The marker or another name is looked up with its ASCII case flipped: if the flipped name is the same file, the root is case-insensitive. If nothing can be probed, the root defaults to case-sensitive. The user can override it (`set_root_case_sensitive`).

**Resolving a root** (at the start and end of a sync, in `resolve()`, and in `relocate_root()`). It follows `engines/file.md` §2.3:
- **With a marker:**
  1. If the last-known path holds our UUID, the root is online.
  2. Otherwise (no marker, an unreadable one, or **another root's UUID**), each mount point `M` is checked for our marker at `M/path_in_volume`.
  3. If found, the root has been relocated: its path is updated and it's online.
  4. If not, it's offline.
- **Without a marker:** the root is online if its path is an existing directory. The safety rules in §4.6 still protect it.

**Manual relocation** (`relocate_root`): the new path must be a directory, must not overlap other roots, and must hold our marker. If it holds no marker at all, a **sample check** is used instead: `relocation_sample_size` known entries must exist with the same size, at least `relocation_min_match_percent` of them. For a marker root, the marker is then written. If the folder holds a *different* root's marker, relocation is rejected.

### 4.6 Sync (per root)

1. **Load settings**, filtered to the root's category kinds.
2. **Resolve the root.** If it's offline, mark it Offline and report the reason. Nothing is scanned or removed.
3. `generation = root.generation + 1`. Count the root's existing entries.
4. **Stream the scanner** inside batched write transactions. A batch commits after `batch_max_rows` rows or `batch_max_milliseconds`, whichever comes first, and always before calling a callback, so no callback ever runs while holding the write lock. For each item:
   - **A file** is looked up by `(root_id, path_key)`:
     - *Not found:* insert it → **added**.
     - *Size, mtime, kind, or exact path changed:* update it → **modified**. A file whose unsettled flag clears also counts as modified, so metadata engines pick it up.
     - *Otherwise:* only `seen_generation` is updated → **unchanged**.
     - In every case, `changed_generation` is set for added and modified files, and `unsettled` is recomputed: `|mtime − now| < settle_window`. A file dated far in the future (a wrong camera clock) is therefore *not* unsettled forever.
   - **A directory error:** every known entry under that folder is stamped as seen, so it can't be removed, and the error is recorded in `file_scan_errors`.
   - **A file error:** that entry is stamped as seen, and the error is recorded.
   - **A root error (drive lost):** stop. The root is marked Offline, and nothing is removed.
   - **A stop request:** stop. The outcome is Cancelled, and nothing is removed.
5. **Resolve the root again.** If its identity changed during the scan, mark it Offline and remove nothing.
6. **Empty-root guard:** the scan found 0 files but the root had entries, so the drive is probably unmounted. Mark it Offline and remove nothing.
7. **Removals:** `to_remove` = entries whose `seen_generation` is older than this sync.
   - Above the mass-removal threshold (and the root had entries): **hold** the removals. The report sets `removals_held`, and the root stores `held_removals`.
   - Otherwise, delete them → **removed**.
   - `apply_held_removals(root)` deletes held removals later, once the user confirms.
8. **Finish:** delete older `file_scan_errors`, and update the root's `generation`, status (Online), `last_sync_ns`, and `file_count`. The outcome is Synced.

`sync(category)` runs step 1–8 for each root of the category (or all roots), in order of id, one at a time. It checks the stop token between roots, and reports `root_index` and `root_count` in its progress.

### 4.7 Mount points (`src/file/platform/`)

- `std::vector<std::filesystem::path> list_mount_points()`, one file per OS, chosen by CMake:
  - **Linux** (`mount_points_linux.cpp`): reads `/proc/self/mountinfo` and decodes its octal escapes (`\040` is a space).
  - **Windows** (`mount_points_windows.cpp`): uses `GetLogicalDriveStringsW` (`C:\`, `E:\`, …). Verified by hand.
  - **Other systems** (`mount_points_other.cpp`): returns nothing. Automatic relocation is then unavailable, while manual relocation and everything else still work. macOS gets a real implementation in phase 4.
- Tests inject their own lister (`MountPointLister`), so relocation is tested with plain folders.

### 4.8 Edge cases (phase 2)

| Case | Expected behavior |
|---|---|
| Root folder missing (drive unplugged) | Offline, entries kept, nothing removed |
| A different drive at the old path (marker UUID differs) | Mount points searched. Offline unless our marker is found elsewhere |
| Drive letter changed (our marker found at `M/path_in_volume`) | Relocated: path updated, all entries keep their ids |
| Empty mount-point folder left by an unmounted drive (no marker) | Offline, nothing removed |
| Marker-less root becomes empty | Empty-root guard: Offline, nothing removed |
| Drive unplugged mid-sync | Root error: what was seen is kept, nothing removed, Offline |
| Unreadable subfolder | Its known entries are kept. The error is recorded and visible in `scan_errors()` |
| More than 25% of entries gone | Removals held, reported, and applied only by `apply_held_removals` |
| A file still being copied | Stored unsettled. Cleared, and counted as modified, on a later sync |
| A file with a future mtime | Not unsettled once it's beyond the settle window |
| Case-only rename on a case-insensitive root | Same row (same id), with `relative_path` updated |
| A stray `.txt` in Music | Not added: the category's kinds filter it |
| Category changed on a root | The next sync applies the new kinds: files of kinds no longer accepted are removed (subject to the guard) |
| Sync cancelled | Cancelled: committed batches stay, nothing removed, generation not advanced |
| Root removed during a sync on another connection | The worker's inserts fail on the foreign key, and that root reports Failed. Data stays consistent |
| Adding a root inside another root, or around one | `Overlap` |
| Adding a folder that's already a root under another path | `AlreadyExists` (same marker UUID) |
| Re-adding a folder after a lost database | Its marker UUID is adopted |
| Read-only root (e.g. NTFS on macOS) | Added with `use_marker = false` |
| Settings value corrupted in the database | The default is used for that field |
| Database from a newer build (unknown setting keys) | Ignored |
| `resolve()` of a file on an unplugged drive / a deleted file | `RootOffline` / `FileMissing` |

### 4.9 Tests and performance

- **`db`:** open in memory and on disk (WAL), exec and prepare errors, binding and reading every type, transaction commit and rollback (RAII), migrations (applied once, versioned per component, a failed migration rolls back), and a WAL reader seeing committed data while another connection writes.
- **`file::Library`:**
  - settings round trip (defaults, overrides, corrupt values, unknown keys)
  - categories CRUD, `InUse`
  - roots: add, the marker written or adopted, `AlreadyExists`, `Overlap`, read-only fallback, case detection
  - sync: added, modified, unchanged, removed; unsettled and future mtimes; the category kind filter
  - every row in §4.8 that can be produced with plain folders, including relocation through an injected mount lister
  - `resolve()`, paging, `changed_entries`, `scan_errors`
- **`master::SyncWorker`:** see `engines/master.md`.
- **Benchmark:** a first sync of 50k files (all inserts), and an incremental sync of 50k unchanged files. **Target:** incremental under 1 s warm (ARCHITECTURE §7).

**Results as built** (Release build, this Linux VM, warm cache, database file in WAL mode):

| Benchmark | Result | Notes |
|---|---|---|
| Scan 50k files | **317 ms** | Phase 1 scanner with one `stat` per media file |
| First sync, 50k files (all inserts) | **≈ 900 ms** | |
| Incremental sync, 50k unchanged | **727 ms** | Target < 1 s. Was 888 ms before the single-statement fast path and the one-`stat` change |
| Peak memory, sync of 5k vs 50k files (`hoardor_sync`, Debug build) | **6.4 MB vs 7.7 MB** | The difference is SQLite's page cache filling up to its fixed 2 MB limit. Flat |

**Scaling to 500k files** (2026-10-01, `HOARDOR_BENCH_FILES=500000`, same machine and build, run back to back with 50k):

| Benchmark | 50k files | 500k files | Per file at 500k |
|---|---|---|---|
| Scan | 385 ms | 4.09 s | ≈ 8 µs |
| First sync (all inserts) | 1.05 s | 8.67 s | ≈ 17 µs |
| Incremental sync (nothing changed) | 683 ms | 7.26 s | ≈ 15 µs |
| Peak memory of the benchmark process | 8,456 KB | 8,576 KB | flat (+120 KB for 10× the files) |

- **Time is linear** in the number of files, and memory doesn't grow with them.
- **A 500k-file library** takes about 7 s to sync incrementally with a warm cache. On a cold HDD expect more, bounded by seeks. That's why Sync runs in the background (§4.10).
- The 50k numbers vary by about ±15% between runs on this VM (scan: 317–409 ms).

- **Profile** (`perf`, incremental sync): about 70% of the time is in the kernel (`stat` path walks), about 16% in hoardor, and SQLite's VM about 3%.
- **Cache size:** a larger SQLite page cache (8 or 16 MB) made no measurable difference, so the default is kept.

**As-built notes** (where the implementation refined the design above):
- **Fast path for unchanged files:** one conditional `UPDATE ... WHERE path_key = ? AND size = ? AND mtime_ns = ? AND kind = ? AND relative_path = ? AND unsettled = ?`. If it changes no row, the file takes the full `SELECT` plus `INSERT`/`UPDATE` path.
- **Case-insensitive collisions:** an `INSERT` hitting the `(root_id, path_key)` unique key is recorded as a scan error ("another file has the same name ignoring case"), and the sync continues.
- **Progress** is reported at the start of each root, every `progress_interval_files` files (always right after a commit), and at the end of the scan.
- **Default categories** are seeded by migration 1. Like any category, they can be renamed, edited, or deleted.
- **Settings** gained the sync fields in the same struct (`file::Settings`).
- **Tests run as `nobody`:** the permission-dependent tests (unreadable folder, read-only folder) are skipped as root, so they were also run as user `nobody` (copying the test binary), where they pass.

### 4.10 Background sync

Every sync, whether at startup or from the Sync button, runs off the UI thread. The app opens instantly on the last known library state, and a sync updates it while the user browses and plays.

**Threading model (Decided, 2026-10-01): hoardor owns the sync thread, through `master`.** There are two layers:

1. **`file::sync()` is a plain blocking function.** It runs on whichever thread calls it and starts no threads itself. It takes a `std::stop_token` and a progress callback. `std::stop_token` is the C++20 standard "please stop" signal: the thread's owner requests a stop, and Sync checks it between roots and batches. This layer is what the file engine's tests exercise: deterministic and single-threaded.
2. **The `master` engine runs syncs in the background on a thread hoardor owns.** TYLI (or a future daemon or CLI) calls `master::SyncWorker::request_sync(category)`, which returns immediately. `master` owns:
   - the sync worker thread and its lifetime (start, stop, join on shutdown)
   - one sync at a time per root, and ignoring duplicate requests
   - `cancel()` for a running sync, which the app calls before a configuration change. Data integrity doesn't depend on it (row 10)
   - the sync thread's I/O priority and throttling while playback uses the same drive (§6, OI-1)
   - in phase 4, one worker per volume

   TYLI only receives progress and completion callbacks.

Why hoardor and not the caller owns the thread (this reverses the earlier proposal that the caller owns it):
- **The rules that keep the app snappy and the data safe are hoardor's domain knowledge.** These are rows 5, 7, and 10 below, and parallelism per volume. If the caller owned the thread, TYLI and any future daemon would each have to re-implement them, and could get them wrong.
- **Only hoardor knows the volumes.** Per-volume concurrency (phase 4) can't be done by a caller that sees only `sync(category)`.
- **I/O priority belongs to the thread doing the I/O.** Changing the priority of a thread borrowed from the caller, and restoring it afterwards, is fragile.
- **hoardor already plans to own a thread:** `db`'s single writer thread (ARCHITECTURE §3). So owning threads isn't a new kind of responsibility.
- **What's kept from the earlier proposal:** the engine function stays blocking and thread-agnostic, so it remains easy to test. Threading lives in one layer (`master`), not spread across engines.

What it costs:
- `master` needs a first slice in phase 2: a worker thread, a small queue of pending sync requests, and shutdown handling. Its design is in `engines/master.md`.
- If that worker and queue turn out to be generic, they become the first `core` component. Under the on-demand rule (ARCHITECTURE §2), background sync is the concrete feature that justifies it.
- **Callback contract:** callbacks run on hoardor's worker thread. They must return quickly and must not call blocking hoardor operations, because that could deadlock. TYLI's bridge passes them to the UI thread.

**Consequences:**

| # | Consequence | What the design does about it |
|---|---|---|
| 1 | **Startup time no longer depends on library size or drive speed.** The UI shows the database state immediately | The intended benefit. Nothing in the startup path may touch a media drive |
| 2 | **The UI shows a slightly stale library** for the seconds until sync finishes. A file deleted since the last run is still listed | The player handles "file missing" gracefully when `resolve()` or opening fails: it shows a message and can sync that root. It never crashes or hangs |
| 3 | **The library changes under the user's eyes.** Rows appear, disappear, and change while a list is open | TYLI's list models must handle inserts and removals while they're displayed. The `SyncReport` and progress callback tell TYLI when to refresh |
| 4 | **Reads and writes overlap in the database.** Browsing reads while Sync writes | SQLite WAL mode lets readers continue during a write, and each reader sees a consistent snapshot. Readers and the writer use separate connections (ARCHITECTURE §3) |
| 5 | **Sync's writes can delay the user's own writes** (play counts, ratings), because SQLite allows one writer at a time | Sync commits in **short batches** (target: each transaction under about 50 ms) so a user write waits at most that long. The batch size is tuned by benchmark |
| 6 | **Callbacks arrive on hoardor's sync thread, not the UI thread** | Callbacks must be quick, thread-safe, and must not call blocking hoardor operations. TYLI's bridge passes them to the UI thread (ARCHITECTURE §1). This is the first cross-thread hand-off, and it's done with a callback, not an event bus |
| 7 | **Disk contention.** A sync and playback on the same HDD compete for the disk head. This includes pressing Sync while watching a movie | **Important open item (§6, OI-1).** To be measured during benchmarking and testing |
| 8 | **Drives still spin up at startup.** Background doesn't mean silent: every attached HDD in a synced category wakes, which takes 5–10 s and makes noise | This is why `sync_on_startup` defaults to off |
| 9 | **Closing the app mid-sync** | `master` requests a stop on its worker and joins it. The stop token cancels between batches. Already committed batches are correct, and no removals happen, because `engines/file.md` §2.4 requires a complete scan. The next sync finishes the job. Exit can still be delayed by a single `stat` stuck on a drive that's spinning up |
| 10 | **The user acts during a sync:** presses Sync again, or edits the configuration (removes a root, changes its category) | A duplicate Sync (same scope already queued or running) is ignored by `SyncWorker::request_sync`. For configuration changes, the app may call `SyncWorker::cancel()` first. Even without that, data stays consistent: every write is in a transaction, and removing a root cascades its entries and makes the worker's next insert for it fail on the foreign key, so that root reports Failed. *(Simplified on 2026-10-01 from "master cancels automatically", which would require routing every config change through `master`.)* |
| 11 | **More background work later.** New files from a sync will feed tag reading (metadata engines), which opens files and is far heavier than `stat` | This is out of scope here. It is likely the first real need for a background job queue in `core`, and it's decided when the metadata engines are designed |
| 12 | **Memory and CPU** | Unchanged. Memory stays flat, and a stat-only sync is I/O-bound, not CPU-bound |


## 5. Decisions (2026-10-01)

1. **Root marker file:** yes. hoardor writes `.hoardor-root` into each root (opt-out per root, skipped on read-only storage).
2. **Marker name:** `.hoardor-root`. TYLI is the product, but hoardor must not know TYLI exists (ARCHITECTURE §1). The dependency points one way: TYLI knows it uses hoardor, never the other way round.
3. **Sync at startup:** a setting (`sync_on_startup`, default off). When on, it runs **in the background**, so startup never waits for it. Consequences are in §4.10.
4. **Mass-removal guard:** 25% of a root's entries by default, configurable.


## 5a. Decisions awaiting the user's code review

The user asked for both phases to be built without stopping, so these phase 2 choices were made during implementation. Confirm or change them when reviewing the code before the merge:

1. **One database connection per thread** instead of a dedicated writer thread. Background writes commit in short batches (2000 rows or 50 ms).
2. **`master` doesn't intercept configuration changes during a sync.** The app may call `SyncWorker::cancel()` first, and transactions plus foreign keys keep the data consistent either way.
3. **Four default categories** (Music, Movies, Shows, Books) are seeded by migration 1.
4. **"Unsettled" uses a symmetric window** (`|mtime − now| < settle window`), so files with future timestamps don't stay unsettled forever.
5. **A folder the user really emptied stays Offline** and keeps its entries (the empty-root guard), instead of being wiped.

Also pending: `build-release/` was committed by mistake in `4ceea17` and `df7a156`. Either squash-merge the PR, or rewrite the branch and force-push (only with the user's go-ahead).

## 6. Open items to revisit

### OI-1: Playback stutter during a sync (IMPORTANT, revisit during benchmarking and testing)

**Question:** if a movie is playing and the user presses Sync, can playback stutter, and by how much?

**When it can happen:** only when **all** of these are true:
- The movie and at least one root being synced are on the **same spinning HDD**. Different drives don't compete: one USB 3 port (5 Gbit/s) carries far more than a movie plus a scan.
- The sync is **cold**, meaning the OS hasn't cached the folder listings. A warm sync barely touches the disk.
- The movie's bitrate is high relative to what the drive can deliver while it's interleaving.

On an SSD, RAID, or a NAS over gigabit Ethernet, the expected effect is none, because there are no seeks or the bandwidth is plentiful.

**Back-of-envelope estimate (unmeasured, for a portable 2.5" USB HDD):**
- **Drive:** about 100–130 MB/s sequential, and about 12–15 ms per random access.
- **Bitrates:**
  - a typical 1080p encode is about 1 MB/s (≈ 8 Mbit/s)
  - a 1080p Blu-ray remux is about 3–5 MB/s
  - a 4K UHD remux peaks around 13–16 MB/s
- **Sync's load:** a cold sync issues random metadata reads back-to-back, so it can take up to about half the disk's time.
- **With 1 MB player reads:** each player read pays one seek. That leaves about 30 MB/s for the player: enough for all of the above, though a 4K remux has little headroom.
- **With small player reads (64 KiB):** the player gets only about 2–3 MB/s. A 1080p remux would starve, and a 4K remux would certainly stutter.
- **How bad it gets:** stutter lasts at most as long as the cold sync, from seconds to tens of seconds per drive, and only if the player's read-ahead buffer runs dry first.

**Expected outcome:**

| Content | Expected effect |
|---|---|
| Audio | No effect |
| Typical compressed video | No effect if the player reads in large chunks with a few seconds of read-ahead |
| High-bitrate remux, cold sync, same HDD | Real risk, with a small read-ahead buffer |

**Levers, cheapest first:**
1. **Player read-ahead.** The player reads in large chunks (≥ 1 MB) and keeps a few seconds buffered: about 5 MB for 1080p, about 80 MB for 10 s of 4K remux. It's a player design decision, weighed against the low-RAM rule.
2. **Yield to playback (portable):** `master` knows what's playing and from which root, so it can pause or throttle a sync of roots on the same volume until playback stops or pauses. This needs no platform code.
3. **Lower I/O priority** for hoardor's sync thread (a platform interface):
   - Linux: `ioprio_set`. Effective with the BFQ scheduler, largely ignored by `mq-deadline`.
   - Windows: background thread mode.
   - macOS: `setiopolicy_np`.

**How to measure (phase 2 testing):**
- Play a 4K remux and a 1080p encode from the external HDD.
- Drop the OS cache, or power-cycle the drive, then sync the category on the same drive.
- Record dropped frames and buffer underruns from the player, and the sync duration.
- Repeat with each lever.
- Record the results in the changelog and close or adjust this item.
