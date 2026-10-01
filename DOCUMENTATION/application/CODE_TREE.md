# Code tree

One tree of the whole repository: every file, namespace, type, function, test, and build target, each with a one-line purpose. Use it to find things before opening files.

- **Update this file** whenever a file, type, or function is added, renamed, or removed (`CLAUDE.md`, "Living documentation"). Then regenerate the HTML version with `python3 tools/code_tree_html.py`. `CODE_TREE.html` is generated, so never edit it by hand.
- **Legend:** `ns` namespace · `class` / `struct` / `enum` types · `fn` free function · `.m()` member function · `static` static member · `(private)` not callable from outside the class · `(internal)` lives in `src/`, not part of the public API · `TEST` a GoogleTest case.
- `DOCUMENTATION/notes/` is the user's personal folder and is left out on purpose.

Last updated: 2026-10-01 (File Sync v1, branch `abhinavp06/FILE_SCANNER_INIT`).

```text
hoardor/
├── CLAUDE.md                                   Claude context: rules, reading order, conventions, workflow (loads every session)
├── README.md                                   (empty)
├── .gitignore                                  build*/, output/, IDE folders
├── CMakeLists.txt                              root build
│   ├── options        BUILD_PLAYGROUND=ON, BUILD_TESTS=ON, RUN_TESTS_AFTER_BUILD=ON, BUILD_BENCHMARKS=OFF
│   ├── target hoardor (static library)         src/db, src/file, src/master; links hoardor_sqlite3 (private), Threads
│   └── platform sources                        Windows: *_windows.cpp · Linux: file_info_posix + mount_points_linux · other: file_info_posix + mount_points_other
│
├── third_party/
│   └── CMakeLists.txt                          SQLite 3.46.1 amalgamation via FetchContent (SHA3-256 pinned)
│       └── target hoardor_sqlite3 (static C)   SQLITE_DQS=0, SQLITE_DEFAULT_MEMSTATUS=0, SQLITE_OMIT_LOAD_EXTENSION
│
├── include/hoardor/                            PUBLIC API (everything a consumer like TYLI may call)
│   │
│   ├── db/
│   │   └── database.hpp                        ns hoardor::db: SQLite mechanics (engines/db.md)
│   │       ├── struct Error                    { int code (SQLite result code), string message }
│   │       ├── using Result<T>                 std::expected<T, Error>
│   │       ├── struct Options                  { int busy_timeout_ms = 5000 }
│   │       ├── class Statement                 one prepared statement; move-only, finalized on destruction
│   │       │   ├── .bind(index, int64 | int | double | string_view)   1-based; chainable; errors surface in step()
│   │       │   ├── .bind_null(index)
│   │       │   ├── .step() -> Result<bool>     true = a row is ready, false = done
│   │       │   ├── .run() -> Result<void>      step to completion (writes)
│   │       │   ├── .reset()                    ready to bind and run again (also clears bindings)
│   │       │   ├── .column_int64 / .column_double / .column_text / .column_is_null (column)   0-based
│   │       │   ├── .note_bind(rc)              (private) remembers the first bind error
│   │       │   └── Statement(sqlite3*, sqlite3_stmt*)   (private) only Database creates statements
│   │       ├── class Database                  one connection; move-only; one thread at a time
│   │       │   ├── static open(file, Options) -> Result<Database>          WAL, synchronous=NORMAL, foreign keys, busy_timeout
│   │       │   ├── static open_in_memory(Options) -> Result<Database>      for tests (no WAL)
│   │       │   ├── .exec(sql) -> Result<void>  one or more statements, no rows
│   │       │   ├── .prepare(sql) -> Result<Statement>
│   │       │   ├── .last_insert_id() / .changes()
│   │       │   └── static open_uri(name, flags, wal, Options)   (private) shared open + pragmas
│   │       ├── class Transaction               BEGIN IMMEDIATE ... COMMIT; rolls back on destruction unless committed
│   │       │   ├── static begin(Database&) -> Result<Transaction>
│   │       │   └── .commit() -> Result<void>
│   │       ├── struct Migration                { int version, string_view sql }
│   │       └── fn migrate(Database&, component, span<Migration>)   applies versions above the recorded one, each in a transaction
│   │
│   ├── file/                                   ns hoardor::file: the file engine (engines/file.md, features/file_sync.md)
│   │   ├── settings.hpp
│   │   │   ├── enum FileKind : uint8           Audio=1, Video=2, Text=3, Image=4, Subtitle=5 (stored in the DB, never renumber)
│   │   │   ├── fn to_string(FileKind)          "audio", "video", ...
│   │   │   ├── fn file_kind_from_string(name)  inverse, ignoring case
│   │   │   ├── struct Settings                 every tunable of the file engine (plain data, persisted in file_settings)
│   │   │   │   ├── extension_kinds             map ext -> FileKind (any case, dot optional)
│   │   │   │   ├── ignored_names / ignored_prefixes          OS + NAS litter, matched ignoring ASCII case
│   │   │   │   ├── sync_on_startup (false) · settle_window_seconds (10) · mass_removal_threshold_percent (25)
│   │   │   │   ├── batch_max_rows (2000) · batch_max_milliseconds (50) · progress_interval_files (500)
│   │   │   │   ├── relocation_sample_size (20) · relocation_min_match_percent (80)
│   │   │   │   └── static defaults()           the default values above
│   │   │   └── fn kind_of(path, Settings)      a file's kind by extension, or nullopt (one-off checks)
│   │   │
│   │   ├── scanner.hpp
│   │   │   ├── struct ScannedFile              { relative_path (UTF-8, '/'), size, mtime_ns (Unix ns), kind }
│   │   │   ├── struct ScanError                { relative_path ("" = root lost, always last), error_code, is_directory }
│   │   │   ├── using ScanResult                std::expected<ScannedFile, ScanError>
│   │   │   ├── struct ScanProgress             { directories_visited, files_emitted, errors }
│   │   │   └── class Scanner                   streams one root's media files; memory grows with depth only
│   │   │       ├── static open(root, Settings) -> expected<Scanner, error_code>   missing / not a folder / unreadable -> error
│   │   │       ├── .next() -> optional<ScanResult>   next file or error; nullopt when done
│   │   │       ├── .progress() -> ScanProgress
│   │   │       ├── struct Level                (private) { directory_iterator, relative path, consumed }: one per depth
│   │   │       ├── .is_ignored(lowered_name)   (private)
│   │   │       ├── .root_reachable()           (private) opens the root to force a real read
│   │   │       └── .on_error(relative, code, is_directory)   (private) local error vs. "root lost" vs. vanished (skip)
│   │   │
│   │   ├── mount_points.hpp
│   │   │   ├── fn list_mount_points()          the OS's mounted volumes (implementations in src/file/platform/)
│   │   │   └── using MountPointLister          std::function returning mount points; tests inject plain folders
│   │   │
│   │   └── library.hpp
│   │       ├── using CategoryId / RootId / EntryId      int64 row ids
│   │       ├── enum ErrorCode                  NotFound, InvalidArgument, AlreadyExists, Overlap, InUse, RootOffline, FileMissing, Database
│   │       ├── struct Error / using Result<T>  { ErrorCode code, string message } / std::expected<T, Error>
│   │       ├── struct Category                 { id, name, kinds }: a UI section (Music, Movies, ...)
│   │       ├── enum RootStatus                 Unknown=0, Online=1, Offline=2
│   │       ├── struct Root                     { id, uuid, category_id, name, path, path_in_volume, use_marker,
│   │       │                                     case_sensitive, status, generation, last_sync_ns, file_count, held_removals }
│   │       ├── struct Entry                    { id, root_id, relative_path, size, mtime_ns, kind, unsettled, changed_generation }
│   │       ├── struct ScanErrorRecord          { root_id, relative_path, is_directory, message, generation }
│   │       ├── enum RootSyncOutcome            Synced, Offline, Cancelled, Failed
│   │       ├── struct RootSyncReport           { root_id, outcome, generation, added, modified, removed, unchanged,
│   │       │                                     unsettled, errors, relocated, removals_held, held_removals, message }
│   │       ├── struct SyncReport               { roots[], cancelled }
│   │       ├── struct SyncProgress             { root_index, root_count, root_id, scan (ScanProgress) }
│   │       ├── using ProgressCallback          std::function<void(const SyncProgress&)>
│   │       └── class Library                   the file engine's repository + logic on one connection
│   │           ├── static open(Database&, MountPointLister = list_mount_points) -> Result<Library>   runs migrations
│   │           ├── settings ··· .load_settings() (defaults + stored) · .save_settings(Settings)
│   │           ├── categories · .categories() · .add_category(name, kinds) · .update_category(Category) · .remove_category(id)
│   │           ├── roots ······ .roots(optional category) · .root(id)
│   │           │                .add_root(category, path, name, use_marker)   normalize, overlap, marker, path_in_volume, case detection
│   │           │                .remove_root(id) · .set_root_category(id, category) · .set_root_case_sensitive(id, bool)
│   │           │                .relocate_root(id, new_path)   verified by marker or a sample of entries
│   │           ├── sync ······· .sync(optional category, stop_token, progress) -> SyncReport   roots of a category, or all
│   │           │                .sync_root(id, stop_token, progress) -> RootSyncReport
│   │           │                .apply_held_removals(id)   applies removals the mass-removal guard held
│   │           ├── reads ······ .entries(root, after, limit) · .changed_entries(root, generation, after, limit)   paged by id
│   │           │                .scan_errors(root, limit) · .resolve(entry) -> path | RootOffline | FileMissing   (playback)
│   │           ├── struct Resolution            (private) { online, relocated, reason }
│   │           ├── .path_in_volume(path)        (private) path relative to its longest containing mount point
│   │           ├── .check_overlap(path, except) (private) rejects nested / containing roots
│   │           ├── .resolve_root(Root&)         (private) marker at last path -> search mounts -> relocate | offline
│   │           └── .sync_one(id, stop, progress, index, count)   (private) the per-root sync algorithm (feature doc §4.6)
│   │
│   └── master/
│       └── sync_worker.hpp                     ns hoardor::master: background execution (engines/master.md)
│           ├── struct SyncCallbacks            { on_progress(SyncProgress), on_finished(scope, SyncReport) }: run on the worker thread
│           └── class SyncWorker                hoardor-owned background sync thread
│               ├── static start(db_file, SyncCallbacks, MountPointLister) -> Result<unique_ptr<SyncWorker>>   own connection; queues a startup sync if enabled
│               ├── ~SyncWorker()               cancels, then the jthread stops and joins
│               ├── .request_sync(optional category) -> bool   false if that scope is already queued or running
│               ├── .cancel()                   stops the running sync (removes nothing) and clears the queue
│               ├── .idle() / .wait_idle()
│               ├── SyncWorker(Database, SyncCallbacks)   (private)
│               ├── .run(stop_token)            (private) the worker loop: wait for a scope, run Library::sync, report
│               └── members                     database_, library_, callbacks_, mutex_, changed_ (condition_variable_any),
│                                               queue_ (deque<scope>), running_, current_ (stop_source), thread_ (jthread, last)
│
├── src/                                        IMPLEMENTATION (internal helpers live here, not in include/)
│   ├── db/
│   │   └── database.cpp                        Statement, Database, Transaction, migrate
│   │       └── fn error_from(sqlite3*, rc)     (internal) SQLite error -> db::Error
│   │
│   ├── file/
│   │   ├── settings.cpp                        to_string, file_kind_from_string, Settings::defaults, kind_of
│   │   │   └── kind_names                      (internal) FileKind <-> name table
│   │   ├── scanner.cpp                         Scanner::open / is_ignored / root_reachable / on_error / next
│   │   ├── text.hpp / text.cpp                 ns hoardor::file::detail (internal)
│   │   │   ├── struct Utf8                     { text (always valid UTF-8), valid (false if repaired) }
│   │   │   ├── fn ascii_lower(text)            lowercases ASCII only
│   │   │   ├── fn normalize_extension(ext)     " .FLAC " -> "flac"
│   │   │   ├── fn to_utf8(path) -> Utf8        never throws; '/' separators; UTF-16 (Windows) or bytes (POSIX)
│   │   │   ├── fn from_utf8(text) -> path      UTF-8 text -> path on every OS
│   │   │   ├── fn utf8_from_utf16(u16string_view) / utf8_from_bytes(string_view)   the two conversions, testable anywhere
│   │   │   └── append_code_point, is_space, replacement_character   (internal helpers)
│   │   ├── file_time.hpp                       ns detail (internal): to_unix_ns(file_time_type), from_unix_ns(ns): exact, via clock_cast
│   │   ├── root_marker.hpp / root_marker.cpp   ns detail (internal): the .hoardor-root file
│   │   │   ├── marker_file_name                ".hoardor-root"
│   │   │   ├── fn new_uuid()                   random v4 UUID
│   │   │   ├── fn read_marker(dir) -> optional<uuid>   nullopt if missing, unreadable, or malformed
│   │   │   ├── fn write_marker(dir, uuid) -> bool      writes then reads back; false on read-only storage
│   │   │   └── header, looks_like_uuid         (internal helpers)
│   │   ├── library_internal.hpp                ns detail (internal), shared by library.cpp + library_sync.cpp
│   │   │   ├── fn database_error(db::Error)    -> file::Error{Database}
│   │   │   ├── fn is_constraint(db::Error)     SQLITE_CONSTRAINT?
│   │   │   ├── fn path_key(relative_path, case_sensitive)   unique key per root (ASCII-lowercased if case-insensitive)
│   │   │   ├── fn root_columns() / read_root(Statement)     SELECT list + row -> Root
│   │   │   ├── fn entry_columns() / read_entry(Statement)   SELECT list + row -> Entry
│   │   │   └── fn set_root_status(Database&, id, RootStatus)
│   │   ├── library.cpp                         Library: open, settings, categories, roots, relocation, entries, resolve
│   │   │   ├── schema_v1 / migrations          (internal) file migration 1 (tables + seeded categories)
│   │   │   ├── join_lines, split_lines, parse_int<Int>(text, out, min, max)   (internal) settings text format
│   │   │   ├── serialize(Settings) / apply(Settings&, key, value)              (internal) Settings <-> file_settings rows
│   │   │   ├── kinds_text(kinds) / parse_kinds(text)                           (internal) "audio,image" <-> kinds
│   │   │   ├── trimmed(text)                                                    (internal)
│   │   │   ├── overlaps(a, b)                  (internal) component-wise, ASCII-case-insensitive prefix check
│   │   │   ├── flip_ascii_case(name) / detect_case_sensitive(dir)              (internal) read-only case probe
│   │   │   └── normalized_directory(path)      (internal) absolute + canonical + must exist + must be a folder + valid UTF-8
│   │   ├── library_sync.cpp                    Library: sync, sync_root, sync_one, apply_held_removals, changed_entries, scan_errors
│   │   │   ├── now_ns()                        (internal) system clock in Unix ns
│   │   │   ├── count_entries(db, root, condition, generation)   (internal)
│   │   │   ├── struct Statements               (internal) the per-sync prepared statements: touch_unchanged (fast path),
│   │   │   │   │                                find, insert, update_changed, update_seen, touch_subtree, touch_one, record_error
│   │   │   │   └── static prepare(db)
│   │   │   └── run(Statement&)                 (internal) run + reset
│   │   └── platform/                           one implementation per OS, selected by CMake (never #ifdef in shared code)
│   │       ├── file_info.hpp                   ns detail: struct FileInfo { size, mtime_ns }; fn file_info(directory_entry)
│   │       ├── file_info_posix.cpp             one stat() (Linux st_mtim / macOS st_mtimespec)
│   │       ├── file_info_windows.cpp           values cached by the directory listing (no system call)
│   │       ├── mount_points_linux.cpp          list_mount_points from /proc/self/mountinfo; unescape(field) (\040 etc.)
│   │       ├── mount_points_windows.cpp        list_mount_points from GetLogicalDriveStringsW
│   │       └── mount_points_other.cpp          returns {} (macOS until file engine phase 4)
│   │
│   └── master/
│       └── sync_worker.cpp                     SyncWorker::start / ~SyncWorker / request_sync / cancel / idle / wait_idle / run
│
├── tests/                                      GoogleTest (target hoardor_tests, 109 tests, run by ctest)
│   ├── CMakeLists.txt                          GoogleTest 1.15.2 via FetchContent; gtest_discover_tests; run_tests target
│   ├── support/
│   │   ├── temp_dir.hpp                        ns hoardor::test: class TempDir (unique temp folder; .path(), .write(relative, size)); fn u8path(utf8)
│   │   └── library_fixture.hpp                 class LibraryTest : Test (in-memory DB + Library + injectable mounts);
│   │                                           .category(name), .all_entries(root), .set_settings(edit)
│   ├── db/
│   │   └── database_test.cpp                   12 TESTs
│   │       ├── Database.*                      OpensInMemoryWithForeignKeysOn, OpensFileInWalMode, OpenFailsForImpossiblePath, ReportsSqlErrors
│   │       ├── Statement.*                     BindsAndReadsEveryType, ResetAllowsReuse, BindErrorIsReportedByStep
│   │       ├── Transaction.*                   CommitKeepsChanges, DestructorRollsBack
│   │       ├── Migrate.*                       AppliesEachVersionOncePerComponent, FailedMigrationRollsBackAndKeepsVersion
│   │       └── Wal.*                           ReaderSeesCommittedDataWhileAnotherConnectionWrites
│   ├── file/
│   │   ├── settings_test.cpp                   10 TESTs
│   │   │   ├── SettingsDefaults.*              CoverEveryKind, IgnoreListsHoldExternalDriveLitter
│   │   │   ├── KindOf.*                        IgnoresExtensionCase, AcceptsUserKeysWithDotOrUppercase, RespectsEditedMap,
│   │   │   │                                   NoExtensionIsNotMedia, DotOnlyNameIsNotMedia, MultipleDotsUseTheLastExtension, EmptySettingsMeanNothingIsMedia
│   │   │   └── FileKindNames.RoundTrip
│   │   ├── text_test.cpp                       6 TESTs: NormalizeExtension.StripsDotSpacesAndCase, AsciiLower.LeavesUtf8Untouched,
│   │   │                                       Utf8FromBytes.{AcceptsValidUtf8, RepairsInvalidSequences},
│   │   │                                       Utf8FromUtf16.ConvertsPairsAndRepairsLoneSurrogates, ToUtf8.UsesForwardSlashes
│   │   ├── scanner_test.cpp                    23 TESTs
│   │   │   ├── ScannerOpen.*                   FailsWhenRootIsMissing, FailsWhenRootIsAFile
│   │   │   └── Scanner.*                       EmptyRootEndsImmediately, EmitsNestedMediaWithRelativeForwardSlashPaths, SkipsDirectoriesAndNonMedia,
│   │   │                                       ClassifiesKinds, ReportsExactSizeAndMtime, MtimeIsStableAcrossScans,
│   │   │                                       RootWithTrailingSeparatorGivesSameRelativePaths, NonAsciiNamesAreUtf8AndOpenTheSameFile,
│   │   │                                       DeepNestingAndLongPaths, IgnoredNamesAndPrefixesAreSkippedAndNotDescended, IgnoredFileNamesAreSkipped,
│   │   │                                       CustomSettingsAreRespected, SymlinksAreNeitherEmittedNorFollowed,
│   │   │                                       UnreadableSubdirectoryIsReportedAndScanContinues, RootRemovedMidScanReportsRootLostOnceAndLast,
│   │   │                                       FilesVanishingMidScanAreSilentlySkipped, DestroyedMidScanIsSafe, ProgressCountersMatchTheTree,
│   │   │                                       NamesDifferingOnlyInCaseAreBothEmitted, FifoIsNotEmitted (POSIX), InvalidUtf8NamesAreReportedNotEmitted (POSIX)
│   │   ├── library_test.cpp                    21 TESTs
│   │   │   ├── LibraryTest.* (settings)        SeedsDefaultCategories, OpeningAgainKeepsData, SettingsDefaultWhenNothingStored,
│   │   │   │                                   SettingsRoundTripEveryField, CorruptSettingValuesFallBackToDefaults, UnknownSettingKeysAreIgnored
│   │   │   ├── LibraryTest.* (categories)      AddUpdateRemoveCategory, CategoryNamesAreUniqueIgnoringCaseAndNotEmpty, CategoryWithFoldersCannotBeRemoved
│   │   │   ├── LibraryTest.* (roots)           AddRootWritesMarkerAndRecordsTheFolder, AddRootAsWholeVolumeHasEmptyPathInVolume, AddRootRejectsBadInput,
│   │   │   │                                   RootsOnDifferentDrivesAndCategoriesAreIndependent, OverlappingRootsAreRejected,
│   │   │   │                                   FolderAlreadyInTheLibraryUnderAnotherPathIsRejected, ExistingMarkerIsAdoptedWhenReAdding,
│   │   │   │                                   RootWithoutMarkerWritesNothing, ReadOnlyFolderFallsBackToNoMarker,
│   │   │   │                                   RootCategoryCanChangeButOnlyToAnExistingOne, RemoveRootKeepsItsMarkerOnDisk
│   │   │   └── RootMarker.RejectsMalformedFiles
│   │   └── sync_test.cpp                       29 TESTs (class SyncTest : LibraryTest; .put(relative, size, mtime), .add_music(use_marker), .music(), .paths(root))
│   │       ├── basics                          FirstSyncAddsMediaOfTheCategoryKindsOnly, SecondSyncChangesNothing,
│   │       │                                   SizeOrMtimeChangesAreModificationsAndKeepTheId, DeletedFilesAreRemovedBelowTheGuard
│   │       ├── guard                           MassRemovalIsHeldUntilConfirmed, MassRemovalThresholdIsASetting
│   │       ├── drives                          MissingRootIsOfflineAndKeepsEverything, DifferentDriveAtTheOldPathIsNotScanned,
│   │       │                                   DriveUnderANewLetterIsFoundThroughMountPoints, EmptiedFolderWithoutMarkerIsTreatedAsUnmounted,
│   │       │                                   EmptyMountPointWithoutOurMarkerIsOffline, DriveLostMidSyncRemovesNothing
│   │       ├── stopping                        CancelledSyncRemovesNothing, StopRequestedBeforeSyncStarts
│   │       ├── time                            FileBeingCopiedIsUnsettledUntilItSettles, FutureMtimeIsNotUnsettledForever
│   │       ├── categories + case               ChangingTheCategoryAppliesItsKinds, CaseOnlyRenameOnCaseInsensitiveRootKeepsTheId,
│   │       │                                   CaseInsensitiveIsRefusedWhenNamesCollide, RemovingARootRemovesItsEntries
│   │       ├── scope + batching                SyncOfOneCategoryOnlyTouchesItsRoots, OneOfflineRootDoesNotStopTheOthers,
│   │       │                                   SmallBatchesGiveTheSameResult, PagingVisitsEveryEntryOnce
│   │       └── playback + relocation           ResolveForPlayback, ManualRelocationWithMarker, ManualRelocationRefusesAnotherRootsFolder,
│   │                                           ManualRelocationWithoutMarkerChecksASample, UnreadableSubfolderKeepsItsEntries
│   └── master/
│       └── sync_worker_test.cpp                8 TESTs (class SyncWorkerTest: a real DB file + .music(), .add_music_root(n), .set_settings(edit))
│           └── SyncWorkerTest.*                RunsARequestedSyncInTheBackground, DuplicateRequestsAreIgnored,
│                                               CancelStopsTheRunningSyncAndRemovesNothing, SyncOnStartupWhenEnabled, NoStartupSyncByDefault,
│                                               OtherConnectionsCanReadAndWriteDuringASync, DestructorStopsARunningSyncPromptly,
│                                               StartFailsWhenTheDatabaseCannotBeOpened
│
├── benchmarks/                                 Google Benchmark 1.9.0 (BUILD_BENCHMARKS=ON, Release); target hoardor_benchmarks
│   ├── CMakeLists.txt
│   ├── support/
│   │   └── bench_tree.hpp                      ns hoardor::bench: fn file_count() (env HOARDOR_BENCH_FILES, default 50,000);
│   │                                           fn library_tree() (Music/Artist N/Album M/Track K.flac, built once per process)
│   └── file/
│       ├── scanner_benchmark.cpp               BM_Scan: full scan of the tree
│       └── sync_benchmark.cpp                  struct Synced, fn fresh_library() (DB file + Library + one root, settle window 0);
│                                               BM_FirstSync (all inserts), BM_IncrementalSync (nothing changed)
│
├── playground/                                 manual experiments against real drives
│   ├── CMakeLists.txt                          targets hoardor_scan, hoardor_sync
│   └── file/
│       ├── scan_playground.cpp                 main: hoardor_scan <folder>: progress, counts by kind, errors, time
│       └── sync_playground.cpp                 main: hoardor_sync <db> <category> <folder>: adds the root once, syncs, prints the report
│
├── tools/                                      developer scripts (not part of the library)
│   └── code_tree_html.py                       CODE_TREE.md -> CODE_TREE.html (run after every edit of this file)
│       ├── fn code_blocks(text)                the fenced text blocks of the Markdown (fences must start a line)
│       ├── fn kind_of(label)                   dir / file / ns / class / struct / enum / fn / method / test / ... for badges
│       ├── fn parse_tree(block)                box-drawing tree -> nested nodes (continuation lines join the node above)
│       ├── fn count(node, kinds)               the stats shown at the top of the page
│       └── fn main()                           writes the self-contained HTML (TEMPLATE: search, expand/collapse, light/dark)
│
└── DOCUMENTATION/
    ├── CHANGELOGS/
    │   ├── README.md                           versions index, versioning rules, how to cut a version, entry template
    │   ├── unreleased.md                       finished work not yet in a version (newest first)
    │   └── baseline.md                         project state before the changelog existed
    └── application/
        ├── ARCHITECTURE.md                     system-wide decisions + decision log (loads every session via CLAUDE.md)
        ├── CODE_TREE.md                        this file (the source)
        ├── CODE_TREE.html                      the same tree as an offline page with search; GENERATED by tools/code_tree_html.py
        ├── engines/
        │   ├── file.md                         file engine reference: purpose, storage model, current state, roadmap
        │   ├── db.md                           db engine reference
        │   └── master.md                       master engine reference (SyncWorker)
        └── features/
            └── file_sync.md                    File Sync v1 plan + as-built results, decisions, open items (OI-1)
```

## Call flow of a Sync (how the pieces connect)

```text
TYLI "Sync" button
└── master::SyncWorker::request_sync(category)            caller's thread: queue the scope, return immediately
    └── SyncWorker::run (worker thread)
        └── file::Library::sync(category, stop, on_progress)
            └── for each root of the category: Library::sync_one
                ├── load_settings + categories             -> Settings filtered to the category's kinds
                ├── resolve_root                           -> read_marker / list_mount_points -> online | relocated | offline
                ├── Scanner::open + Scanner::next ...       -> ScannedFile | ScanError   (file_info: one stat per media file)
                │   └── per item, in short db::Transactions: Statements.touch_unchanged | find + insert | update_changed | ...
                ├── resolve_root again, empty-root guard, mass-removal guard
                └── DELETE unseen rows (or hold them), update file_roots
        └── SyncCallbacks::on_finished(scope, SyncReport)  -> TYLI's bridge -> UI thread
```
