# hoardor Architecture

This is a living document. It records system-wide decisions and why they were made. Each engine's detailed design lives in `engines/<engine>.md`.

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

- `file`: library roots, scanning, realtime watching, and change detection.
- `audio`, `video`, `graphics`, `text`: planned.
- `master`: orchestrates the other engines.
- `db`: **infrastructure, not a feature engine**. See §3.

**Communication:** engines never call each other directly. An engine publishes events (for example `file::FileAdded`), and the master engine routes them to whichever engines care. This keeps engines independently testable and fits the planned agent-per-engine setup, which will be designed after the first draft.

## 3. Persistence (Decided)

- **One SQLite database file** for the whole library. Cross-engine joins (a track joined to its file) and atomic multi-engine transactions stay possible.
- **`hoardor::db` owns the mechanics:** connections, the pragmas (WAL mode), a single writer thread plus reader connections, prepared-statement caching, transactions, and running migrations.
- **Each engine owns its own data:** its tables (prefixed `<engine>_`, e.g. `file_*`), its migrations, and its repository class (e.g. `file::Repository`) built on `db`. A central "DB engine" that implements every engine's repository was rejected. It would become a god-module and break per-engine ownership.
- **Writes are batched in transactions.** One transaction per batch is orders of magnitude faster than autocommit on every row.
- **Tests run against real in-memory SQLite (`:memory:`).** Don't abstract repositories behind interfaces until a second implementation exists.

## 4. Memory (Decided)

The library lives on disk in SQLite. hoardor exposes paged and streaming queries, and RAM use must not grow with library size. Avoid caches unless a benchmark justifies them.

## 5. Platforms (Decided)

- Targets are Windows (the user's main OS), Linux, and macOS. Development happens on a Linux VM, so Linux backends are built and tested first, then Windows, then macOS.
- OS-specific code sits behind an interface in `src/<engine>/platform/`.
- CI (GitHub Actions) builds and tests on all three OSes, because only Linux can run locally.
- Portability rules:
  - Store paths as UTF-8 everywhere.
  - Windows and macOS file systems are usually case-insensitive.
  - Windows needs long-path (>260 characters) support.
  - Volumes come and go.

## 6. File engine principles (Decided; details in `engines/file_engine.md`)

- **Each OS gets its own native watcher:** `inotify` on Linux, `ReadDirectoryChangesW` on Windows, and `FSEvents` on macOS.
- **The watcher is a hint, and the reconciliation scan is the truth.** Every OS mechanism can drop events (queue or buffer overflow, coalescing). A stat-only scan compares path, size, and mtime against the database. It runs on startup, after any lost events, on an interval, and on demand.
- **An unavailable root is OFFLINE, never "everything deleted".** The library sits on external drives. Identify roots by volume ID as well as path, because drive letters change.
- **Files must settle before they are reported**, so a copy in progress isn't treated as a series of modifications.
- **Parallelism is per physical device, not per CPU core.** Concurrent reads on a spinning HDD thrash the disk head.
- **Never hash whole files.** A partial fingerprint (size plus the first and last N KiB) is used only to detect moves and renames.
- **Category is configuration on a library root**, not a fixed list in code. Examples are Music, Podcast, Movie, Show, Book, and Blog. The file engine only classifies files by *kind* (audio, video, text, image, subtitle) through a configurable map of file extensions.

## 7. Testing and performance (Decided)

- Tests use GoogleTest, fetched with CMake `FetchContent` and discovered by `gtest_discover_tests`, so they run through `ctest` and Visual Studio's Test Explorer.
- Google Benchmark will be added for hot paths.
- Proposed targets, to be validated by benchmarks. The current library has thousands of files: about 1 TB of music and about 5 TB of other media.
  - Incremental reconciliation of about 50k files: under 1 s with a warm OS cache, a few seconds on a cold HDD.
  - Change on disk to database row, after the file settles: under about 200 ms.
  - Memory stays flat regardless of library size.

## Decision log

| Date | Decision |
|---|---|
| 2026-10-01 | hoardor stays a pure C++23 library with no Qt. Qt and QML live only in TYLI. |
| 2026-10-01 | `db` is an infrastructure layer. Engines own their repositories and their prefixed tables, all in one database file. |
| 2026-10-01 | Native watchers on each OS, with a reconciliation scan as the source of truth. Unavailable roots are marked offline. |
| 2026-10-01 | Tests use GoogleTest. Benchmarks will use Google Benchmark. |
| 2026-10-01 | Media category is configuration on a library root. Text has subcategories (Books, Blogs, …). |
