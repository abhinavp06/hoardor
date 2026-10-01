# Unreleased

Work that is done but not yet part of a version. The newest entries come first. When a version is cut, these entries move unchanged into `v<version>.md`, and this file is emptied (see `README.md`).

### 500k-file benchmark, code tree, and untracked build output (2026-10-01, branch `abhinavp06/FILE_SCANNER_INIT`)

**Summary:**
- **Benchmarks:** the library size is now configurable, and they were run at 500k files. Time scales linearly and memory stays flat.
- **Code tree:** added `DOCUMENTATION/application/CODE_TREE.md`, one tree of the whole repository, and made keeping it current part of the workflow.
- **Cleanup:** stopped tracking `build-release/`, which was committed by mistake.

**Added**
- `benchmarks/support/bench_tree.hpp`:
  - `hoardor::bench::file_count()`, from the `HOARDOR_BENCH_FILES` environment variable (default 50,000)
  - `library_tree()`, one generated tree shared by every benchmark
- `DOCUMENTATION/application/CODE_TREE.md`:
  - every file, namespace, type, member and free function (public, private, and internal), test, and build target, with a one-line purpose
  - a Sync call-flow diagram

**Changed**
- **Benchmarks:** `scanner_benchmark.cpp` and `sync_benchmark.cpp` use the shared tree. They're renamed `BM_Scan`, `BM_FirstSync`, and `BM_IncrementalSync`, since the size is no longer fixed. `BM_FirstSync` now runs 2 iterations instead of 3, to keep 500k runs reasonable.
- **`CLAUDE.md`:** `CODE_TREE.md` is now step 5 of the session reading order, part of the layout, a Definition-of-done item, and a living-documentation rule.
- **Results:** `features/file_sync.md` §4.9 has a 50k vs 500k table, and `ARCHITECTURE.md` §7 has the 500k results.

**Fixed**
- `build-release/` (353 files) had been committed in `4ceea17` and updated in `df7a156`. `git add -A` picked it up before `.gitignore` covered `build*/`. It's untracked now and ignored, but it remains in those two commits. Rewriting the pushed history would need a force-push; that's up to the user (a squash merge of the PR would also keep it out of `master`).

**Performance** (Release build, warm cache, this VM, runs back to back)

| | 50k | 500k |
|---|---|---|
| Scan | 385 ms | 4.09 s |
| First sync | 1.05 s | 8.67 s |
| Incremental sync | 683 ms | 7.26 s |
| Peak memory (benchmark process) | 8,456 KB | 8,576 KB |

**Known limitations / follow-ups**
- Generating the 500k tree takes about a minute of the 85 s run.
- The incremental sync at 500k (7.3 s warm) is fine in the background. If libraries that size become real, per-volume parallel scans (phase 4) and skipping unchanged directories are the next levers.
- Stage files explicitly instead of `git add -A` while build folders are around.

### File Sync v1, phase 2: SQLite, library roots, background Sync (2026-10-01, branch `abhinavp06/FILE_SCANNER_INIT`)

**Summary:** Implemented phase 2 of File Sync (`features/file_sync.md` §4), completing **file scanner v1**:
- **Storage:** SQLite arrives (`hoardor::db`), and the file engine gets its tables and settings persistence.
- **Configuration:** the user configures categories and folders (roots).
- **Sync:** pressing Sync reconciles the disk with the database under the offline-safety rules, in the background on a hoardor-owned thread (`master::SyncWorker`).

The design was written into the docs first (commit `f0fac91`), then built in 7 commits. 109 tests pass, including as a non-root user, and the suite is clean under AddressSanitizer and ThreadSanitizer.

**Added**
- **`hoardor::db`** (`include/hoardor/db/database.hpp`, `src/db/database.cpp`):
  - `Database`: WAL, `synchronous=NORMAL`, foreign keys, `busy_timeout`; file or in-memory
  - `Statement` (RAII)
  - `Transaction`: `BEGIN IMMEDIATE`, rolling back on destruction
  - `migrate()`: versions per component, each migration in a transaction
  - `sqlite3.h` is hidden from public headers
- **SQLite 3.46.1 amalgamation** (`third_party/CMakeLists.txt`): fetched with `FetchContent`, SHA3-256 pinned, built as the static library `hoardor_sqlite3` with `SQLITE_DQS=0` and no extension loading.
- **`file::Library`** (`include/hoardor/file/library.hpp`, `src/file/library.cpp`, `src/file/library_sync.cpp`):
  - settings load and save
  - categories CRUD
  - roots: add, remove, change category, case sensitivity, manual relocation
  - `sync(category | all)`, `sync_root`, `apply_held_removals`
  - paged `entries` and `changed_entries`, `scan_errors`
  - `resolve()` for playback
- **Schema** (file migration 1): `file_settings`, `file_categories` (seeded with Music, Movies, Shows, Books), `file_roots`, `file_entries` (`path_key`, `seen_generation`, `changed_generation`, `unsettled`), and `file_scan_errors`.
- **New `file::Settings` fields:** `sync_on_startup` (false), `settle_window_seconds` (10), `mass_removal_threshold_percent` (25), `batch_max_rows` / `batch_max_milliseconds` (2000 / 50), `progress_interval_files` (500), `relocation_sample_size` / `relocation_min_match_percent` (20 / 80).
- **Root marker** `.hoardor-root` (`src/file/root_marker.*`): a text file holding a v4 UUID, read back after writing to verify it.
- **Platform backends** (`src/file/platform/`, chosen by CMake):
  - `list_mount_points()`: Linux `/proc/self/mountinfo`, Windows drive letters, other OSes empty for now
  - `file_info()`: one POSIX `stat`, or the values Windows already cached
- **`master::SyncWorker`** (`include/hoardor/master/sync_worker.hpp`, `src/master/sync_worker.cpp`):
  - its own connection, one `std::jthread`, and a queue of scopes
  - duplicate requests ignored
  - `cancel()`, `idle()`, `wait_idle()`
  - `sync_on_startup`
  - a destructor that stops and joins promptly
- **Benchmarks:** `benchmarks/file/sync_benchmark.cpp` (first sync and incremental sync of 50k files).
- **Playground:** `hoardor_sync <db> <category> <folder>`, for real drives.
- **Docs:** `engines/db.md` and `engines/master.md` (new), `features/file_sync.md` §4 (detailed design and as-built results), `engines/file.md` §3 (current state).

**Changed**
- **Scanner:** size and mtime now come from the one-call platform `file_info()` instead of libstdc++'s `file_size()` plus `last_write_time()`. The scan went from 427 to 317 ms.
- **ARCHITECTURE §3:** one database connection per thread replaces "a single writer thread".
- **ARCHITECTURE §7:** the measured results.
- **`CLAUDE.md`:** the layout (`benchmarks/`, `third_party/`), the `BUILD_BENCHMARKS` option, and how to run the sanitizers and the non-root test pass.
- **`.gitignore`:** `/build*/`.

**Design decisions and trade-offs**
- **One connection per thread, not a writer thread:** WAL plus `BEGIN IMMEDIATE` plus `busy_timeout` plus short batches gives the same behavior with no queue machinery in `db`. The worker's batches commit every 2000 rows or 50 ms, and always before a callback, so the UI's writes never wait long. A test checks that the UI connection can read and write mid-sync in under 1 s.
- **Mark and sweep with generations:** every file a sync sees is stamped with the new generation, and only a *complete*, *verified* sync deletes rows with older stamps. This makes "offline is never deleted" mechanical: cancelled, lost, offline, identity-changed, and empty-root syncs simply never reach the sweep.
- **Unreadable folders are protected** by stamping their known subtree as seen with a key range (`[dir/, dir0)`, since `'0'` follows `'/'` in byte order). That's one statement, with no list of errors held in memory.
- **No lists in reports:** `RootSyncReport` holds counts and a generation, and consumers page through `changed_entries`. Other engines will reference `file_entries(id)` with `ON DELETE CASCADE`, so removals propagate without a removed-id list.
- **`path_key`** (ASCII-lowercased on case-insensitive roots) keeps ids across case-only renames on NTFS and exFAT. Making a root case-insensitive is refused if its names would collide.
- **Unsettled uses a symmetric window** (`|mtime − now| < window`), so files with future timestamps aren't unsettled forever. A file that settles counts as *modified*, so metadata engines pick it up.
- **Config changes during a sync:** `master` doesn't intercept them. Integrity comes from transactions and foreign keys, and the app can call `SyncWorker::cancel()` first. This simplifies the earlier "master cancels automatically" (documented in `features/file_sync.md` §4.10, row 10).
- **Performance work driven by measurement** (`perf`):
  - 70% of the time was kernel path walks, which led to the one-`stat` backend.
  - An unchanged file now costs one conditional `UPDATE` instead of a `SELECT` plus an `UPDATE`.
  - A larger SQLite cache was measured, made no difference, and was rejected.
- **Benchmark settle window = 0:** the generated tree is seconds old, so with the default window its files would count as still being copied.

**Files**
- New: `include/hoardor/db/database.hpp`, `src/db/database.cpp`, `third_party/CMakeLists.txt`, `include/hoardor/file/library.hpp`, `include/hoardor/file/mount_points.hpp`, `src/file/library.cpp`, `src/file/library_sync.cpp`, `src/file/library_internal.hpp`, `src/file/root_marker.hpp`, `src/file/root_marker.cpp`, `src/file/platform/file_info.hpp`, `src/file/platform/file_info_posix.cpp`, `src/file/platform/file_info_windows.cpp`, `src/file/platform/mount_points_linux.cpp`, `src/file/platform/mount_points_windows.cpp`, `src/file/platform/mount_points_other.cpp`, `include/hoardor/master/sync_worker.hpp`, `src/master/sync_worker.cpp`, `tests/db/database_test.cpp`, `tests/file/library_test.cpp`, `tests/file/sync_test.cpp`, `tests/master/sync_worker_test.cpp`, `tests/support/library_fixture.hpp`, `benchmarks/file/sync_benchmark.cpp`, `playground/file/sync_playground.cpp`, `DOCUMENTATION/application/engines/db.md`, `DOCUMENTATION/application/engines/master.md`.
- Changed: `CMakeLists.txt`, `tests/CMakeLists.txt`, `benchmarks/CMakeLists.txt`, `playground/CMakeLists.txt`, `include/hoardor/file/settings.hpp`, `src/file/text.hpp`, `src/file/text.cpp`, `src/file/scanner.cpp`, `.gitignore`, `CLAUDE.md`, `DOCUMENTATION/application/ARCHITECTURE.md`, `DOCUMENTATION/application/features/file_sync.md`, `DOCUMENTATION/application/engines/file.md`, `DOCUMENTATION/CHANGELOGS/README.md`.

**Tests:** 109 in total, 58 new in phase 2: `db` 12, `LibraryTest` 20, `RootMarker` 1, `SyncTest` 29, `SyncWorkerTest` 8. Plus one phase 1 test fixed.
- **`db`:** foreign keys on, WAL on files, an uncreatable path, SQL errors, every column type (including UTF-8 and an embedded NUL), reuse, bind errors, commit, rollback by destructor, migrations (once, per component, a failure rolled back), and a WAL reader during a write.
- **Library:**
  - seeded categories, reopening
  - settings: defaults, a round trip of every field, corrupt or out-of-range values, unknown keys, an empty list
  - categories: CRUD, unique ignoring case, blank names, `InUse`
  - roots:
    - add: normalized path, marker, name, `path_in_volume`, a whole volume
    - bad input
    - independent roots on different drives and categories
    - overlap: same, nested, parent, and a common-prefix sibling allowed
    - `AlreadyExists` through a copied marker, marker adoption, `use_marker = false`, read-only fallback
    - category change, removal keeping the marker
  - malformed markers
- **Sync:**
  - kind filter by category, unchanged, modified (keeps the id), removed
  - mass removal held and applied, the threshold as a setting
  - missing root offline then back
  - a different drive at the old path
  - relocation through mount points (ids kept)
  - an emptied marker-less root, an empty mount point
  - the drive lost mid-sync (root error recorded, generation not advanced)
  - cancel mid-sync, stop before start
  - unsettled then settled, future mtime
  - category change applying kinds
  - case-only rename keeping the id, case-insensitive refused on collisions
  - a root removed with its entries
  - section vs global sync with progress positions
  - one offline root not stopping others
  - batch size 1
  - paging
  - `resolve` (OK, `FileMissing`, `RootOffline`, `NotFound`)
  - manual relocation: with a marker, another root's marker refused, sample check pass and fail
  - an unreadable subfolder keeping its entries
- **SyncWorker:** a background run visible on another connection, duplicates ignored, cancel, startup sync on and off, reads and writes mid-sync, prompt shutdown, a bad database path.
- **Quality runs:**
  - all 109 pass as root, and as user `nobody` (so the 3 permission tests run)
  - the worker tests pass 50 repeated runs
  - AddressSanitizer is clean; it found a bug in a test's string-literal length, now fixed
  - ThreadSanitizer is clean (run with ASLR disabled through `setarch -R`)

**Performance** (Release build, this Linux VM, warm cache; target: incremental sync of 50k under 1 s)

| Measurement | Result |
|---|---|
| Scan, 50k files | 317 ms |
| First sync, 50k files | ≈900 ms |
| Incremental sync, 50k files | 727 ms |
| Peak memory, sync of 5k files | 6.4 MB |
| Peak memory, sync of 50k files | 7.7 MB (flat; the growth is SQLite's fixed 2 MB cache filling up) |

**Known limitations / follow-ups**
- **Windows is untested** (CI deferred). Check by hand on Windows:
  - `mount_points_windows.cpp` and `file_info_windows.cpp`
  - junctions
  - long paths
  - UTF-16 conversion
  - case detection on NTFS
- **macOS:** no mount-point listing yet (phase 4), so automatic relocation is unavailable there.
- **Cold-HDD numbers:** not measured yet. Run `hoardor_scan` / `hoardor_sync` on the external drive after a power cycle.
- **OI-1 (playback stutter during a sync) is still open.** I/O priority and yielding to playback aren't implemented.
- **Case folding is ASCII-only:** `path_key` and overlap checks don't fold non-ASCII letters (e.g. `Ä` vs `ä`).
- **Empty-root guard:** a root the user really emptied stays Offline (its entries are kept) until it's removed or gets files again.
- **Relocation without a marker** uses a random sample, so very small libraries are checked against fewer files.

### File Sync v1, phase 1: streaming scanner and settings (2026-10-01, branch `abhinavp06/FILE_SCANNER_INIT`)

**Summary:** Implemented phase 1 of File Sync (`features/file_sync.md` §3). A streaming, flat-memory `Scanner` walks one root and yields media files or errors one at a time. Every tunable lives in `file::Settings`. Phase 1 also brings the build and test wiring (GoogleTest, an optional Google Benchmark target, tests running after each build) and a playground tool for real drives. The naive `discover()` skeleton is gone. The user approved the design on 2026-10-01.

**Added**
- `file::Settings` (a plain struct; defaults in code; persisted from phase 2) with `extension_kinds`, `ignored_names`, and `ignored_prefixes`. The defaults are the design doc's tables.
- `file::FileKind` with stable numeric values (Audio=1…Subtitle=5) for the database, `to_string`, `file_kind_from_string`, and `kind_of()`.
- `file::Scanner`: `open(root, settings)`, `next()` → `std::optional<std::expected<ScannedFile, ScanError>>`, `progress()`.
- Internal helpers:
  - `src/file/text.*`: UTF-16 and byte paths become valid UTF-8 with `/`, never throwing, flagging and repairing invalid input with U+FFFD. Also ASCII case-folding and extension normalization.
  - `src/file/file_time.hpp`: an exact `file_time_type` ↔ Unix-nanosecond conversion via `clock_cast`.
- Build:
  - GoogleTest 1.15.2 via `FetchContent`, with `gtest_discover_tests`
  - `RUN_TESTS_AFTER_BUILD` wired as a `run_tests` target in the default build
  - a `BUILD_BENCHMARKS` option (default OFF) with Google Benchmark 1.9.0
  - compiler warnings on (`-Wall -Wextra -Wpedantic`; `/W4 /utf-8` on MSVC)
- `benchmarks/file/scanner_benchmark.cpp`, `playground/file/scan_playground.cpp` (`hoardor_scan <dir>`), `tests/support/temp_dir.hpp`.

**Removed**
- `include/hoardor/file/file_engine.hpp` and `src/file/file_engine.cpp` (`discover()`, `MediaType`, `FileType`, `File`, `FileEntry`).

**Design decisions and trade-offs**
- **A custom directory stack instead of `recursive_directory_iterator`:** in libstdc++, the first error on any subdirectory ends the entire recursive iteration. That would turn one unreadable folder into "the rest of the drive is unknown". With its own stack of `directory_iterator`s, the scanner keeps errors local and still uses memory proportional to depth.
- **Cached entry types instead of `symlink_status()`:**
  - The type checks are free (`d_type`), whereas `symlink_status()` costs an `lstat` per entry in libstdc++: benchmark 709 → 427 ms.
  - *Trade-off:* Windows junction handling depends on how MSVC reports junctions, so it needs a manual check.
- **A custom UTF-8 converter instead of `u8string()`:**
  - MSVC throws on unpaired surrogates, and libstdc++ passes invalid bytes through.
  - The converter never throws and lets the scanner report invalid names as errors, so every emitted path is valid UTF-8.
  - The UTF-16 path is unit-tested on Linux through `utf8_from_utf16`.
- **Root reachability is probed by opening the root, not with `stat`:** a cached `stat` of a dead mount can still succeed.
- **Folders deleted mid-scan are skipped silently, not reported:** reporting them would protect their stale entries from removal, but they really are gone.
- **The phase 2 sync settings aren't in `Settings` yet:** they arrive with the phase that uses them.

**Files**
- New: `include/hoardor/file/settings.hpp`, `include/hoardor/file/scanner.hpp`, `src/file/settings.cpp`, `src/file/scanner.cpp`, `src/file/text.hpp`, `src/file/text.cpp`, `src/file/file_time.hpp`, `tests/file/settings_test.cpp`, `tests/file/text_test.cpp`, `tests/file/scanner_test.cpp`, `tests/support/temp_dir.hpp`, `benchmarks/CMakeLists.txt`, `benchmarks/file/scanner_benchmark.cpp`.
- Changed: `CMakeLists.txt`, `tests/CMakeLists.txt`, `playground/CMakeLists.txt`, `playground/file/scan_playground.cpp` (renamed from `file_engine_playground.cpp` and rewritten).
- Removed: `include/hoardor/file/file_engine.hpp`, `src/file/file_engine.cpp`.
- Docs: `features/file_sync.md` (status, §3.3 behavior as built, §3.8 results, §3.9 limitations), `engines/file.md` (current state).

**Tests** (39, run with `ctest --test-dir build`; all pass as root, and also as user `nobody`, so the permission test runs)
- **Settings and `kind_of`:**
  - every default kind and the litter defaults
  - extension case, user keys with a dot or uppercase letters
  - an edited map (removed `ts`, remapped `txt`, added `nfo`)
  - no extension, a dot-only name (`.mp3`), multiple dots, a trailing dot
  - empty settings, kind-name round trip
- **Text helpers:**
  - extension normalization, ASCII-only lowercasing
  - valid UTF-8 (CJK, emoji, NFD) kept byte for byte
  - invalid bytes repaired: Latin-1, a lone continuation byte, an overlong sequence, an encoded surrogate, a truncated sequence
  - UTF-16 surrogate pairs, lone high and low surrogates
  - `/` separators
- **Scanner:**
  - a missing root, a root that's a file, an empty root (and staying finished)
  - nested relative paths
  - directories and non-media skipped, including a directory named `empty.mp3`
  - every kind, exact size, exact mtime (ns), the same mtime across scans
  - a root with a trailing separator
  - non-ASCII names (CJK, emoji, NFC and NFD) round-trip to the same file
  - 40-level nesting with a path over 260 characters
  - every default ignored name as a directory (not descended), `._` and `.Trash-` prefixes, case-insensitive `@EADIR`, an ignored file name
  - custom and empty settings
  - symlinks to a file and a directory, and a symlink cycle
  - an unreadable subdirectory (an error, then the scan continues)
  - the root removed mid-scan (exactly one root error, last)
  - files vanishing mid-scan (no errors)
  - the scanner destroyed mid-scan
  - progress counters
  - case-only-different names (both emitted)
  - a FIFO (POSIX)
  - invalid UTF-8 file and directory names (POSIX)

**Performance**
- **Speed:** a 50k-file tree, Release build, warm cache, on this VM: **427 ms** (≈117k files/s). The target is < 1 s.
- **Memory:** peak RSS of `hoardor_scan` is **4,224 KB for both 5k and 50k files**, so memory is flat.
- **Cold external HDD:** not measured yet. It needs the user's drive and the playground.

**Known limitations / follow-ups**
- Verify on Windows: junctions, long paths, the UTF-16 conversion, and the mtime epoch.
- Measure a cold-HDD scan with `hoardor_scan` on the real external drive.
- On Linux, each media file takes two `stat` calls (`file_size` and `last_write_time`). The second is cache-served, so it's acceptable for now.

### Design docs split into engine references and feature plans (2026-10-01, branch `abhinavp06/FILE_SCANNER_INIT`)

**Summary:** On the user's question of whether the doc should be `file_engine.md` or `file_sync.md`, the design docs were split in two. `engines/<engine>.md` holds the long-lived reference for each engine. `features/<feature>.md` holds the plan for each feature, which may span engines. Documentation only. No design content changed. Text was moved and cross-references were renumbered.

**Added**
- `DOCUMENTATION/application/features/`, with `file_sync.md` (moved with `git mv` from `engines/file_engine.md`):
  - a header table: status, branch, version, engines involved
  - 1 Overview (what the user gets, what's out of scope)
  - 2 Phases (1 and 2)
  - 3 Phase 1 (formerly §4)
  - 4 Phase 2 and 4.1 Background sync (formerly §5)
  - 5 Decisions (formerly §10)
  - 6 Open items, including OI-1 (formerly §11)
- `DOCUMENTATION/application/engines/file.md`:
  - 1 Purpose, consumers, non-goals
  - 2 Storage model (2.1–2.5, unchanged)
  - 3 Current state (a placeholder until v1 ships)
  - 4 Roadmap: the phase table, with phase 3, phase 4, and deferred realtime watching as 4.1–4.3
  - 5 Relationship to `core`

**Changed**
- `CLAUDE.md`: session-start step 4, the repository layout, workflow rule 1 (design before code), the Definition of done, and Living documentation all cover both doc types.
- `DOCUMENTATION/application/ARCHITECTURE.md`: the intro, the §6 heading, and a decision-log row. Older decision-log rows and changelog entries still name `engines/file_engine.md`. They were correct at the time and aren't rewritten.

**Design decisions and trade-offs**
- **Engine reference plus feature plans** (the user picked this out of three options):
  - *Why:* features span engines (File Sync touches `file`, `db`, and `master`), and appending every feature to its engine's doc would grow giant files, which the user explicitly doesn't want.
  - *Engine docs* stay small and current.
  - *Feature docs* keep the full reasoning, and record which version they shipped in.
- *Rejected:* one feature doc only. Engine facts would scatter across features.
- *Rejected:* one engine doc only. It grows without bound.
- **`engines/file.md`, not `file_engine.md`:** matches the existing `engines/<engine>.md` pattern. The suffix was redundant inside `engines/`.
- **Phase numbers stay engine-wide** (1–4): File Sync v1 is phases 1–2, and phases 3–4 are future features on the engine roadmap. Existing references to "phase 3" and "phase 4" stay valid.

**Files**
- `DOCUMENTATION/application/features/file_sync.md` (moved from `engines/file_engine.md` and restructured), `DOCUMENTATION/application/engines/file.md` (new), `CLAUDE.md`, `DOCUMENTATION/application/ARCHITECTURE.md`, `DOCUMENTATION/CHANGELOGS/unreleased.md`.

### Scope of file scanner v1: phases 1 and 2 on one branch (2026-10-01, branch `abhinavp06/FILE_SCANNER_INIT`)

**Summary:** The user decided that this branch ships the full scanning feature: phase 1 (the streaming scanner and settings) and phase 2 (SQLite, DB design, library roots, background Sync, and settings persistence). They go in one PR, released as `v0.1.0`. Documentation only.

**Changed**
- `CLAUDE.md` workflow rule 2: "one phase is one PR" becomes "one feature branch is one shippable feature and one PR, built in phases that are each designed first".
- `DOCUMENTATION/application/engines/file_engine.md`:
  - the status line defines file scanner v1
  - the §3 phase plan reflects the branch policy
  - the §5 phase 2 outline notes it's part of this branch, and what its detailed design must cover: `db` mechanics, the `file_*` schema, settings persistence, migrations, and `engines/master_engine.md` for the background worker
- `DOCUMENTATION/application/ARCHITECTURE.md`: a decision-log row for the branching rule and the v1 scope.
- `DOCUMENTATION/CHANGELOGS/README.md`: `v0.1.0` = documentation plus file scanner v1 (phases 1 and 2).

**Design decisions and trade-offs**
- **One feature per PR over one phase per PR** (the user's call): the user wants to ship something usable end to end. The PR is larger, but each phase is still designed, tested, and documented on its own, so the review can follow it phase by phase.
- **Phase 2 is designed after phase 1 is built,** not now. This keeps "design before code" without front-loading the DB design before the scanner it consumes exists.

**Files**
- `CLAUDE.md`, `DOCUMENTATION/application/engines/file_engine.md`, `DOCUMENTATION/application/ARCHITECTURE.md`, `DOCUMENTATION/CHANGELOGS/README.md`, `DOCUMENTATION/CHANGELOGS/unreleased.md`.

**Known limitations / follow-ups**
- The user reviews `file_engine.md` again before phase 1 starts.
- Write the phase 2 detailed design (DB, schema, settings persistence, `master_engine.md`) after phase 1 is built, and get it approved before coding phase 2.

### Branch renamed to `abhinavp06/FILE_SCANNER_INIT`; settings and simplicity principles (2026-10-01, branch `abhinavp06/FILE_SCANNER_INIT`)

**Summary:**
- The work so far was committed on `abhinavp06/file_engine_init` (`e5a7f20`). Development continues on the new feature branch `abhinavp06/FILE_SCANNER_INIT`, which delivers **file scanner v1**.
- Following the user's direction, settings became a first-class design rule from the very first implementation, and "keep it simple" became a non-negotiable.
- Documentation only.

**Changed**
- **Branch references:** every changelog entry and `CHANGELOGS/README.md` now name `abhinavp06/FILE_SCANNER_INIT`, as the user asked. For the record: commits up to and including `e5a7f20` were made on `abhinavp06/file_engine_init`, which still exists locally and on `origin`.
- **`DOCUMENTATION/application/engines/file_engine.md`:**
  - Phase 1 API: `KindMap` (class) and `ScanOptions` are replaced by one plain struct, `file::Settings` (`extension_kinds`, `ignored_names`, `ignored_prefixes`, `defaults()`), plus a free function `kind_of()`. `Scanner::open(root, settings)` lowercases its own copy.
  - "Why so few types" rationale added. Files, tests, defaults, and the phase table updated to match.
  - §2.2: roots are explicitly independent, with no common library folder (e.g. Music on HDD A, Movies on HDD B).
  - New §2.5 "HDD vs SSD": same code, differences only in configuration and performance.
  - Phase 2: settings persistence in SQLite. The sync settings join `file::Settings`.
- **`DOCUMENTATION/application/ARCHITECTURE.md`:**
  - new §3a **Settings**
  - in §2, a table of **anticipated `core` components** (candidates with their triggering features and likelihood) and a list of what does *not* belong in `core`
  - two decision-log rows
- **`CLAUDE.md`:** "Customizable by design" expanded. A new **Keep it simple, no bloat** non-negotiable.

**Design decisions and trade-offs**
- **A plain `Settings` struct per engine over a settings class hierarchy or a key-value registry:**
  - It's the least code, it's type-safe, and it's trivially testable.
  - Phase 2 maps fields to SQLite rows.
  - *Rejected:* a generic typed registry. It's more machinery than one engine needs. It can be revisited if many engines end up duplicating save and load code.
- **No mocks for the DB in phase 1:** the scanner never touches the database, so there's nothing to mock. The only DB-bound piece is settings persistence, which is a comment until phase 2. Introducing SQLite on this branch was considered (the user allowed it if mocks proved costly) and isn't needed for phase 1.
- **`kind_of` is a free function, not a `KindMap` class:** it's a pure lookup over settings data, so a class adds nothing.

**Files**
- `DOCUMENTATION/application/engines/file_engine.md`, `DOCUMENTATION/application/ARCHITECTURE.md`, `CLAUDE.md`, `DOCUMENTATION/CHANGELOGS/README.md`, `DOCUMENTATION/CHANGELOGS/unreleased.md`.

**Known limitations / follow-ups**
- Open: should phase 2 (SQLite, DB design, roots, Sync) also land on this branch, or in its own branch/PR?
- The user reviews code after phase 1 is implemented.

### Changelog split into per-version files (2026-10-01, branch `abhinavp06/FILE_SCANNER_INIT`)

**Summary:** Replaced the single `DOCUMENTATION/CHANGELOG.md` with a `DOCUMENTATION/CHANGELOGS/` folder: one file per version, an `unreleased.md` for work in progress, and a README with the index, versioning rules, release steps, and the entry template. The user didn't want one giant file. No entry text was changed.

**Added**
- `DOCUMENTATION/CHANGELOGS/README.md`: what each file is for, a **Versions** index table, versioning rules, the "Cutting a version" procedure, the changelog rules, and the entry template (moved from the top of the old file). The versioning rules are SemVer, `0.MINOR.PATCH` before 1.0, MINOR for each engine phase or feature, PATCH for fixes and docs, the version kept in the root `CMakeLists.txt`, and the first version `v0.1.0` cut when this branch merges.
- `DOCUMENTATION/CHANGELOGS/baseline.md`: the pre-changelog "Baseline" section, moved verbatim.

**Changed**
- `DOCUMENTATION/CHANGELOG.md` → `DOCUMENTATION/CHANGELOGS/unreleased.md` (`git mv`, so git can follow the history). It keeps every `[Unreleased]` entry verbatim. The template and baseline moved out.
- `CLAUDE.md`: session-start step 2, the repository layout, the Definition of done, and Living documentation now point to the new folder and the release procedure.

**Design decisions and trade-offs**
- **Per-version files, plus `unreleased.md`, plus an index README:**
  - *Rejected:* one file per entry. It's too fragmented, and a version's story would be spread across many files.
  - *Rejected:* one file per month. Months don't line up with releases.
  - *Why this won:* per-version files stay bounded and match how releases are discussed. The index gives a one-screen overview.
- **The version is cut at merge time, not now:** nothing has been released yet. The CMake version (0.1.0) already names the first release, so this branch becomes `v0.1.0` when it merges.
- **Moving entries is the only restructuring allowed.** The "never rewrite history" rule still holds. Version files are frozen once cut.

**Files**
- `DOCUMENTATION/CHANGELOGS/README.md` (new), `DOCUMENTATION/CHANGELOGS/baseline.md` (new), `DOCUMENTATION/CHANGELOGS/unreleased.md` (moved from `DOCUMENTATION/CHANGELOG.md`), `CLAUDE.md`.

**Known limitations / follow-ups**
- Cut `v0.1.0` (create `v0.1.0.md` and add an index row) in the PR that merges this branch.

### File engine: hoardor owns background sync; playback-stutter open item (2026-10-01, branch `abhinavp06/FILE_SCANNER_INIT`)

**Summary:** On the user's question, reversed the proposal that the caller owns the sync thread. hoardor now owns background execution through the `master` engine, and engine functions stay blocking. Also recorded "playback stutter during a sync" as an important open item, with an estimate, levers, and a measurement plan. Documentation only.

**Changed**
- `DOCUMENTATION/application/engines/file_engine.md`:
  - §5.1's threading model is rewritten as two layers: a blocking `file::sync()` with `std::stop_token` and a progress callback, run on a hoardor-owned worker in `master`. It includes the rationale, the costs, and the callback contract. Rows 6, 7, 9, and 10 are updated to match.
  - New §11 "Open items to revisit" with **OI-1: playback stutter during a sync**:
    - when it can happen (same HDD, cold sync, high bitrate)
    - a back-of-envelope estimate
    - the expected outcome for each kind of content
    - three levers: player read-ahead, yielding to playback, lower I/O priority
    - a measurement plan for phase 2 testing
- `DOCUMENTATION/application/ARCHITECTURE.md`: a new decision-log row (Proposed) replacing the caller-owns-the-thread proposal.

**Design decisions and trade-offs**
- **hoardor owns the thread** (rejected: the caller owns it):
  - The rules that keep the app snappy and safe (one sync per root, cancellation on config change, I/O priority, yielding to playback, per-volume workers) are hoardor's domain knowledge. With caller-owned threads, every consumer (TYLI, a daemon, a CLI) would have to re-implement them.
  - Only hoardor knows volumes, and I/O priority belongs to the thread doing the I/O.
  - hoardor already plans a db writer thread.
- **Threading lives in one layer (`master`):** engine functions stay single-threaded and deterministic for tests. The cost is a first slice of `master` in phase 2 (worker thread, request queue, shutdown), designed in `engines/master_engine.md` at that point. Background sync becomes the concrete feature that may justify a first `core` component (a worker or job queue).
- **Stutter handled as a measured open item, not a pre-built fix:** the estimate says audio and typical video are unaffected, and high-bitrate remuxes on the same cold HDD are at risk. Levers are chosen after measurement, cheapest first.

**Files**
- `DOCUMENTATION/application/engines/file_engine.md`, `DOCUMENTATION/application/ARCHITECTURE.md`, `DOCUMENTATION/CHANGELOG.md`.

**Known limitations / follow-ups**
- **OI-1 (important):** measure playback during a sync in phase 2 testing (`file_engine.md` §11).
- Write `engines/master_engine.md` when phase 2 is detailed.
- The user's review of `file_engine.md` is still pending before phase 1 starts.

### File engine: open questions resolved, background sync (2026-10-01, branch `abhinavp06/FILE_SCANNER_INIT`)

**Summary:** The user answered all four open design questions. Sync is now defined to always run in the background, and its consequences are documented. Documentation only. `file_engine.md` awaits the user's final review before phase 1 implementation starts.

**Changed**
- `DOCUMENTATION/application/engines/file_engine.md`:
  - §2.3: the marker file is Decided.
  - §5 gains the library settings (`sync_on_startup` default off, settle window, mass-removal threshold) and a new **§5.1 Background sync**: the threading model (blocking `sync()` with a `std::stop_token` and a progress callback, and the caller owns the thread) plus a table of 12 consequences and how each is handled.
  - §10 is replaced by the recorded decisions.
- `DOCUMENTATION/application/ARCHITECTURE.md`:
  - §3: background write batches stay short (about 50 ms).
  - §6: the marker is Decided, and sync runs in the background.
  - The decision log gains three rows: the marker name, the background-sync threading model, and the 25% guard.

**Design decisions and trade-offs**
- **Marker named `.hoardor-root`, not TYLI-branded:** hoardor must stay independent of TYLI. The dependency points from TYLI to hoardor only.
- **Blocking `sync()`, with the caller owning the thread,** over hoardor starting its own threads:
  - It keeps hoardor free of thread management and keeps `core` empty, which follows the build-on-demand rule.
  - TYLI already has worker threads (Qt), and a future daemon would bring its own.
  - Cancellation uses the standard `std::stop_token`, not a custom flag.
- **`sync_on_startup` defaults to off:** running in the background removes the startup delay, but not the drive spin-up noise and the disk contention with playback.
- **Short write batches (about 50 ms):** SQLite allows one writer at a time. Short sync transactions keep the user's own writes (play counts, ratings) responsive. The exact batch size is set by benchmark in phase 2.
- **I/O priority deferred until measured:** lowering the sync thread's disk priority needs platform code. It's added only if the manual test (high-bitrate video playing while its drive syncs) shows stutter.

**Files**
- `DOCUMENTATION/application/engines/file_engine.md`, `DOCUMENTATION/application/ARCHITECTURE.md`, `DOCUMENTATION/CHANGELOG.md`.

**Known limitations / follow-ups**
- The user reviews `file_engine.md`, then phase 1 (scanning) implementation starts.
- Phase 2 must benchmark the sync batch size against the 50 ms target.
- The metadata engines' background work (tag reading) will probably be the first real need for a `core` job queue. That gets decided when those engines are designed.

### File engine: Sync by category (2026-10-01, branch `abhinavp06/FILE_SCANNER_INIT`)

**Summary:** Aligned the scanning design with how the user will actually use the app. Categories and their directories are configured once. A **Sync** button in each UI section syncs that category, and a global Sync syncs everything. Folder-level scans are dropped. Documentation only.

**Changed**
- `DOCUMENTATION/application/engines/file_engine.md`:
  - §1 describes Sync as the scanning model.
  - §2.2 is now "Library configuration and paths": categories are configuration and map to UI sections, a root belongs to one category, and a category can have several roots (for example Music on two HDDs).
  - In the §2.1 sleeping-drive row, syncing a section wakes only that section's drives.
  - The phase 2 outline gains categories that accept certain kinds (with defaults) and the `sync()` / `sync(CategoryId)` API, including how the layers fit, progress, cancellation, offline roots in the report, and no duplicate concurrent syncs. The folder scope and the `scan_on_startup` root field are removed.
  - In phase 4, drive arrival is noted as the first real need for syncing a single root.
  - Open question 3 now recommends startup sync off by default.
- `DOCUMENTATION/application/ARCHITECTURE.md`:
  - §6 has a rewritten "on demand" bullet.
  - §7 drops the folder-rescan target.
  - The decision log gains a row for Sync.

**Design decisions and trade-offs**
- **Category as a filter on roots, not a parameter of the scanner:** phase 1's `Scanner` scans one directory and knows nothing about categories. `sync(category)` only selects which roots to run it over. Per-section and global Sync are therefore the same code, and changing the UI between them means calling a different overload.
- **`CategoryId` (configured) instead of a `MediaType` enum:** this follows the earlier decision that categories are configuration. A new section such as Podcasts or Blogs doesn't need a recompile.
- **No folder-level scans:** the user doesn't want to choose folders. A category-wide stat-only pass is fast enough (about 50k files in under 1 s warm, seconds on a cold HDD) that a finer scope isn't worth its API and testing cost.
- **Per-category kind filter (proposed default):** it keeps unrelated files (a `.txt` in Music, a `.pdf` among movies) out of a section, at no cost to the scanner.

**Files**
- `DOCUMENTATION/application/engines/file_engine.md`, `DOCUMENTATION/application/ARCHITECTURE.md`, `DOCUMENTATION/CHANGELOG.md`.

**Known limitations / follow-ups**
- The user still needs to review `file_engine.md` and answer its §10 questions.

### File engine: on-demand scanning and storage model (2026-10-01, branch `abhinavp06/FILE_SCANNER_INIT`)

**Summary:** Replaced realtime watching with on-demand scanning. Redesigned the file engine so the library can live on external HDDs today and on RAID, a NAS, or a home server later without code changes. Documentation only. The design doc is still Proposed and awaits the user's review.

**Changed**
- `DOCUMENTATION/application/engines/file_engine.md`, mostly rewritten:
  - **Scanning model:** realtime watching moved to a "Deferred" section (§9). Scans run at startup, on request (library, root, or folder), and later on drive arrival.
  - **New §2 "Storage model":**
    - a table of storage scenarios: unplugged drives, drive-letter changes, a different drive taking an old letter, empty mount points, unplugging mid-scan, sleeping drives, multiple HDDs, copies in progress, FAT/exFAT time quirks, read-only NTFS on macOS, migrating to RAID or a NAS, NAS latency
    - path rules: entries relative to the root, no Unicode normalization, no overlapping roots
    - root identity: marker file, then volume ID, then last-known path, with a resolution algorithm
    - safety rules for removals: complete scan, root verified before and after, error subtrees excluded, empty-root guard, mass-removal guard
  - **Phase plan:**
    1. scanning
    2. roots, persistence, reconciliation, with mount-point listing on Linux and Windows
    3. moves and renames, including the whole-hour mtime-shift check
    4. platform volume support: volume IDs, arrival triggers, per-volume concurrency, macOS
  - **Phase 1 API additions:**
    - `Scanner::progress()` (`ScanProgress` counters), because user-started scans need progress
    - a root-lost `ScanError` (empty `relative_path`, always last), so an unplugged drive produces one error instead of thousands
    - names that can't be represented as UTF-8 are reported as `illegal_byte_sequence` errors
  - **Ignore defaults:** expanded to include NAS litter (`@eaDir`, `#recycle`, `#snapshot`, `.snapshot`), `lost+found`, `FOUND.000`, `.DocumentRevisions-V100`, the `.Trash-` prefix, and hoardor's own marker.
  - **Open questions (§10):** the marker file, its name, the `scan_on_startup` default, and the mass-removal threshold.
- `DOCUMENTATION/application/ARCHITECTURE.md`:
  - §2: new description of the `file` engine. Removed the expectation that the event bus arrives with watching.
  - §3: the database lives on internal storage.
  - §6: rewritten for on-demand scanning, storage-agnostic roots, location-independent identity (marker *Proposed*), removal safety, not waking drives while browsing, no writes to media, unsettled files, and configurable per-volume concurrency.
  - §7: replaced the "change to row < 200 ms" target with a folder-rescan target.
  - Decision log: three new rows. The on-demand row explicitly supersedes the earlier "native watchers" row.

**Design decisions and trade-offs**
- **On-demand scanning over realtime watching** (user decision, after weighing the cost):
  - *Same either way:* idle CPU and startup cost, because a startup scan is needed with or without a watcher.
  - *Saved:* about 1 KiB of kernel memory per directory on Linux, a permanent background thread, three platform backends, and a Windows safe-eject problem.
  - *Lost:* changes show up at the next scan instead of within about 200 ms.
  - The scanner and reconciliation are identical in both designs, so watching can be added later without a redesign.
- **Marker file as primary root identity (Proposed):**
  - *Why:* it's the only identity that works the same on every OS, file system, RAID, and NAS, travels with the data during a migration, and tells "another drive got this letter" apart from "the root is empty".
  - *Downside:* it writes one small file to the user's storage. It is therefore opt-out per root and skipped on read-only storage, with volume IDs (phase 4) as the fallback.
  - *Rejected as primary:* volume IDs alone (platform-specific, change on reformat, and don't exist for NAS shares) and path alone (breaks on letter changes, and dangerous when another drive takes the letter).
- **Removal safety rules:** external drives make "the folder is empty" or "this read failed" common, and wrongly deleting entries would lose user data such as play counts and metadata. Several cheap guards are layered rather than relying on one.
- **Browsing from SQLite only:** spinning up a sleeping HDD takes 5–10 s and is noisy. Only scans and playback may touch media drives.
- **Unsettled flag instead of watcher-style settling:** a scan can't wait for a file to finish copying. It records the file and marks it, and a later scan settles it.
- **One root at a time in phase 2:** the safe default for HDDs. Per-volume concurrency waits for phase 4, when volumes can be identified.

**Files**
- `DOCUMENTATION/application/engines/file_engine.md`: rewritten as described above.
- `DOCUMENTATION/application/ARCHITECTURE.md`: §2, §3, §6, §7, and the decision log.
- `DOCUMENTATION/CHANGELOG.md`: this entry.

**Known limitations / follow-ups**
- Waiting on the user's review of `file_engine.md` and the four open questions in its §10.
- A hung NFS hard mount can block a scan thread uninterruptibly. Scans must run off the UI thread.
- Until CI exists, all Windows behavior is verified by hand.

### File engine design (phase 1 scope) and the `core` engine decision (2026-10-01, branch `abhinavp06/FILE_SCANNER_INIT`)

**Summary:** Scoped the file engine around its real consumers and narrowed the first phase to scanning. Recorded `core` as the home for shared infrastructure, built only on demand. No code changed. The design doc is a draft awaiting the user's approval.

**Added**
- `DOCUMENTATION/application/engines/file_engine.md` (status: Proposed). It contains:
  - purpose and consumers: background scanning, database sync, realtime updates, metadata engines, and the player
  - non-goals
  - a five-phase plan
  - a full phase 1 spec: a pull-based streaming `Scanner` returning `std::expected<ScannedFile, ScanError>`, a configurable `KindMap` (extension to `FileKind`), default ignore lists for OS litter on external drives, behavior rules, an edge-case table, the file list, the test plan, and performance targets
  - outlines for phases 2 (persistence and reconciliation) and 3 (realtime watching)

**Changed**
- `DOCUMENTATION/application/ARCHITECTURE.md` §2:
  - added the `core` engine
  - rewrote **Communication**: until an event bus exists, engines return results to the caller and `master` passes them on
  - added the "`core` is built on demand" rule
  - added decision-log rows: the `core` engine, the phase 1 scope, `db` staying separate from `core`, and CI being deferred
  - §5: CI marked as deferred
- `CLAUDE.md` code conventions: the event rule now matches. Added the `core` on-demand rule.

**Design decisions and trade-offs**
- **No event bus yet.** The user's rule is that no infrastructure is added before a feature needs it. Phase 1 has a single consumer (the caller), so plain return values are enough. The likely first need is phase 3, when a watcher thread produces changes that several engines consume.
- **`core` vs putting infrastructure inside each engine:** shared building blocks (event bus, ring buffers) get one owner, so they aren't duplicated per engine. `core` is still infrastructure, like `db`, and not a feature engine.
- **Pull-based `Scanner::next()` over a returned vector, a callback, or `std::generator`:**
  - The vector breaks the flat-memory rule.
  - A callback makes batching into transactions and early stopping awkward.
  - `std::generator` isn't available in GCC 13 or Apple Clang yet.
- **Errors in the stream (`std::expected`) instead of a collected list:** memory stays flat even on a badly broken drive. Phase 2 also knows exactly which subtree is unknown, so it never treats an unreadable folder as deleted.
- **`db` stays separate from `core`** (user decision). `core` stays plain standard C++ with no dependencies, while `db` is the layer that brings in SQLite.
- **CI deferred** (user decision). Google Benchmark stays in phase 1. Windows-specific behavior is verified by hand until CI exists.
- **`MediaType` removed from the file engine:** categories are configuration on a library root (ARCHITECTURE §6). The file engine only classifies by kind.

**Files**
- `DOCUMENTATION/application/engines/file_engine.md`: new.
- `DOCUMENTATION/application/ARCHITECTURE.md`: §2 and the decision log.
- `CLAUDE.md`: code conventions.
- `DOCUMENTATION/CHANGELOG.md`: this entry.

**Known limitations / follow-ups**
- The design doc awaits the user's approval before phase 1 implementation starts.
- Open question: realtime watching vs on-demand scanning only. This affects whether phases 3 and 5 stay in the plan.

### Project documentation and Claude context (2026-10-01, branch `abhinavp06/FILE_SCANNER_INIT`)

**Summary:** Set up the documentation structure and persistent Claude context, so architecture decisions and the workflow don't have to be re-explained every session.

**Added**
- `CLAUDE.md` at the repository root. Claude Code loads it automatically every session, and it imports `DOCUMENTATION/application/ARCHITECTURE.md`. It contains:
  - the project overview and common goal
  - a **session-start reading order**: architecture, changelog, git state, engine design doc, public headers, sources, tests, playground, and finally personal notes (only when asked). Each step explains what that location represents.
  - non-negotiables
  - repository layout
  - build and test commands
  - code conventions
  - workflow
  - a **Definition of done** checklist
  - **Living documentation** rules. The changelog is critical and is updated after every feature and before every PR. Past entries are never rewritten. The architecture and engine docs are kept in sync with the code continuously.
- `DOCUMENTATION/application/ARCHITECTURE.md`. It records system-wide architecture decisions with rationale and a dated decision log:
  - hoardor vs TYLI split
  - engines and event-based communication
  - SQLite persistence model
  - memory policy
  - platform strategy
  - file engine principles
  - testing approach
- `DOCUMENTATION/application/engines/`. This is the home for per-engine design docs and PRDs.
- `DOCUMENTATION/CHANGELOG.md` (this file).

**Removed**
- The empty `DOCUMENTATION/application/claude/` and `DOCUMENTATION/application/product/` folders. Claude context has to be in a root `CLAUDE.md` to load automatically, so product and design docs now live directly under `DOCUMENTATION/application/`.

**Design decisions and trade-offs**
- **Root `CLAUDE.md` vs a nested folder:** only `CLAUDE.md` files at the repository root (or in parent directories) load automatically when a session starts. Instructions placed in `DOCUMENTATION/application/claude/` would need a manual prompt every session.
- **`CLAUDE.md` imports `ARCHITECTURE.md`:** the architecture lives in one place and loads every session, with no duplication.
- **Per-engine design docs:** `engines/<engine>.md` matches the planned agent-per-engine model. Each future engine agent gets a self-contained spec.
