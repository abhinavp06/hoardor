# hoardor Architecture

This is a living document. It records system-wide decisions and why they were made. Each engine's long-lived reference lives in `engines/<engine>.md`, and each feature's plan lives in `features/<feature>.md`.

Status markers: **Decided** means agreed with the user. **Proposed** means it still needs validation, for example by benchmarks.

## 1. hoardor vs TYLI (Decided, 2026-10-01)

| | hoardor (this repo) | TYLI (separate repo) |
|---|---|---|
| Role | Core library: scanning, storage, metadata, decoding, recommendations | The application: windows, QML views, player controls, visualizer |
| Depends on | C++23 standard library, SQLite, and domain libraries added as needed | Qt / QML and hoardor |
| Qt | **Never** | Thin adapter layer: QObject wrappers, list models over hoardor queries, and a bridge that hands hoardor events to the UI thread |

Rationale:
- hoardor stays headless. Later it can run as a daemon or CLI on a home server for local AI analysis, without Qt.
- Tests are simpler and faster, memory use is lower, and builds are quicker.
- The ownership boundary is clear for future agents, one per engine.
- Qt's offerings at this layer are weak or unnecessary anyway. `QFileSystemWatcher` doesn't scale to large recursive trees, and raw `sqlite3` beats `QSqlDatabase` on both control and speed.

## 2. Engines (Decided)

hoardor is divided into engines, each in its own namespace with its own rules:

- `file`: library roots and their identity, on-demand scanning, change detection, and resolving files to paths.
- `audio`, `video`: metadata (built in `v0.2.0`). `player`: playback through libmpv (designed 2026-10-03, `engines/player.md`). `graphics`, `text`: planned.
- `master`: orchestrates the other engines.
- `db`: **infrastructure, not a feature engine**. See §3.
- `core`: **infrastructure, not a feature engine**. Shared building blocks such as an event bus, ring buffers, and queues. Plain standard C++ with no third-party dependencies. See below.

**Communication:** engines never call each other directly. The master engine coordinates them. Until an event bus exists, an engine hands its results to the caller with plain return values, pull-style iterators, or callbacks, and `master` passes them on to whichever engines care. Once an event bus exists (in `core`), engines publish events (for example `file::FileAdded`) and `master` routes them. Either way, engines stay independently testable, which fits the planned agent-per-engine setup that will be designed after the first draft.

**`core` is built on demand (Decided, 2026-10-01).** Infrastructure is added to `hoardor::core` only when a concrete feature needs it, and that feature is its first consumer. Nothing goes into `core` speculatively. Each addition gets a decision-log row naming the feature that required it. With on-demand scanning, no planned feature needs an event bus yet, so none is assumed.

**Anticipated `core` components.** These are *candidates*, not commitments. Each is built only when the named feature actually needs it, and gets a decision-log row then.

| Candidate | What it is | The feature that would trigger it | Likelihood |
|---|---|---|---|
| Background worker + job queue | A hoardor-owned thread that runs queued jobs, with stop and join on shutdown | Background Sync in `master` (file engine phase 2). Later, tag reading after a sync | High, and probably first |
| Logging sink | A log function hoardor calls, with the host app (TYLI) deciding where the output goes | Background work that has to report problems nobody is waiting for | High |
| Small shared helpers (UTF-8 path conversion, ASCII case-folding, file-time conversion) | Free functions, not classes | A second engine needing the same helper. Until then they stay private inside the file engine | Medium |
| Lock-free ring buffer (single producer, single consumer) | A fixed-size buffer passing samples between threads without locks | Audio playback (decoder thread → audio output callback), visualizer | High, with the `audio` engine |
| Thread pool | A few workers for parallel jobs | Per-volume parallel scans (file phase 4), parallel tag reading | Medium |
| Event bus / dispatcher | Publish and subscribe between engines | Several engines reacting to the same change once callbacks routed by `master` stop being enough | Low for now |
| Fast non-cryptographic hash | For partial fingerprints | File phase 3 (moves and renames), if another engine also needs hashing | Low |
| Object pool / arena | Reuse of memory for hot paths | Only if a benchmark shows allocation cost in audio or video | Low |

Things that look like infrastructure but **don't** belong in `core`:
- **SQLite and settings persistence** go in `db` (mechanics) and each engine's repository.
- **OS-specific code** (mount points, I/O priority, volume IDs) goes in each engine's `platform/` folder.
- **Error and result types** come from the standard library (`std::expected`, `std::error_code`).

## 3. Persistence (Decided)

- **One SQLite database file** for the whole library. Cross-engine joins (a track joined to its file) and atomic multi-engine transactions stay possible.
- **`hoardor::db` owns the mechanics:** connections, the pragmas (WAL mode), prepared statements, transactions, and running migrations. **One connection per thread** (changed on 2026-10-01 from "a single writer thread"): WAL lets readers run during a write, writers use `BEGIN IMMEDIATE` with a `busy_timeout`, and background writes stay in short batches. That gives the same behavior as a writer thread without a queue inside `db`. Details are in `engines/db.md`.
- **Each engine owns its own data:** its tables (prefixed `<engine>_`, e.g. `file_*`), its migrations, and its repository class (e.g. `file::Repository`) built on `db`. A central "DB engine" that implements every engine's repository was rejected. It would become a god-module and break per-engine ownership.
- **Writes are batched in transactions.** One transaction per batch is orders of magnitude faster than autocommit on every row. Background batches stay short (about 50 ms), so the user's own writes (play counts, ratings) are never blocked noticeably.
- **The database lives on the machine's internal storage** (the app-data folder), never on a media drive. Media drives come and go, and SQLite's WAL mode doesn't work on network file systems.
- **Tests run against real in-memory SQLite (`:memory:`).** Don't abstract repositories behind interfaces until a second implementation exists.

## 3a. Settings (Decided, 2026-10-01)

- **Start as configurable as possible, and prune later.** Anything a user might tune is a setting, not a constant. Settings that real use shows nobody touches can be removed later.
- **Each engine has one plain `Settings` struct** (e.g. `file::Settings`) holding all of its tunables, with defaults in code (`Settings::defaults()`). It has no getters, setters, or settings classes: it's plain data that the engine reads.
- **Settings are persisted in SQLite** in the engine's own tables (the `<engine>_` prefix, §3), through the engine's repository. This arrives with the first DB work (file engine phase 2). Until then, code that will load or save settings carries a comment saying so. No mocks.
- **A value missing from the database falls back to the code default.** That way, new settings added in later versions just work with older databases.
- **Configuration that is user data, not tuning,** gets its own tables designed with the engine: categories, library roots, and their per-root options.

## 4. Memory (Decided)

The library lives on disk in SQLite. hoardor exposes paged and streaming queries, and RAM use must not grow with library size. Avoid caches unless a benchmark justifies them.

## 5. Platforms (Decided)

- Targets are Windows (the user's main OS), Linux, and macOS. Development happens on a Linux VM, so Linux backends are built and tested first, then Windows, then macOS.
- OS-specific code sits behind an interface in `src/<engine>/platform/`.
- CI (GitHub Actions) will build and test on all three OSes, because only Linux can run locally. It is deferred for now (decision log, 2026-10-01).
- Portability rules:
  - Store paths as UTF-8 everywhere.
  - Windows and macOS file systems are usually case-insensitive.
  - Windows needs long-path (>260 characters) support.
  - Volumes come and go.

## 6. File engine principles (Decided unless marked; details in `engines/file.md` and `features/file_sync.md`)

- **Scanning is on demand, through Sync (Decided, 2026-10-01).** The user configures categories (Music, Movies, …) and assigns directories (roots) to each. A **Sync** of one category, which is a UI section, scans all of that category's roots. A global Sync scans every root. There are no folder-level scans. A startup sync is an opt-in setting. Every sync runs in the background, so startup never waits for it. A sync on drive plug-in comes later. Realtime watching is deferred. If it's ever added, it is only a hint that triggers a sync.
- **The scan is the truth.** A stat-only scan compares path, size, and mtime against the database.
- **Storage-agnostic roots.** A library root is any folder: an external HDD, RAID, a NAS share, or a local disk. Files are stored relative to their root, so a root's location can change (a new drive letter, or a migration to RAID) without losing entries.
- **Root identity doesn't depend on location.** It comes from a `.hoardor-root` marker file in the root (Decided; opt-out, skipped on read-only storage), a volume ID as fallback, and the last-known path as a hint. Drive letters and mount names change, and a different drive can take over an old letter.
- **An unavailable root is OFFLINE, never "everything deleted".** Removals are applied only after a complete scan of a verified root. An empty root that has known entries counts as unmounted. A mass-removal guard holds large removals for confirmation.
- **Browsing never touches media drives.** The library is browsed from SQLite, so sleeping or unplugged drives are never spun up just to show the UI.
- **hoardor never writes to media storage**, apart from the optional root marker.
- **Files being copied are not trusted.** A file modified within a configurable settle window is stored as unsettled, and metadata engines skip it until a later scan.
- **Parallelism is per physical device, not per CPU core.** Concurrent reads on a spinning HDD thrash the disk head. Scan concurrency is configurable per volume: 1 for an HDD, more for an SSD, RAID, or NAS.
- **Never hash whole files.** A partial fingerprint (size plus the first and last N KiB) is used only to detect moves and renames.
- **Category is configuration on a library root**, not a fixed list in code. Examples are Music, Podcast, Movie, Show, Book, and Blog. The file engine only classifies files by *kind* (audio, video, text, image, subtitle) through a configurable map of file extensions.

## 7. Testing and performance (Decided)

- Tests use GoogleTest, fetched with CMake `FetchContent` and discovered by `gtest_discover_tests`, so they run through `ctest` and Visual Studio's Test Explorer.
- Google Benchmark (`BUILD_BENCHMARKS=ON`, Release build) covers the hot paths: the scan, and first and incremental syncs.
- Proposed targets, to be validated by benchmarks. The current library has thousands of files: about 1 TB of music and about 5 TB of other media.
  - Incremental reconciliation of about 50k files: under 1 s with a warm OS cache, a few seconds on a cold HDD.
  - Memory stays flat regardless of library size.
- **Measured (2026-10-01, File Sync v1, this Linux VM, warm cache, Release):**
  - a scan of 50k files in 317 ms
  - an incremental sync of 50k files in 727 ms
  - a first sync of 50k files in ≈900 ms
  - sync peak memory of 6.4 MB at 5k files and 7.7 MB at 50k
  - not measured yet: a cold HDD, which needs the user's drive and `hoardor_sync`
  - **at 500k files:** a scan in 4.1 s, an incremental sync in 7.3 s, a first sync in 8.7 s; benchmark-process peak memory of 8,576 KB vs 8,456 KB at 50k. Time is linear and memory is flat (`HOARDOR_BENCH_FILES=500000`)
- **Sanitizers:** the suite also runs clean under AddressSanitizer and ThreadSanitizer. The steps are in `CLAUDE.md` ("Build and test").

## Decision log

| Date | Decision |
|---|---|
| 2026-10-01 | hoardor stays a pure C++23 library with no Qt. Qt and QML live only in TYLI. |
| 2026-10-01 | `db` is an infrastructure layer. Engines own their repositories and their prefixed tables, all in one database file. |
| 2026-10-01 | Native watchers on each OS, with a reconciliation scan as the source of truth. Unavailable roots are marked offline. |
| 2026-10-01 | Tests use GoogleTest. Benchmarks will use Google Benchmark. |
| 2026-10-01 | Media category is configuration on a library root. Text has subcategories (Books, Blogs, …). |
| 2026-10-01 | A `core` engine (`hoardor::core`) holds shared infrastructure (event bus, ring buffers, …). Each component is added only when a feature needs it. Until an event bus exists, engines return results to the caller and `master` passes them on. |
| 2026-10-01 | `db` stays its own infrastructure namespace (`hoardor::db`) and does not move under `core`. `core` stays plain standard C++ with no dependencies, while `db` brings in SQLite. |
| 2026-10-01 | CI is deferred. Windows and macOS behavior is verified by hand until it is added. |
| 2026-10-01 | Scanning is on demand (startup, user request, later drive arrival). Realtime watching is deferred. This supersedes the earlier "native watchers on each OS" decision. The reconciliation scan remains the source of truth. |
| 2026-10-01 | Library roots are storage-agnostic (external HDD, RAID, NAS). Files are stored relative to their root, root identity doesn't depend on location, and offline-safety rules guard removals. The marker-file identity is still Proposed. |
| 2026-10-01 | The SQLite database lives on internal storage, never on a media drive. |
| 2026-10-01 | The user-facing scan action is **Sync**: `sync(category)` for one UI section, `sync()` for everything. A category can have several roots. Folder-level scans are dropped as unnecessary. |
| 2026-10-01 | Root identity uses a `.hoardor-root` marker file. The name stays hoardor's, because hoardor must not depend on or know about TYLI. |
| 2026-10-01 | Sync always runs in the background. A startup sync is a setting (default off). *Proposed, pending the user's review:* engine calls like `sync()` stay blocking, and the caller (TYLI, or a future daemon) owns the thread. hoardor owns no threads for now. |
| 2026-10-01 | The mass-removal guard defaults to 25% of a root's entries and is configurable. |
| 2026-10-01 | Settings: start as configurable as possible. One plain `Settings` struct per engine, defaults in code, persisted in SQLite (from file engine phase 2), with code defaults filling missing values. No mocks before the DB exists. |
| 2026-10-01 | Docs split into `engines/<engine>.md` (long-lived engine reference) and `features/<feature>.md` (per-feature plan, may span engines). `engines/file_engine.md` became `engines/file.md` plus `features/file_sync.md`. |
| 2026-10-01 | Branching: one feature branch is one shippable feature and one PR, built in phases that are each designed first. File scanner v1 = file engine phases 1 and 2 (SQLite, DB design, roots, background Sync) on `abhinavp06/FILE_SCANNER_INIT`. |
| 2026-10-01 | File Sync phase 2 design: `db` uses one connection per thread (no writer thread) with WAL, `BEGIN IMMEDIATE`, and `busy_timeout`. The SQLite 3.46.1 amalgamation is fetched with FetchContent. `master::SyncWorker` owns the background sync thread. See `engines/db.md`, `engines/master.md`, `features/file_sync.md` §4. |
| 2026-10-01 | A hot-path query (size + mtime) gets a platform backend (`file_info`: one POSIX `stat`, cached values on Windows) after `perf` showed 70% of sync time in kernel path walks from libstdc++'s two `stat` calls per file. |
| 2026-10-01 | Keep it simple: plain structs and free functions first. A class only when something holds state or invariants. An abstraction only when a second implementation exists. |
| 2026-10-01 | *Proposed, pending the user's review, and replacing the caller-owns-the-thread proposal above:* hoardor owns background execution. Engine functions like `file::sync()` stay blocking and thread-agnostic. The `master` engine runs them on a hoardor-owned worker and enforces one sync per root, cancellation, I/O priority, and yielding to playback. Callbacks arrive on hoardor's thread. |
| 2026-10-01 | The file engine starts with scanning only (phase 1: streaming scanner and configurable extension-to-kind map). Roots and persistence, move detection, and platform volume support follow in later phases. See `engines/file_engine.md`. |
| 2026-10-02 | Order of work after File Sync v1 (`v0.1.0`): media listing (every media type browsable from file and folder names), then a first player draft (audio and video), then metadata (tags, cover art, chapters), built on the player's decoder where possible. Tag reading no longer lands on `abhinavp06/FILE_SCANNER_INIT`, which merges as `v0.1.0`. Details: `engines/file.md` §4.0. |
| 2026-10-02 | **Order changed again (the user, reviewing the media-listing mockups):** names must come from metadata, so metadata moves into `abhinavp06/MEDIA_LISTING` ("Media library v1", `features/media_listing.md`), before the player. This supersedes the row above for the metadata/player order. |
| 2026-10-02 | **ffmpeg** (libavformat, libavcodec, libavutil; LGPL, linked as shared libraries) is hoardor's metadata reader, and later its decoder. TagLib was rejected as a second dependency. |
| 2026-10-02 | **Movie and show info comes from offline sources only:** embedded tags, `.nfo` sidecar files, images next to the files, and names as a last resort (flagged). No online lookup. |
| 2026-10-02 | **hoardor's query APIs are generic:** filter, order, group, and count by fields, with keyset cursors. Layouts (albums, artist or genre pages) and grouping copies of the same album or movie in different qualities belong to TYLI. hoardor never groups copies. |
| 2026-10-02 | New engines `audio` and `video` own track and movie/episode metadata (`audio_*`, `video_*`). `master` reads metadata right after each root sync, while the drive is awake. The paging types (`Cursor`, `Page`) are `core`'s first component, triggered by these two engines. |
| 2026-10-02 | `DATABASE.md` is the single reference for every table, column, index, and migration. Feature docs propose schema changes, and `DATABASE.md` is updated when they're built. |
| 2026-10-02 | Sync and metadata reading run **one worker per physical drive** (`file::device_of`: Linux block device, Windows disk number), up to `parallel_devices` (default 4), each on its own SQLite connection. This implements §6's "parallelism per physical device". Several workers per SSD are not done yet. ThreadSanitizer runs with `tests/tsan.supp` (SQLite's lock-free WAL index). |
| 2026-10-03 | No Books category by default until text has its own build (the user). File migration 3 deletes the seeded one only when untouched (no folders, default kinds). Text stays a supported file kind, and a Books category can be added by hand. |
| 2026-10-03 | **The player runs on libmpv** (the user's choice over Qt Multimedia in TYLI, and over our own ffmpeg + miniaudio), wrapped by a new engine `hoardor::player` with no Qt. Video frames go through mpv's OpenGL render API to the app. The queue holds entry ids, resolved one at a time through an injected resolver. *Proposed, pending approval of `features/player.md`:* the rest of the design. |

