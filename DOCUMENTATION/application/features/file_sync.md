# Feature: File Sync (v1)

| | |
|---|---|
| Status | **In progress**: approved by the user on 2026-10-01. Phase 1 is built. Phase 2 is designed in §4 and being built |
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
  - **On Linux** each *media* file costs two `stat` calls, because libstdc++'s `file_size()` and `last_write_time()` each stat. The second is served from the inode cache, so it never adds disk I/O. A single platform `stat` could replace them if a benchmark ever needs it.
  - **On Windows**, the directory listing already carries size and mtime.
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
- **Result (Release build, this Linux VM, warm cache): 427 ms for 50k files (≈117k files/s)**, within the target.
  - The first version took 709 ms. It used `symlink_status()`, which does an uncached `lstat` per entry in libstdc++.
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


## 4. Phase 2: Roots, persistence, reconciliation (outline)

Part of file scanner v1. To be detailed here before implementation, after phase 1 is built. That covers the full DB design: `hoardor::db` mechanics, the `file_*` schema, settings persistence, and migrations. It also covers the first slice of `master` (background sync worker), in its own `engines/master_engine.md`.

- **Categories:** configured by the user, each with a name and the file kinds it accepts. The defaults are Music: audio and image (cover art); Movies and Shows: video, subtitle, and image; Books: text and image. Sync passes each root a `Settings` copy whose `extension_kinds` is filtered to its category's kinds, so a stray `.txt` in the Music folder is ignored. Phase 1's `Scanner` doesn't change.
- **Library roots:** UUID, display name, category, last-known path, path within its volume, marker on/off, case-sensitive yes/no (detected read-only by looking up an existing name with its case flipped), status (online/offline), and last scan's file count (the progress estimate).
- **Root resolution and relocation** as in `engines/file.md` §2.3. Needs a small platform interface, `list_mount_points()`: `/proc/self/mountinfo` on Linux, `GetLogicalDriveStringsW` on Windows (Windows verified by hand).
- **Tables:** `file_roots` and `file_entries` (root, relative path, size, mtime, kind, unsettled flag, the generation of the last scan that saw it). Unique on (root, relative path), with case folding for case-insensitive roots.
- **Sync API.** This is the call behind the UI's Sync button:

  ```cpp
  SyncReport sync();                    // every root (a global Sync)
  SyncReport sync(CategoryId category); // every root of one category (a section's Sync)
  ```

  How the layers fit together:
  - **The category** decides only *which roots* take part.
  - **Each root** then goes through the same steps: resolve it (`engines/file.md` §2.3), scan it with phase 1's `Scanner`, and reconcile the result.
  - **Switching the UI** from per-section Sync to a global Sync is just a matter of which overload TYLI calls.
  - **Speed:** a whole-category sync is the only granularity. It's fast enough that adding one album doesn't need anything finer: a stat-only pass over about 50k files takes under 1 s warm, and seconds on a cold HDD.
- **For each root, Sync:**
  - streams the scanner and upserts rows in batched transactions
  - removes unseen rows only under the safety rules in `engines/file.md` §2.4
  - returns a report to the caller: per root, added, modified, removed, unsettled, and errors, plus which roots were skipped as offline. There is no event bus.
  - reports progress (root N of M, plus the scanner's counters) and can be cancelled between roots and between batches
  - if a sync is requested while one is already running, it doesn't start a second one for the same roots
- **Unsettled files:** a file whose mtime is within a configurable settle window (default 10 s) of the scan is probably still being copied. It is stored with `unsettled = true`, and metadata engines skip it until a later scan clears the flag. The caller (`master`) may run a follow-up sync after the window.
- **Player lookup:** `resolve(file_id)` returns the absolute path, or "offline".
- **Database location:** the machine's internal app-data folder, never a media drive (ARCHITECTURE §3).
- **Settings persistence:** `file::Settings` is saved in SQLite (the table design is part of phase 2's DB design), and it gains the sync settings: `sync_on_startup` (default off), the settle window, and the mass-removal guard threshold (default 25%).
- **Dependencies:** this phase brings in SQLite and the minimal `hoardor::db` the file engine needs.

### 4.1 Background sync

Every sync, whether at startup or from the Sync button, runs off the UI thread. The app opens instantly on the last known library state, and a sync updates it while the user browses and plays.

**Threading model (Proposed): hoardor owns the sync thread, through `master`.** There are two layers:

1. **`file::sync()` is a plain blocking function.** It runs on whichever thread calls it and starts no threads itself. It takes a `std::stop_token` and a progress callback. `std::stop_token` is the C++20 standard "please stop" signal: the thread's owner requests a stop, and Sync checks it between roots and batches. This layer is what the file engine's tests exercise: deterministic and single-threaded.
2. **The `master` engine runs syncs in the background on a thread hoardor owns.** TYLI (or a future daemon or CLI) calls something like `master.request_sync(category)` and returns immediately. `master` owns:
   - the sync worker thread and its lifetime (start, stop, join on shutdown)
   - one sync at a time per root, and ignoring duplicate requests
   - cancelling a running sync when the configuration changes
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
- `master` needs a first slice in phase 2: a worker thread, a small queue of pending sync requests, and shutdown handling. It gets its own design doc (`engines/master_engine.md`) when phase 2 is detailed.
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
| 10 | **The user acts during a sync:** presses Sync again, or edits the configuration (removes a root, changes its category) | A duplicate Sync is ignored (§4). A configuration change cancels the running sync for affected roots before applying. `master` enforces both |
| 11 | **More background work later.** New files from a sync will feed tag reading (metadata engines), which opens files and is far heavier than `stat` | This is out of scope here. It is likely the first real need for a background job queue in `core`, and it's decided when the metadata engines are designed |
| 12 | **Memory and CPU** | Unchanged. Memory stays flat, and a stat-only sync is I/O-bound, not CPU-bound |


## 5. Decisions (2026-10-01)

1. **Root marker file:** yes. hoardor writes `.hoardor-root` into each root (opt-out per root, skipped on read-only storage).
2. **Marker name:** `.hoardor-root`. TYLI is the product, but hoardor must not know TYLI exists (ARCHITECTURE §1). The dependency points one way: TYLI knows it uses hoardor, never the other way round.
3. **Sync at startup:** a setting (`sync_on_startup`, default off). When on, it runs **in the background**, so startup never waits for it. Consequences are in §4.1.
4. **Mass-removal guard:** 25% of a root's entries by default, configurable.


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
