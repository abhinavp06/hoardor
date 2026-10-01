# Unreleased

Work that is done but not yet part of a version. The newest entries come first. When a version is cut, these entries move unchanged into `v<version>.md`, and this file is emptied (see `README.md`).

### Changelog split into per-version files (2026-10-01, branch `abhinavp06/file_engine_init`)

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

### File engine: hoardor owns background sync; playback-stutter open item (2026-10-01, branch `abhinavp06/file_engine_init`)

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

### File engine: open questions resolved, background sync (2026-10-01, branch `abhinavp06/file_engine_init`)

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

### File engine: Sync by category (2026-10-01, branch `abhinavp06/file_engine_init`)

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

### File engine: on-demand scanning and storage model (2026-10-01, branch `abhinavp06/file_engine_init`)

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

### File engine design (phase 1 scope) and the `core` engine decision (2026-10-01, branch `abhinavp06/file_engine_init`)

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

### Project documentation and Claude context (2026-10-01, branch `abhinavp06/file_engine_init`)

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
