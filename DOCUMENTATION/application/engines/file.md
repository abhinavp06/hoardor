# File engine (`hoardor::file`)

Status: **Decided** (with the approval of `features/file_sync.md`, 2026-10-01). §3 "Current state" matches the code shipped in `v0.1.0` (File Sync v1).

This is the long-lived reference for the file engine: what it owns, the storage model it's built on, its current public API and tables, and its roadmap. Each feature that changes the engine has its own doc in `features/`.

## 1. Purpose

The file engine is hoardor's only view of the file system. It answers three questions for the rest of the library:

1. **What media files exist?** It scans library roots and classifies files by kind.
2. **What changed since the last scan?** It reconciles a scan against the database.
3. **Where is file X right now?** It resolves a stored file to an openable path, for example when the player loads a track, and knows when the storage holding it is offline.

Scanning is **on demand**, through **Sync**: the user presses Sync in a section of the app (one category), or a global Sync covers everything. See `features/file_sync.md`. The user never picks folders to scan by hand. Realtime watching is deferred (§4.3). The scan was always the source of truth, and a watcher would only have been a hint. Dropping the watcher removes a background thread, kernel watch memory, three platform backends, and a Windows problem where an open watch handle blocks safe-eject. The cost is that changes appear only after the next scan.

### Consumers

| Consumer | What it needs from the file engine | Phase |
|---|---|---|
| Background scanning | A fast, flat-memory walk of a root, with progress | 1 |
| Database / library state | `file_*` tables kept in sync with disk (added, modified, removed) | 2 |
| Metadata engines (`audio`, `video`, `text`) | The list of added or changed files after a scan, with their paths | 2 |
| Player | A stored file resolved to an absolute path, or "storage offline" | 2 |
| UI (through TYLI) | Root status (online, offline) without waking sleeping drives | 2 |

### Non-goals

- **Reading file contents.** Tags, durations, and cover art belong to the `audio`, `video`, and `text` engines. The file engine reads bytes only for the partial move-detection fingerprint (phase 3).
- **Writing to media storage.** hoardor never modifies, moves, or deletes the user's media. The one exception is the optional root marker file (§2.3).
- **Library categories** (Music, Podcast, Movie, …). Category is configuration on a library root (ARCHITECTURE §6). The file engine only knows *kinds*.
- **Event delivery infrastructure.** With on-demand scanning, a scan returns its results to the caller. Nothing needs an event bus (ARCHITECTURE §2).


## 2. Storage model

Today the whole library sits on external USB hard drives attached to a laptop. Later it may move to RAID, a NAS, or a home server. The file engine must work on all of them **without being aware of which one it is on**. Wherever storage types differ, the difference is configuration, not code.

### 2.1 Storage scenarios

| Scenario | What happens | How the design handles it |
|---|---|---|
| External HDD unplugged | The root's path disappears | The root goes **offline**. Entries are kept, and the player reports "offline" |
| Drive comes back with a different drive letter (`E:` → `F:`), or Linux/macOS mounts it as `Drive1` | The stored absolute path is wrong | Root identity doesn't depend on the path (§2.3). The root is found again and its path updated |
| A *different* drive gets the old letter `E:` | The old path exists but holds other content | The marker belongs to a different root, so this root stays offline. It is **never** scanned as "everything deleted, everything new" |
| Unmounted drive leaves an empty mount-point folder (common on Linux `/mnt/...` and with NAS mounts) | The root path exists but is empty | No marker, or zero files where the database has some, means offline, not deleted (§2.4) |
| Drive unplugged mid-scan | Every directory read starts failing | The scanner reports "root lost" once and stops. Reconciliation keeps what it saw and removes nothing |
| Drive asleep (spun down) | Any disk access spins it up, which takes 5–10 s and makes noise | Browsing the library reads only SQLite, never the media drive. Syncing a section spins up only the drives that hold that section's directories. Syncing at startup is a setting |
| Several external HDDs | Scanning two roots on the same disk at once makes the disk head thrash | Phase 2 scans one root at a time. Concurrency per volume comes later as configuration |
| File copied in while a scan runs | A half-written file looks like a real one | Recently modified files are stored as *unsettled* and skipped by metadata engines until a later scan (`features/file_sync.md` §4) |
| exFAT/FAT32 drive shared between OSes | FAT32 stores local time, so a DST or timezone change shifts every mtime by whole hours. Precision is coarse (2 s on FAT32) | Phase 2 compares mtimes for equality. Phase 3 checks suspicious whole-hour shifts with the fingerprint before calling them modified |
| NTFS drive plugged into a Mac | macOS mounts NTFS read-only | Read-only roots work. The marker becomes optional, with a fallback identity (§2.3) |
| Library moved from external HDDs to RAID or a NAS | Every root's location changes. A copy may not preserve mtimes | Entries are stored relative to their root, so relocating a root keeps every entry, and its play counts and metadata. Changed mtimes only trigger a metadata re-read |
| NAS share (SMB/NFS) as a root | Each `stat` is a network round trip. Mounts can drop or hang | The same root model, with a UNC path (`\\nas\media`) or mount path as its location. Scan time depends on latency. Parallel listing comes later. A hung NFS hard mount can block a scan thread (known limitation) |
| RAID array or SSD | Fast, and handles parallel reads well | Looks like one ordinary volume. Gets a higher per-volume scan concurrency (later) |

### 2.2 Library configuration and paths

- A **category** (Music, Movies, Shows, Podcasts, Books, Blogs, …) is user configuration, not a hard-coded enum (ARCHITECTURE §6). It corresponds to a section of the UI. Adding "Podcasts" or "Blogs" later is a settings change, not a code change.
- A **library root** is a directory the user assigns to a category. It can be a whole volume (`E:\`), a subfolder (`E:\Media\Music`), a RAID mount, or a network share. A root belongs to exactly one category.
- A category can have **several roots**, for example Music split across two external HDDs. Syncing Music syncs both.
- **There is no common library folder.** Roots are independent and can sit on different drives. For example:
  - Music on HDD A (`E:\Music`)
  - Movies on HDD B (`F:\Films`)
  - Shows on HDD B and a NAS share at the same time

  Nothing assumes they share a parent folder or a drive.
- A root's location is the **only** absolute path the file engine stores, and it is allowed to change.
- Every file is stored as `root_id` plus `relative_path` (UTF-8, `/` separators). Relocating a root updates one row.
- `relative_path` keeps the exact bytes the file system returned and is **never Unicode-normalized**. Normalizing would change the name, and the file couldn't be opened again. Folding case and accents for search is a separate key, built by whatever engine provides search.
- Roots can't overlap. A root inside another root, on the same volume, is rejected, so no file is counted twice.

### 2.3 Root identity

Three pieces of identity are used, strongest first:

1. **Marker file** (Decided): `.hoardor-root` in the root folder, holding a random root UUID and a format version. The name is hoardor's, not TYLI's (`features/file_sync.md` §5).
   - The same code works on every OS, file system, RAID, and NAS, with no platform API involved.
   - It travels with the data. A drive with a new letter, or a library copied to RAID, is recognized automatically.
   - It tells "a different drive got this letter" apart from "this root is empty".
   - Writing it is opt-out per root. It is skipped on read-only storage.
2. **Volume ID**: the file-system UUID or serial (phase 4, platform-specific). This is the fallback when there's no marker.
3. **Last-known path**: a hint, always checked first because it's the cheapest.

**Resolving a root** (before a scan or playback, never just for browsing):
1. If the last-known path holds a marker with this root's UUID, the root is **online**.
2. If the last-known path holds a marker with a **different** UUID, the root stays **offline**. This path now belongs to another root.
3. Otherwise, look for the marker on each mounted volume, at the root's path within its volume. If found, the root has been **relocated**: update its path and mark it online.
4. Otherwise the root is **offline**. The user can relocate it manually (`relocate_root`). That is verified by the marker, or, for a root without one, by checking that a sample of known entries exists with matching sizes.

Telling online from offline must not wake a sleeping drive for UI purposes. "Is the volume mounted" comes from the OS mount list. The marker is read only when a scan or playback is about to touch the drive anyway.

### 2.4 Safety rules: offline is never "deleted"

Reconciliation removes entries only when **all** of these hold:
- The scan completed: no root-lost error, and the scanner wasn't stopped early.
- The root resolves to the same identity at the end of the scan as at the start.
- The entry isn't under a subtree that returned a `ScanError`.
- The scan found at least one file, or the database had none for this root. An empty root with known entries is treated as an unmounted drive.
- The removal is below the **mass-removal guard**: a configurable share of the root's entries (default 25%). Above it, the changes are reported to the caller for confirmation and not applied.

### 2.5 HDD vs SSD (and RAID, NAS)

The code is the same for every kind of storage. Nothing branches on "is this an HDD". The differences show up only as configuration and performance:

| Aspect | HDD | SSD / RAID / NAS | Where it's handled |
|---|---|---|---|
| Cold sync speed | Bounded by seeks (about 12–15 ms each) | Much faster, with no seeks. A NAS is bounded by network round trips instead | Benchmarks and manual measurement (`features/file_sync.md` §3.8) |
| Concurrent scans on one device | Harmful: the disk head thrashes | Beneficial | Per-volume `scan_concurrency` setting (phase 4), default 1, which is safe everywhere |
| Playback during a sync | Can stutter (`features/file_sync.md` §6 OI-1) | Not expected | `features/file_sync.md` §6 OI-1 |
| Sleeping drive wake-up | 5–10 s spin-up | None | Browsing never touches media drives (§2.1) |
| Fingerprint reads (phase 3) | Each costs a seek | Cheap | Fingerprints only for unpaired removed/added files, never the whole library |

Detecting the storage type automatically is possible on each OS, but USB enclosures often misreport it. So hoardor doesn't guess: the safe default (concurrency 1) is right for an HDD, and the user can raise it for a volume.


## 3. Current state

As built by File Sync v1 (`features/file_sync.md`, branch `abhinavp06/FILE_SCANNER_INIT`, ships in `v0.1.0`).

**Public headers (`include/hoardor/file/`):**

| Header | Contents |
|---|---|
| `settings.hpp` | `FileKind` (Audio=1, Video=2, Text=3, Image=4, Subtitle=5, stored in the database, so never renumbered), `to_string`, `file_kind_from_string`, `Settings` (every tunable, `defaults()`), `kind_of()` |
| `scanner.hpp` | `Scanner` (`open`, `next`, `progress`), `ScannedFile`, `ScanError`, `ScanResult`, `ScanProgress` |
| `library.hpp` | `Library` (settings, categories, roots, sync, paging, `resolve`), `Category`, `Root`, `RootStatus`, `Entry`, `ScanErrorRecord`, `RootSyncOutcome`, `RootSyncReport`, `SyncReport`, `SyncProgress`, `ProgressCallback`, `Error`, `ErrorCode`, `Result` |
| `mount_points.hpp` | `list_mount_points()`, `MountPointLister` |

**Internal (`src/file/`):**
- `text.*`: UTF-8 conversion that never throws, ASCII case-folding, extension normalization.
- `file_time.hpp`: exact mtime ↔ Unix nanoseconds.
- `root_marker.*`: `.hoardor-root` read and write, UUIDs.
- `library_internal.hpp`: helpers shared by `library.cpp` and `library_sync.cpp`.

**Platform backends (`src/file/platform/`, chosen by CMake):**
- `file_info_posix.cpp` and `file_info_windows.cpp`
- `mount_points_linux.cpp`, `mount_points_windows.cpp`, and `mount_points_other.cpp` (empty for now)

**Tables** (migration 1 of component `file`): `file_settings`, `file_categories` (seeded with Music, Movies, Shows, Books), `file_roots`, `file_entries`, and `file_scan_errors`. The full schema is in `features/file_sync.md` §4.3.

**Settings** (`file::Settings`, persisted one row per field in `file_settings`):

| Field | Default |
|---|---|
| `extension_kinds` | see `features/file_sync.md` §3.4 |
| `ignored_names` | Windows, macOS, Linux, and NAS litter, plus `.hoardor-root` |
| `ignored_prefixes` | `._`, `.Trash-` |
| `sync_on_startup` | `false` |
| `settle_window_seconds` | `10` |
| `mass_removal_threshold_percent` | `25` |
| `batch_max_rows` / `batch_max_milliseconds` | `2000` / `50` |
| `progress_interval_files` | `500` |
| `relocation_sample_size` / `relocation_min_match_percent` | `20` / `80` |

**Media library v1, phase 1 (branch `abhinavp06/MEDIA_LISTING`):**
- `Entry::added_ns`: set when a sync first inserts an entry (file migration 2).
- `FileKind::Info` for `.nfo`.
- `Library::companions(entry, parent_levels, limit, prefixes)`: with name prefixes, one index lookup per prefix (2026-10-03).
- Schema details: `DATABASE.md`.

## 4. Roadmap

| Phase | Scope | Delivered by |
|---|---|---|
| 1. Scanning | Streaming scanner, settings | `features/file_sync.md` (v1) |
| 2. Roots, persistence, reconciliation | SQLite, roots, background Sync | `features/file_sync.md` (v1) |
| 3. Moves and renames | Partial fingerprint, move pairing, clock-shift check | A future feature doc |
| 4. Platform volume support | Volume IDs, drive-arrival triggers, per-volume concurrency, macOS | A future feature doc |
| Deferred | Realtime watching | Not planned |

### 4.0 Next feature: Media listing → Media library v1 (`features/media_listing.md`)

**Superseded on 2026-10-02 (the user):** listing by file and folder names was rejected in review. The branch now delivers metadata (new `audio` and `video` engines, ffmpeg) and generic query APIs. For the file engine that means `added_ns`, `FileKind::Info` (`.nfo`), and `companions(entry)`. The outline below is kept as history.

The user approved these proposals on 2026-10-01. The detailed design goes in `features/media_listing.md` on a new branch, after File Sync v1 merges. TYLI's tabs list a category's media across all of its folders.

**Order of work (the user, 2026-10-02):** every media type gets a basic form first, then playback, then metadata:
1. **Media listing** (this section): every category browsable from file and folder names, with no tags yet.
2. **Player, first draft:** audio and video playback (books get a reader later). Where decoding lives (hoardor with ffmpeg, or Qt's media player for a first draft) is decided in its design.
3. **Metadata:** tags, cover art, and chapters, built on the player's decoder where possible, so a separate tag library (e.g. TagLib) may not be needed. Analysis features such as Skip Intro (finding the audio that repeats across episodes) come after that.

This replaces the plan of 2026-10-02 to put tag reading on `abhinavp06/FILE_SCANNER_INIT`.

- **Listed kinds per category:** a new category setting next to the accepted kinds. Music lists audio; Movies and Shows list video; Books lists text. Cover images and subtitles are companions of a file, not list items.
- **Rows** show file facts until tag reading exists: name, folder, size, modified date, kind, which root it belongs to, and whether that root is online. The API is shaped so artist, album, and title can be added later.
- **Sorting:** folder then name (the default: album and season order), name, and date added. Each sort gets an index, so it stays fast at 500k files.
- **Cursor (keyset) paging:** "the next N after this row". It stays fast deep into a list, doesn't skip or repeat rows while a sync runs, and matches Qt's `fetchMore`.
- **Offline roots:** their files are listed and shown greyed out. Listing never touches a drive.
- **A cheap total count per category.**
- **Search by name:** a later phase.

### 4.1 Phase 3: Moves and renames (outline)

- **Fingerprint:** size plus the first and last N KiB (configurable), computed only for files whose path disappeared and a new path with the same size appeared, never for the whole library.
- **Moves:** pair "removed" and "added" files with equal fingerprints as moves, so their ids, play counts, and metadata are kept.
- **Clock shifts:** files with the same size whose mtime moved by an exact number of whole hours (the FAT32 time-zone and DST problem) are checked against their fingerprint before being reported as modified.
- **Relocation:** when a root without a marker is relocated, the sample check can use fingerprints.


### 4.2 Phase 4: Platform volume support (outline)

- **Volume IDs** as the fallback identity for roots without a marker:
  - Linux: `/dev/disk/by-uuid`
  - Windows: `GetVolumeInformationW`
  - macOS: DiskArbitration
- **Volume arrival:** plugging in a drive triggers resolution and an optional sync of its roots. This is the first need for syncing a single root, and it may become a third `sync` overload then. hoardor exposes `check_roots()`. The trigger can come from hoardor's own platform listener, or from TYLI calling `check_roots()` when the OS reports a device change.
- **Per-device concurrency: built 2026-10-02** (on `abhinavp06/MEDIA_LISTING`, at the user's request). `file::device_of` names a path's physical drive, and `master` syncs and reads metadata with one worker per drive, up to `parallel_devices` (default 4). Still open: several workers per SSD/RAID/NAS, and a DiskArbitration backend on macOS.
- **macOS** mount-point listing (`getmntinfo`).


### 4.3 Deferred: realtime watching

Not planned. If it's ever added, it is only a **hint** that triggers a sync of the affected root (`features/file_sync.md` §4). The scan stays the source of truth. It would need:
- native backends: `inotify` on Linux, `ReadDirectoryChangesW` on Windows, `FSEvents` on macOS
- settling and overflow handling
- handling of Windows device-removal notifications, so a watch handle never blocks safe-eject
- probably a `core` event bus

Cost if added: a permanent background thread, about 1 KiB of kernel memory per watched directory on Linux, and three platform backends.


## 5. Relationship to `core`

The file engine needs nothing from `core` in phases 1–4. With on-demand scanning, every result goes back to the caller. Any `core` component it needs later is introduced in the phase that needs it, with that phase as its first consumer.
