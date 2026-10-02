# hoardor

hoardor is the core C++ library behind **TYLI**, a fully offline, native alternative to Plex for music, movies, TV shows, and text (books, blogs, notes). It is a library only. The Qt/QML application lives in a separate repository (TYLI) and consumes hoardor. hoardor never contains UI code and never depends on Qt.

The architecture, the decisions behind it, and their rationale are in the file below. Treat it as binding. Change it only through an explicit decision with the user, and record that decision in it.

@DOCUMENTATION/application/ARCHITECTURE.md

## Workspace

hoardor usually lives in a workspace next to the TYLI app: `/root/tyli-workspace/{hoardor, tyli}`. The workspace root has its own `CLAUDE.md` (not version-controlled), which imports this file and holds the cross-repo rules. Every path in this file is relative to the hoardor repository root, wherever the session started. hoardor never depends on TYLI.

## Common goal

Build a single, snappy, low-memory, fully offline home for the user's whole media and writing library, with hoardor as the engine room. Every change should move toward that goal without costing performance, memory, or portability.

## Session start: read in this order

Do this at the start of every session, before proposing or changing anything. Each step adds context the next step depends on.

1. **`CLAUDE.md` + `DOCUMENTATION/application/ARCHITECTURE.md`** load automatically. They define *how* we build: rules, conventions, and decisions.
2. **`DOCUMENTATION/CHANGELOGS/`** covers *where we are*. Read `unreleased.md` in full, and the newest `v*.md` file (find it in the index in `README.md`), so you know what was done last, the open follow-ups, and the known limitations. Search older version files when the history of a specific area matters.
3. **`git status` and `git log --oneline -15`** show the current branch and any uncommitted work that the changelog doesn't capture yet.
4. **`DOCUMENTATION/application/features/<feature>.md`** gives *what we're building right now*: the feature's goals, phases, API detail, edge cases, test plan, decisions, and open items. **`DOCUMENTATION/application/engines/<engine>.md`** gives *what each engine is*: its responsibilities, principles (e.g. the file engine's storage model), current API and tables, and roadmap. Read the feature doc for the task, and the doc of every engine it touches.
5. **`DOCUMENTATION/application/CODE_TREE.md`** is one tree of the whole repository: every file, type, function, and test, with a line each. Use it to find code before opening files. When the task touches stored data, also read **`DOCUMENTATION/application/DATABASE.md`**: every table, column, index, and migration.
6. **`include/hoardor/<engine>/`** is the public API contract for that engine. Read it before the implementation.
7. **`src/<engine>/`** contains the implementation, including the platform backends in `src/<engine>/platform/`.
8. **`tests/<engine>/`** shows what is already covered, so new tests extend it and don't duplicate it.
9. **`playground/<engine>/`**: read only when the task involves a manual experiment.
10. **`DOCUMENTATION/notes/`** holds the user's personal notes. Read only when the user asks, and never edit.

## Non-negotiables

- **Performance and low RAM come first.** Anything that runs in the background, such as scanning, syncing, or indexing, must be fast and use a flat amount of memory. Never load the whole library into memory. Data lives in SQLite and is read in pages.
- **Stack:** C++23, CMake, SQLite (raw `sqlite3`), and GoogleTest. Add a third-party library only when a domain requires it (for example ffmpeg or a tag reader), and discuss it with the user first.
- **Cross-platform:** the user's main OS is Windows, and Linux and macOS must also work well. Development and testing happen on a Linux VM. Keep platform-specific code behind an interface in its own files, and never use `#ifdef` in shared logic.
- **Customizable by design:** behavior the user might want to tune (file-extension mappings, intervals, categories) is configuration, not a hard-coded constant. Start as configurable as possible. Real use will show what to prune. Each engine keeps every tunable in one plain `Settings` struct with defaults in code, persisted in SQLite (ARCHITECTURE §3a).
- **Keep it simple. No bloat.** Prefer plain structs and free functions. Write a class only when something holds state or invariants (e.g. `Scanner`). Add an abstract interface only when a second implementation actually exists. Don't build a dozen types for a small feature. Keep future integrations in mind, but don't build for them early.

## Repository layout

```
CLAUDE.md                     # this file. It loads automatically every session
CMakeLists.txt                # root build. Defines the `hoardor` library and the options
include/hoardor/<engine>/     # public headers (.hpp)
src/<engine>/                 # implementations (.cpp). Platform backends live in src/<engine>/platform/
tests/<engine>/               # GoogleTest suite. Thorough and edge-case driven
playground/<engine>/          # throwaway manual experiments. Anything goes here (hoardor_scan, hoardor_sync)
benchmarks/<engine>/          # Google Benchmark targets (BUILD_BENCHMARKS=ON, Release build)
third_party/                  # third-party code fetched by CMake (SQLite amalgamation)
tools/                        # developer scripts (code_tree_html.py)
DOCUMENTATION/
  CHANGELOGS/                 # detailed changelog, one file per version. See "Living documentation" below
    README.md                 # index of versions, versioning rules, how to cut a version, entry template
    unreleased.md             # finished work not yet in a version (new entries go here)
    v<x.y.z>.md               # one frozen file per version
    baseline.md               # project state before the changelog existed
  notes/                      # the user's PERSONAL notes. Read only when asked. Never edit
  application/
    ARCHITECTURE.md           # system-wide architecture and decision log
    DATABASE.md               # every SQLite table, column, index, and migration (the schema's single reference)
    CODE_TREE.md              # one tree of the whole repo: files, types, functions, tests, targets (the source)
    CODE_TREE.html            # the same tree as an offline page with search; GENERATED, never edit by hand
    engines/<engine>.md       # long-lived reference per engine: responsibilities, principles, current API/tables, roadmap
    features/<feature>.md     # plan per feature (PRD): phases, API detail, edge cases, tests, decisions, open items, status
```

## Build and test

hoardor needs ffmpeg's development files (metadata reading):
- **Linux:** `apt install libavformat-dev libavcodec-dev libavutil-dev ffmpeg`. The `ffmpeg` command-line tool generates test media; tests that need it skip without it.
- **macOS:** `brew install ffmpeg`.
- **Windows:** a shared LGPL build (e.g. BtbN's `win64-lgpl-shared`), passed with `-DFFMPEG_ROOT=<folder with include/ lib/ bin/>`, its `bin` on `PATH`.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Build options in the root `CMakeLists.txt`: `BUILD_PLAYGROUND`, `BUILD_TESTS`, `RUN_TESTS_AFTER_BUILD` (default ON when hoardor is built on its own, OFF when an app such as TYLI adds it with `add_subdirectory`), `BUILD_BENCHMARKS` (default OFF). Build output goes to `build*/` (git-ignored).

```bash
# Benchmarks (always Release)
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_BENCHMARKS=ON -DBUILD_TESTS=OFF
cmake --build build-release -j && ./build-release/benchmarks/hoardor_benchmarks

# Sanitizers (san = address or thread). On recent kernels ThreadSanitizer needs ASLR off: prefix with `setarch $(uname -m) -R`.
# ThreadSanitizer also needs TSAN_OPTIONS=suppressions=tests/tsan.supp (SQLite's lock-free WAL index, see the file).
cmake -S . -B build-$san -DCMAKE_BUILD_TYPE=Debug -DRUN_TESTS_AFTER_BUILD=OFF \
      -DCMAKE_CXX_FLAGS=-fsanitize=$san -DCMAKE_C_FLAGS=-fsanitize=$san -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=$san
```

Permission tests (unreadable or read-only folders) skip themselves when run as root. On this VM, run the test binary once as an unprivileged user (e.g. copy it somewhere world-readable and use `runuser -u nobody -- ./hoardor_tests`) so they run too.

## Code conventions

- Each engine has its own namespace: `hoardor::<engine>` (for example `hoardor::file`, `hoardor::db`). Use `#pragma once`.
- Types and enum values use `PascalCase`. Functions, variables, members, and file names use `snake_case`. Headers use `.hpp` and sources use `.cpp`.
- Engines never call each other directly. The master engine coordinates them. Until `core` has an event bus, engines return results to the caller (return values, `next()`-style iterators, callbacks).
- `hoardor::core` holds shared infrastructure (event bus, ring buffers, …). Add to it only when a concrete feature needs it, never speculatively.
- Each engine owns its SQLite tables (prefixed `<engine>_`) and its repository. `hoardor::db` provides only the mechanics.
- Store paths as UTF-8 at every boundary (the database, events, the public API).
- Match the style of the surrounding code. Keep comments for the *why*, not the *what*.

## Workflow

1. **Design before code.** For every feature, write `DOCUMENTATION/application/features/<feature>.md` (goals, phases, public API, schema, edge cases, test plan, open items), and update the `engines/<engine>.md` of every engine whose responsibilities or principles change. Get the user's approval before implementing.
2. **Work in feature branches, built in phases.** A branch delivers one complete, shippable feature and becomes one PR against `master`. Inside it, the work is built and tested phase by phase, and each phase is designed in its engine doc before it's coded. Branch names follow `abhinavp06/<topic>`.
3. **Tests are not optional.** Every phase ships with GoogleTest coverage of the edge cases listed in its design doc. Add benchmarks for performance-critical paths and check them against the targets in the design doc.
4. **Keep the docs in sync with the code, continuously.** Update them during the work, not only at the end. See "Living documentation" below.
5. **Commit or open a PR only when the user asks.**
6. **The user is new to C++ and Qt.** Explain non-obvious C++ and CMake decisions briefly when you make them.

### Definition of done (for every feature, and before every PR)

- [ ] The build passes, and `ctest` passes on Linux.
- [ ] New or changed behavior has edge-case tests.
- [ ] `DOCUMENTATION/CHANGELOGS/unreleased.md` has a complete entry (see below).
- [ ] `DOCUMENTATION/application/features/<feature>.md` matches what was actually built, and its status is updated (e.g. Shipped in `v0.1.0`).
- [ ] `DOCUMENTATION/application/engines/<engine>.md` "Current state" lists the real public API, tables, and settings.
- [ ] `DOCUMENTATION/application/ARCHITECTURE.md` reflects any system-wide decision, including a new decision-log row.
- [ ] `DOCUMENTATION/application/DATABASE.md` matches every table, column, index, and migration that was added or changed.
- [ ] `DOCUMENTATION/application/CODE_TREE.md` lists every file, type, function, and test that was added, renamed, or removed, and `CODE_TREE.html` was regenerated (`python3 tools/code_tree_html.py`).

## Living documentation

These files are the project's memory. The user relies on them for context later, so treat them as part of the deliverable, not an afterthought.

- **`DOCUMENTATION/CHANGELOGS/` is critical. History matters.** Add an entry to `unreleased.md` after every completed feature, and whenever the user asks for a PR. Use the entry template in `CHANGELOGS/README.md`. When a version is cut, follow "Cutting a version" in that README: entries move unchanged into `v<version>.md`, and the root `CMakeLists.txt` version must match. Each entry includes:
  - what changed and why
  - design decisions and the alternatives rejected
  - every file touched
  - the tests added and the edge cases they cover
  - performance results
  - known limitations and follow-ups

  Be thorough. A future reader with no other context should understand the change. New work goes in `unreleased.md` until a version is cut. Never rewrite or delete past entries, and never edit a version file once it's cut. If something was wrong, add a correcting entry.
- **`DOCUMENTATION/application/ARCHITECTURE.md`**: update it whenever a system-wide decision is made or changed, and add a dated row to its decision log.
- **`DOCUMENTATION/application/features/<feature>.md`**: update it whenever the feature's design, API, schema, edge cases, or phases change. Implementation often reveals changes, so record them as they happen, so the doc never drifts from the code.
- **`DOCUMENTATION/application/DATABASE.md`**: the user asked for one schema reference (2026-10-02). Feature docs propose schema changes (listed under its "Proposed" section); when they're built, update its tables, indexes, and migration history in the same change.
- **`DOCUMENTATION/application/CODE_TREE.md` + `CODE_TREE.html`**: the user asked for both to stay current whenever we work on a feature. Update the Markdown in the same change whenever a file, type, function, test, or build target is added, renamed, or removed, then run `python3 tools/code_tree_html.py` to regenerate the HTML. Never edit the HTML by hand. It's a map, so a stale entry is worse than none.
- **`DOCUMENTATION/application/engines/<engine>.md`**: update it when an engine's responsibilities, principles, or roadmap change, and refresh its "Current state" whenever a feature ships.
